#include "MetalGuestMemory.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {
namespace Abi = ShaderRecompiler::BdaAbi;

namespace {
constexpr std::size_t addressAlignment = 16;
id<MTLBuffer> sharedBuffer(id<MTLDevice> device, std::size_t bytes) {
    if (bytes == 0 || bytes > device.maxBufferLength) throw std::invalid_argument("Metal guest buffer exceeds the device limit");
    auto buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buffer == nil || buffer.contents == nullptr) throw std::runtime_error("Metal guest buffer allocation failed");
    if (buffer.gpuAddress == 0) throw std::runtime_error("Metal guest buffer has no GPU address");
    return buffer;
}
}

MetalGuestMemory::MetalGuestMemory(id<MTLDevice> device) : device(device) {
    static_assert(std::endian::native == std::endian::little);
    if (device == nil) throw std::invalid_argument("Metal guest memory requires a device");
    if (@available(macOS 13.0, *)) {} else throw std::runtime_error("Metal GPU addresses require macOS 13 or newer");
}

void MetalGuestMemory::RegisterBorrowedHostSpanUntilSnapshotsComplete(std::uint64_t guestAddress, std::span<std::byte> host, bool writable) {
    if (host.empty() || host.data() == nullptr) throw std::invalid_argument("Metal guest span must be nonempty with a valid host pointer");
    if (device.maxBufferLength < addressAlignment || host.size() > device.maxBufferLength - (addressAlignment - 1) || host.size() > std::numeric_limits<std::uint64_t>::max() - guestAddress) {
        throw std::invalid_argument("Metal guest span size or address overflow");
    }
    const auto hostBegin = reinterpret_cast<std::uintptr_t>(host.data());
    if (host.size() > std::numeric_limits<std::uintptr_t>::max() - hostBegin) throw std::invalid_argument("Metal borrowed host span address overflow");
    const auto end = guestAddress + host.size();
    constexpr auto writableLimit = std::uint64_t{std::numeric_limits<std::uint32_t>::max()} << Abi::WrittenPageShift;
    if (writable && end > writableLimit) throw std::invalid_argument("Writable guest address exceeds the written-page ABI");
    std::lock_guard lock(mutex);
    auto position = std::lower_bound(ranges.begin(), ranges.end(), guestAddress, [](const HostRange& range, std::uint64_t address) { return range.begin < address; });
    if ((position != ranges.end() && position->begin < end) ||
        (position != ranges.begin() && std::prev(position)->begin + std::prev(position)->borrowedHost.size() > guestAddress)) {
        throw std::invalid_argument("Metal guest ranges must not overlap");
    }
    if (ranges.size() >= std::numeric_limits<std::uint32_t>::max()) throw std::invalid_argument("Metal guest table count overflow");
    ranges.insert(position, {guestAddress, host, writable});
}

void MetalGuestMemory::Unregister(std::uint64_t guestAddress) {
    std::lock_guard lock(mutex);
    auto found = std::find_if(ranges.begin(), ranges.end(), [guestAddress](const HostRange& range) { return range.begin == guestAddress; });
    if (found == ranges.end()) throw std::invalid_argument("Metal guest range is not registered");
    ranges.erase(found);
}

MetalGuestMemory::DispatchSnapshot MetalGuestMemory::CaptureAfterPriorSnapshotsComplete() const {
    std::lock_guard lock(mutex);
    return DispatchSnapshot(device, ranges);
}

