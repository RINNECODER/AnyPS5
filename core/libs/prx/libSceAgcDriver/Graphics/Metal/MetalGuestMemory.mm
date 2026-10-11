#include "MetalGuestMemory.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

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

// Whether any page of [begin, end) is mapped from a file (a guest image's segments). Wiring such
// pages for a no-copy buffer broke later execution of the image under Rosetta (SIGBUS without an
// address on a call into the image), so file-backed components are copied instead; they are small.
bool fileBacked(std::uintptr_t begin, std::uintptr_t end) {
    for (auto cursor = begin; cursor < end;) {
        mach_vm_address_t region = cursor;
        mach_vm_size_t size = 0;
        vm_region_extended_info_data_t info{};
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &region, &size, VM_REGION_EXTENDED_INFO, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
            return true;
        if (region >= end) return false;
        if (info.external_pager != 0) return true;
        cursor = region + size;
    }
    return false;
}
}

MetalGuestHostAliases::MetalGuestHostAliases(id<MTLDevice> device) : device(device) {
    if (device == nil) throw std::invalid_argument("Metal guest host aliases require a device");
    const auto page = ::getpagesize();
    if (page <= 0 || (page & (page - 1)) != 0) throw std::runtime_error("Metal guest host aliases require a power-of-two host page");
    pageBytes = static_cast<std::uintptr_t>(page);
}

void MetalGuestHostAliases::Invalidate(const void* host, std::size_t bytes) {
    const auto begin = reinterpret_cast<std::uintptr_t>(host);
    const auto end = bytes > std::numeric_limits<std::uintptr_t>::max() - begin ? std::numeric_limits<std::uintptr_t>::max() : begin + bytes;
    std::lock_guard lock(mutex);
    std::erase_if(entries, [&](const Entry& entry) { return entry.begin < end && begin < entry.end; });
}

void MetalGuestHostAliases::Clear() {
    std::lock_guard lock(mutex);
    entries.clear();
}

std::size_t MetalGuestHostAliases::Count() const {
    std::lock_guard lock(mutex);
    return static_cast<std::size_t>(std::count_if(entries.begin(), entries.end(), [](const Entry& entry) { return entry.buffer != nil; }));
}

