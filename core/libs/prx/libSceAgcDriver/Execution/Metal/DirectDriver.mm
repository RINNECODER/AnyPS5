// Fork-only: configures the native Metal driver for the Rosetta direct path. See DirectDriver.hpp.
#include "prx/libSceAgcDriver/Execution/Metal/DirectDriver.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {
namespace {

using Ranges = std::vector<NativeGuestMemory::BorrowedRange>;

// Host-built command words (VideoOut's flip packet) are copied here before submission. The driver
// captures submitted words during the call, so one buffer under a lock is enough.
constexpr std::size_t StagingBytes = 64 * 1024;

struct DirectState {
    DirectState() {
        void* mapping = ::mmap(nullptr, StagingBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapping == MAP_FAILED) throw std::runtime_error("Rosetta direct path: cannot map the command staging buffer");
        staging = static_cast<std::byte*>(mapping);
    }
    std::mutex stagingMutex;
    std::byte* staging = nullptr;

    std::mutex rangesMutex;
    std::uint64_t allocationGeneration = std::numeric_limits<std::uint64_t>::max();
    std::shared_ptr<const Ranges> ranges = std::make_shared<const Ranges>();

    std::mutex driverMutex;
    bool configured = false;
    std::shared_ptr<const Ranges> applied;
    std::uint64_t driverGeneration = 0;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = nil;
};

// Leaked so guest threads still running during static destruction never see it destroyed.
DirectState& State() {
    static auto* state = new DirectState;
    return *state;
}

// The largest power of two that is at most half the device's buffer limit: every borrowed range
// becomes one Metal buffer (a no-copy alias or a copy), and the alignment padding needs headroom.
std::uint64_t MetalRangeLimit() {
    static const std::uint64_t limit = [] {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const std::uint64_t half = device != nil ? device.maxBufferLength / 2 : 0;
        std::uint64_t chunk = std::uint64_t{1} << 28;
        while (chunk <= half / 2) chunk *= 2;
        return chunk;
    }();
    return limit;
}

Ranges IdentityRanges() {
    Ranges result;
    for (const auto& range : GuestAllocations::GuestAllocationsAcquire_nid_postfix()) {
        // Only GPU-mapped memory is GPU-visible, as on the console. CPU-only ranges (the image's
        // code and data, CPU heaps) would be copied or wired for every submission; checks of
        // host-side structures fall back to the host page check below.
        if (range->address == 0 || range->bytes == 0 || !range->gpu) continue;
        if (range->bytes > std::numeric_limits<std::uint64_t>::max() - range->address)
            throw std::runtime_error("Rosetta direct path: guest allocation overflows the address space");
        if (!result.empty()) {
            auto& last = result.back();
            const auto lastEnd = last.guestAddress + last.host.size();
            if (range->address < lastEnd) throw std::runtime_error("Rosetta direct path: guest allocations overlap");
            if (range->address == lastEnd && last.writable == range->writable) {
                last.host = {last.host.data(), last.host.size() + range->bytes};
                continue;
            }
        }
        auto* host = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(range->address));
        result.push_back({range->address, {host, range->bytes}, range->writable});
    }
    result = SplitBorrowedRanges(result, MetalRangeLimit());
    // The staging buffer is a fresh host mapping, so it never overlaps a guest allocation.
    const auto staging = reinterpret_cast<std::uint64_t>(State().staging);
    const auto position = std::lower_bound(result.begin(), result.end(), staging,
        [](const NativeGuestMemory::BorrowedRange& range, std::uint64_t address) { return range.guestAddress < address; });
    result.insert(position, {staging, {State().staging, StagingBytes}, false});
    return result;
}

bool Covered(const Ranges& ranges, std::uint64_t address, std::size_t bytes) {
    const auto found = std::upper_bound(ranges.begin(), ranges.end(), address,
        [](std::uint64_t value, const NativeGuestMemory::BorrowedRange& range) { return value < range.guestAddress; });
    if (found == ranges.begin()) return false;
    const auto& range = *std::prev(found);
    return address - range.guestAddress <= range.host.size() && bytes <= range.host.size() - (address - range.guestAddress);
}

std::shared_ptr<const std::vector<NativeGuestMemory::BorrowedRange>> ProcessRanges() { return DirectGuestRanges(); }

// Guest threads validate their buffers (VideoOut registration, multi-submit arrays) against the
// guest allocations from the start, before anything configures the driver.
__attribute__((constructor)) void InstallProcessRanges() { NativeGuestMemory::SetProcessRangesProvider(&ProcessRanges); }

std::filesystem::path UtilityLibraryPath() {
    if (const char* overridden = std::getenv("ANYPS5_METAL_UTILITY_LIBRARY"); overridden != nullptr && *overridden != '\0')
        return overridden;
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&DirectDriver), &info) == 0 || info.dli_fname == nullptr)
        throw std::runtime_error("Rosetta direct path: cannot locate libSceAgcDriver to find AnyPS5Utilities.metallib");
    return std::filesystem::path(info.dli_fname).parent_path() / "AnyPS5Utilities.metallib";
}