MetalGuestMemory::DispatchSnapshot::DispatchSnapshot(id<MTLDevice> device, const std::vector<HostRange>& ranges) : device(device) {
    if (ranges.size() > (std::numeric_limits<std::size_t>::max() - sizeof(Abi::Header)) / sizeof(Abi::Range)) throw std::invalid_argument("Metal guest table size overflow");
    table = sharedBuffer(device, sizeof(Abi::Header) + ranges.size() * sizeof(Abi::Range));
    fault = sharedBuffer(device, Abi::FaultBufferBytes);
    std::memset(fault.contents, 0, fault.length);
    const Abi::Header header{Abi::Version, static_cast<std::uint32_t>(ranges.size()), sizeof(Abi::Range), 0};
    std::memcpy(table.contents, &header, sizeof(header));
    struct PhysicalComponent {
        std::uintptr_t begin;
        std::uintptr_t end;
        std::uint64_t alignment;
        id<MTLBuffer> buffer;
        std::size_t bufferOffset;
    };
    std::vector<std::size_t> physicalOrder(ranges.size());
    for (std::size_t i = 0; i < ranges.size(); ++i) physicalOrder[i] = i;
    std::sort(physicalOrder.begin(), physicalOrder.end(), [&ranges](std::size_t left, std::size_t right) {
        return reinterpret_cast<std::uintptr_t>(ranges[left].borrowedHost.data()) < reinterpret_cast<std::uintptr_t>(ranges[right].borrowedHost.data());
    });
    std::vector<PhysicalComponent> components;
    std::vector<std::size_t> componentIndices(ranges.size());
    for (const auto i : physicalOrder) {
        const auto& range = ranges[i];
        const auto begin = reinterpret_cast<std::uintptr_t>(range.borrowedHost.data());
        if (range.borrowedHost.size() > std::numeric_limits<std::uintptr_t>::max() - begin) throw std::invalid_argument("Metal borrowed host span address overflow");
        const auto end = begin + range.borrowedHost.size();
        const auto alignment = (range.begin - begin) & (addressAlignment - 1);
        if (components.empty() || begin >= components.back().end) components.push_back({begin, end, alignment, nil, 0});
        else {
            if (alignment != components.back().alignment) throw std::invalid_argument("Metal physically aliased guest spans have incompatible address alignment");
            components.back().end = std::max(components.back().end, end);
        }
        componentIndices[i] = components.size() - 1;
    }
    for (auto& component : components) {
        const auto bytes = component.end - component.begin;
        if (device.maxBufferLength < addressAlignment || bytes > device.maxBufferLength - (addressAlignment - 1)) throw std::invalid_argument("Metal physical guest component exceeds the device limit");
        component.buffer = sharedBuffer(device, bytes + addressAlignment - 1);
        if (component.buffer.gpuAddress > std::numeric_limits<std::uint64_t>::max() - component.buffer.length) throw std::runtime_error("Metal GPU address overflow");
        component.bufferOffset = static_cast<std::size_t>((component.begin + component.alignment - component.buffer.gpuAddress) & (addressAlignment - 1));
        std::memcpy(static_cast<std::byte*>(component.buffer.contents) + component.bufferOffset, reinterpret_cast<const void*>(component.begin), bytes);
    }
    mirrors.reserve(ranges.size());
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        const auto& component = components[componentIndices[i]];
        const auto hostBegin = reinterpret_cast<std::uintptr_t>(range.borrowedHost.data());
        const auto bufferOffset = component.bufferOffset + hostBegin - component.begin;
        const Abi::Range entry{range.begin, range.begin + range.borrowedHost.size(), component.buffer.gpuAddress + bufferOffset, Abi::Read | (range.writable ? Abi::Write : 0), 0};
        std::memcpy(static_cast<std::byte*>(table.contents) + sizeof(header) + mirrors.size() * sizeof(entry), &entry, sizeof(entry));
        mirrors.push_back({range, component.buffer, bufferOffset});
    }
}

id<MTLBuffer> MetalGuestMemory::DispatchSnapshot::Table() const { return table; }
id<MTLBuffer> MetalGuestMemory::DispatchSnapshot::FaultBuffer() const { return fault; }

NSArray<id<MTLBuffer>>* MetalGuestMemory::DispatchSnapshot::Buffers() const {
    NSMutableArray<id<MTLBuffer>>* buffers = [NSMutableArray arrayWithCapacity:mirrors.size()];
    for (const auto& mirror : mirrors) [buffers addObject:mirror.buffer];
    return buffers;
}

