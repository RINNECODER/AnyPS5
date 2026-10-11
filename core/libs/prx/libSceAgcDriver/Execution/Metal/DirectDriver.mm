// Fork-only: configures the native Metal driver for the Rosetta direct path. See DirectDriver.hpp.
#include "prx/libSceAgcDriver/Execution/Metal/DirectDriver.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
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

Ranges IdentityRanges() {
    Ranges result;
    for (const auto& range : GuestAllocations::GuestAllocationsAcquire_nid_postfix()) {
        if (range->address == 0 || range->bytes == 0) continue;
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

// libSceVideoOut validates guest buffers through this export (upstream defines it next to the
// Vulkan driver's guest memory, which this build does not link).
extern "C" void AgcDriverCheckGuestMemory_nid_postfix(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    AgcDriver::GuestMemory::CheckRange(pointer, bytes, alignment, writable);
}