void Configure(DirectState& state, MetalDriver& driver, const std::shared_ptr<const Ranges>& ranges) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) throw std::runtime_error("Rosetta direct path: no Metal device");
        const auto path = UtilityLibraryPath();
        NSError* error = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
        if (library == nil) {
            const std::string reason = error != nil ? error.localizedDescription.UTF8String : "unknown error";
            throw std::runtime_error("Rosetta direct path: cannot load " + path.string() + ": " + reason);
        }
        driver.Configure((__bridge void*)device, (__bridge void*)library, *ranges,
            [](std::uint32_t queue) { AgcDriverDeliverEopInterrupt(queue); }, state.driverGeneration);
        state.device = device;
        state.library = library;
    }
    state.applied = ranges;
    state.configured = true;
}

}

std::vector<NativeGuestMemory::BorrowedRange> SplitBorrowedRanges(const std::vector<NativeGuestMemory::BorrowedRange>& ranges, std::uint64_t limit) {
    if (limit == 0 || (limit & (limit - 1)) != 0) throw std::invalid_argument("Rosetta direct path: range limit must be a power of two");
    std::vector<NativeGuestMemory::BorrowedRange> result;
    result.reserve(ranges.size());
    for (const auto& range : ranges) {
        std::uint64_t offset = 0;
        while (offset < range.host.size()) {
            const auto address = range.guestAddress + offset;
            const auto boundary = (address & ~(limit - 1)) + limit;
            const auto bytes = std::min<std::uint64_t>(range.host.size() - offset, boundary - address);
            result.push_back({address, range.host.subspan(offset, bytes), range.writable});
            offset += bytes;
        }
    }
    return result;
}

std::shared_ptr<const std::vector<NativeGuestMemory::BorrowedRange>> DirectGuestRanges() {
    auto& state = State();
    std::lock_guard lock(state.rangesMutex);
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    if (generation != state.allocationGeneration) {
        // A mutation between the generation read and the snapshot only makes the next call rebuild.
        state.ranges = std::make_shared<const Ranges>(IdentityRanges());
        state.allocationGeneration = generation;
    }
    return state.ranges;
}

void DirectSubmit(const Packet& packet, std::uint32_t queue) {
    auto& driver = DirectDriver();
    const auto address = reinterpret_cast<std::uint64_t>(packet.addr);
    const auto bytes = std::size_t{packet.dw_num} * sizeof(std::uint32_t);
    if (bytes == 0 || Covered(*DirectGuestRanges(), address, bytes)) {
        driver.SubmitCommandBuffer(address, packet.dw_num, packet.flags, queue);
        return;
    }
    if (packet.addr == nullptr || bytes > StagingBytes)
        throw std::runtime_error("Rosetta direct path: command buffer outside guest memory is null or exceeds the staging buffer");
    auto& state = State();
    std::lock_guard lock(state.stagingMutex);
    std::memcpy(state.staging, packet.addr, bytes);
    driver.SubmitCommandBuffer(reinterpret_cast<std::uint64_t>(state.staging), packet.dw_num, packet.flags, queue);
}

MetalDriver& DirectDriver() {
    auto& driver = MetalDriver::Get();
    // Workers reach the entry points only through driver callbacks; they keep the ranges they run with.
    if (MetalDriver::OnDriverWorkerThread()) return driver;
    auto& state = State();
    std::lock_guard lock(state.driverMutex);
    const auto ranges = DirectGuestRanges();
    if (!state.configured) {
        Configure(state, driver, ranges);
    } else if (ranges != state.applied) {
        driver.ReplaceBorrowedRanges(*ranges, ++state.driverGeneration);
        state.applied = ranges;
    }
    return driver;
}

}

namespace {

// Whether every page of [address, address + bytes) is mapped with the needed access.
bool HostAccessible(std::uint64_t address, std::size_t bytes, bool writable) {
    const auto end = address + bytes;
    for (auto cursor = address; cursor < end;) {
        mach_vm_address_t region = cursor;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object) != KERN_SUCCESS)
            return false;
        if (region > cursor || (info.protection & VM_PROT_READ) == 0 || (writable && (info.protection & VM_PROT_WRITE) == 0)) return false;
        cursor = region + size;
    }
    return true;
}

}

// libSceVideoOut validates guest buffers through this export (upstream defines it next to the
// Vulkan driver's guest memory, which this build does not link). Guest pointers are host pointers
// here, so structures outside the guest allocations (on guest thread stacks, which are host thread
// stacks on this path) pass when the process can access them, as upstream's page check does.
extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    try {
        AgcDriver::GuestMemory::CheckRange(pointer, bytes, alignment, writable);
    } catch (const std::out_of_range&) {
        if (!HostAccessible(reinterpret_cast<std::uint64_t>(pointer), bytes, writable)) throw;
    }
}