void MetalGuestMemory::DispatchSnapshot::DeclareResources(id<MTLComputeCommandEncoder> encoder) const {
    if (completed || encoder == nil || encoder.device != device) throw std::invalid_argument("Metal guest snapshot requires a live encoder on its device");
    [encoder useResource:table usage:MTLResourceUsageRead];
    [encoder useResource:fault usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    for (std::size_t i = 0; i < mirrors.size(); ++i) {
        const auto& mirror = mirrors[i];
        if (std::any_of(mirrors.begin(), mirrors.begin() + i, [&mirror](const Mirror& previous) { return previous.buffer == mirror.buffer; })) continue;
        const bool writable = std::any_of(mirrors.begin(), mirrors.end(), [&mirror](const Mirror& alias) { return alias.buffer == mirror.buffer && alias.range.writable; });
        [encoder useResource:mirror.buffer usage:MTLResourceUsageRead | (writable ? MTLResourceUsageWrite : 0)];
    }
}

Abi::Fault MetalGuestMemory::DispatchSnapshot::CompleteAndCopyDirtyPagesToBorrowedHost(id<MTLCommandBuffer> completedCommands) {
    if (completed || completedCommands == nil || completedCommands.commandQueue.device != device || completedCommands.status != MTLCommandBufferStatusCompleted) {
        throw std::invalid_argument("Metal guest copy-back requires a successful completed dispatch and an unused snapshot");
    }
    Abi::Fault report{};
    std::memcpy(&report, fault.contents, sizeof(report));
    if (report.state == Abi::FaultState::Empty) {
        const Abi::Fault empty{};
        if (std::memcmp(&report, &empty, sizeof(report)) != 0) throw std::runtime_error("Unpublished Metal guest fault record");
    } else if (report.state != Abi::FaultState::Ready || report.reserved != 0 ||
               static_cast<std::uint32_t>(report.reason) < static_cast<std::uint32_t>(Abi::FaultReason::Unmapped) ||
               static_cast<std::uint32_t>(report.reason) > static_cast<std::uint32_t>(Abi::FaultReason::Unaligned)) {
        throw std::runtime_error("Incomplete or invalid Metal guest fault record");
    }
    const auto* words = static_cast<const std::uint32_t*>(fault.contents);
    if (words[Abi::WrittenOverflowWord] != 0) throw std::runtime_error("Metal guest written-page tracker overflow");
    std::set<std::uint64_t> pages;
    for (std::uint32_t i = 0; i < Abi::WrittenPageSlots; ++i) {
        const auto encoded = words[Abi::WrittenSlotsWord + i];
        if (encoded != 0) pages.insert(std::uint64_t{encoded - 1u} << Abi::WrittenPageShift);
    }
    constexpr auto pageBytes = std::uint64_t{1} << Abi::WrittenPageShift;
    for (const auto page : pages) {
        const bool writable = std::any_of(mirrors.begin(), mirrors.end(), [page](const Mirror& mirror) {
            return mirror.range.writable && mirror.range.begin < page + pageBytes && mirror.range.begin + mirror.range.borrowedHost.size() > page;
        });
        if (!writable) throw std::runtime_error("Metal guest dirty page is unmapped or read-only");
    }
    struct CopySegment {
        std::uintptr_t begin;
        std::uintptr_t end;
        id<MTLBuffer> buffer;
        std::size_t bufferOffset;
    };
    std::vector<CopySegment> segments;
    for (const auto& mirror : mirrors) {
        if (!mirror.range.writable) continue;
        const auto end = mirror.range.begin + mirror.range.borrowedHost.size();
        const auto firstPage = mirror.range.begin & ~(pageBytes - 1u);
        for (auto it = pages.lower_bound(firstPage); it != pages.end() && *it < end; ++it) {
            const auto begin = std::max(*it, mirror.range.begin);
            const auto finish = std::min(*it + pageBytes, end);
            const auto offset = static_cast<std::size_t>(begin - mirror.range.begin);
            const auto hostBegin = reinterpret_cast<std::uintptr_t>(mirror.range.borrowedHost.data()) + offset;
            segments.push_back({hostBegin, hostBegin + finish - begin, mirror.buffer, mirror.bufferOffset + offset});
        }
    }
    std::sort(segments.begin(), segments.end(), [](const CopySegment& left, const CopySegment& right) { return left.begin < right.begin; });
    std::vector<CopySegment> merged;
    for (const auto& segment : segments) {
        if (!merged.empty() && segment.buffer == merged.back().buffer && segment.begin <= merged.back().end) merged.back().end = std::max(merged.back().end, segment.end);
        else merged.push_back(segment);
    }
    for (const auto& segment : merged) {
        std::memcpy(reinterpret_cast<void*>(segment.begin), static_cast<const std::byte*>(segment.buffer.contents) + segment.bufferOffset, segment.end - segment.begin);
    }
    completed = true;
    return report;
}

}