id<MTLBuffer> MetalGuestHostAliases::alias(std::uintptr_t begin, std::uintptr_t end, std::uint64_t now) {
    if (end <= begin || end > std::numeric_limits<std::uintptr_t>::max() - (pageBytes - 1)) return nil;
    const auto first = begin & ~(pageBytes - 1);
    const auto last = (end + pageBytes - 1) & ~(pageBytes - 1);
    for (auto& entry : entries) {
        if (entry.begin != first || entry.end != last) continue;
        entry.epoch = now;
        return entry.buffer;
    }
    id<MTLBuffer> buffer = nil;
    if (last - first <= device.maxBufferLength && !fileBacked(first, last)) {
        buffer = [device newBufferWithBytesNoCopy:reinterpret_cast<void*>(first) length:last - first
                                          options:MTLResourceStorageModeShared deallocator:nil];
        if (buffer != nil && (buffer.contents != reinterpret_cast<void*>(first) || buffer.gpuAddress == 0 ||
                                 buffer.gpuAddress > std::numeric_limits<std::uint64_t>::max() - buffer.length)) {
            buffer = nil;
        }
    }
    // A refusal is remembered too, so an unaliasable component is not retried on every capture.
    entries.push_back({first, last, buffer, now});
    return buffer;
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
    constexpr auto writableLimit = Abi::WrittenAddressLimit;
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

MetalGuestMemory::DispatchSnapshot MetalGuestMemory::CaptureAfterPriorSnapshotsComplete(MetalGuestHostAliases* aliases) const {
    std::lock_guard lock(mutex);
    return DispatchSnapshot(device, ranges, aliases);
}

MetalGuestMemory::DispatchSnapshot::DispatchSnapshot(id<MTLDevice> device, const std::vector<HostRange>& ranges,
    MetalGuestHostAliases* aliases) : device(device) {
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
        bool aliased;
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
        if (components.empty() || begin >= components.back().end) components.push_back({begin, end, alignment, nil, 0, false});
        else {
            if (alignment != components.back().alignment) throw std::invalid_argument("Metal physically aliased guest spans have incompatible address alignment");
            components.back().end = std::max(components.back().end, end);
        }
        componentIndices[i] = components.size() - 1;
    }
    std::unique_lock<std::mutex> aliasLock;
    std::uint64_t aliasEpoch = 0;
    if (aliases != nullptr) {
        if (aliases->device != device) throw std::invalid_argument("Metal guest host aliases belong to another device");
        aliasLock = std::unique_lock(aliases->mutex);
        aliasEpoch = ++aliases->epoch;
    }
    for (auto& component : components) {
        const auto bytes = component.end - component.begin;
        // A no-copy alias starts on a host page, so its GPU address keeps the host address's
        // low bits: it can stand in for a copy only when guest and host agree modulo 16.
        if (aliases != nullptr && component.alignment == 0) {
            if (id<MTLBuffer> alias = aliases->alias(component.begin, component.end, aliasEpoch)) {
                const auto offset = static_cast<std::size_t>(component.begin - reinterpret_cast<std::uintptr_t>(alias.contents));
                if (((alias.gpuAddress + offset - component.begin) & (addressAlignment - 1)) == 0 && offset + bytes <= alias.length) {
                    component.buffer = alias;
                    component.bufferOffset = offset;
                    component.aliased = true;
                    continue;
                }
            }
        }
        if (device.maxBufferLength < addressAlignment || bytes > device.maxBufferLength - (addressAlignment - 1)) throw std::invalid_argument("Metal physical guest component exceeds the device limit");
        component.buffer = sharedBuffer(device, bytes + addressAlignment - 1);
        if (component.buffer.gpuAddress > std::numeric_limits<std::uint64_t>::max() - component.buffer.length) throw std::runtime_error("Metal GPU address overflow");
        component.bufferOffset = static_cast<std::size_t>((component.begin + component.alignment - component.buffer.gpuAddress) & (addressAlignment - 1));
        std::memcpy(static_cast<std::byte*>(component.buffer.contents) + component.bufferOffset, reinterpret_cast<const void*>(component.begin), bytes);
        copiedBytes += bytes;
    }
    if (aliases != nullptr) {
        // Every capture names the whole borrowed set, so an alias it did not use belongs to a
        // superseded layout of the host components.
        std::erase_if(aliases->entries, [aliasEpoch](const MetalGuestHostAliases::Entry& entry) { return entry.epoch != aliasEpoch; });
        aliasLock.unlock();
    }
    mirrors.reserve(ranges.size());
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto& range = ranges[i];
        const auto& component = components[componentIndices[i]];
        const auto hostBegin = reinterpret_cast<std::uintptr_t>(range.borrowedHost.data());
        const auto bufferOffset = component.bufferOffset + hostBegin - component.begin;
        const Abi::Range entry{range.begin, range.begin + range.borrowedHost.size(), component.buffer.gpuAddress + bufferOffset, Abi::Read | (range.writable ? Abi::Write : 0), 0};
        std::memcpy(static_cast<std::byte*>(table.contents) + sizeof(header) + mirrors.size() * sizeof(entry), &entry, sizeof(entry));
        mirrors.push_back({range, component.buffer, bufferOffset, component.aliased});
    }
}

id<MTLBuffer> MetalGuestMemory::DispatchSnapshot::Table() const { return table; }
id<MTLBuffer> MetalGuestMemory::DispatchSnapshot::FaultBuffer() const { return fault; }
std::uint64_t MetalGuestMemory::DispatchSnapshot::CopiedBytes() const { return copiedBytes; }

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
    constexpr auto pageBytes = std::uint64_t{1} << Abi::WrittenPageShift;
    if (!Abi::ForEachWrittenRange(words, [&](std::uint64_t begin, std::size_t bytes) {
            for (std::uint64_t offset = 0; offset < bytes; offset += pageBytes) pages.insert(begin + offset);
        })) {
        throw std::runtime_error("Metal guest written-page tracker is incomplete");
    }
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
        // An aliased mirror is the borrowed host memory itself: the GPU already wrote it.
        if (!mirror.range.writable || mirror.aliased) continue;
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
