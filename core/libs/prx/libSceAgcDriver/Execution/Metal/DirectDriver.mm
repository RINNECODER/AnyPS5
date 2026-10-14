// Fork-only: configures the native Metal driver for the Rosetta direct path. See DirectDriver.hpp.
#include "prx/libSceAgcDriver/Execution/Metal/DirectDriver.hpp"
#include "prx/libSceAgcDriver/Eq/include/Event.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalGuestMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <optional>
#include <string>
#include <thread>

namespace {

// Whether every page of [address, address + bytes) is mapped with the needed access.
bool HostAccessible(std::uint64_t address, std::size_t bytes, bool writable) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
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

namespace AgcDriver::Metal {
namespace {

using Ranges = std::vector<NativeGuestMemory::BorrowedRange>;

// Command words outside the GPU-visible guest allocations (VideoOut's flip packet, a DCB in CPU-only
// or host memory) are copied here before submission. The driver captures submitted words during
// the call, so one buffer under a lock is enough. It starts at StagingBytes and grows to the
// largest such buffer, up to MaxStagingBytes (the capture's own limit is larger).
constexpr std::size_t StagingBytes = 64 * 1024;
constexpr std::size_t MaxStagingBytes = 64 * 1024 * 1024;

std::byte* MapStaging(std::size_t bytes) {
    void* mapping = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) throw std::runtime_error("Rosetta direct path: cannot map the command staging buffer");
    return static_cast<std::byte*>(mapping);
}

struct DirectState {
    // Held while the staging buffer is filled and submitted.
    std::mutex stagingMutex;
    // Guards staging/stagingBytes for readers outside stagingMutex (IdentityRanges); never held
    // while taking another lock.
    std::mutex stagingRegionMutex;
    std::byte* staging = MapStaging(StagingBytes);
    std::size_t stagingBytes = StagingBytes;

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
    std::span<std::byte> staging;
    {
        auto& state = State();
        std::lock_guard lock(state.stagingRegionMutex);
        staging = {state.staging, state.stagingBytes};
    }
    const auto stagingAddress = reinterpret_cast<std::uint64_t>(staging.data());
    const auto position = std::lower_bound(result.begin(), result.end(), stagingAddress,
        [](const NativeGuestMemory::BorrowedRange& range, std::uint64_t address) { return range.guestAddress < address; });
    result.insert(position, {stagingAddress, staging, false});
    return result;
}

// Replaces the staging buffer with one of at least `bytes` (caller holds stagingMutex) and makes
// the next DirectGuestRanges() rebuild, so the next DirectDriver() hands the driver the new range.
// The old mapping stays mapped: ranges already handed out (a worker's snapshot) may still name it.
void GrowStaging(DirectState& state, std::size_t bytes) {
    auto size = state.stagingBytes;
    while (size < bytes) size *= 2;
    auto* mapping = MapStaging(size);
    {
        std::lock_guard lock(state.stagingRegionMutex);
        state.staging = mapping;
        state.stagingBytes = size;
    }
    std::lock_guard lock(state.rangesMutex);
    state.allocationGeneration = std::numeric_limits<std::uint64_t>::max();
}

// A submission whose words cannot be read or parsed is dropped with a log line instead of aborting
// the title. ANYPS5_AGC_STRICT_SUBMISSIONS=1 keeps the abort (to catch a driver bug in the act).
bool StrictSubmissions() {
    static const bool strict = [] {
        const char* value = std::getenv("ANYPS5_AGC_STRICT_SUBMISSIONS");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    return strict;
}

void SkipSubmission(const char* call, const Packet& packet, std::uint32_t queue, bool staged, const std::string& reason) {
    static std::atomic<std::uint64_t> skipped{0};
    const auto count = ++skipped;
    if (count > 16 && (count & (count - 1)) != 0) return;
    std::fprintf(stderr, "[rosetta] %s: skipped the command buffer at %p (%u words, %zu bytes, flags 0x%x, queue %u, %s): %s (skip %llu)\n",
        call != nullptr ? call : "submit", packet.addr, packet.dw_num, std::size_t{packet.dw_num} * sizeof(std::uint32_t),
        static_cast<unsigned>(packet.flags), queue, staged ? "outside guest memory, staged" : "in guest memory", reason.c_str(),
        static_cast<unsigned long long>(count));
}

// Staged words are a copy, so what the driver would otherwise read from the title's memory later
// must be settled when the copy is taken (#472):
// - a title may still be writing the last packets of a buffer it submits (the driver re-reads a
//   rejected buffer for a while, but re-reading a copy never sees the late words), so the copy is
//   retaken until its packets parse, for up to RestageMs;
// - the driver reads a REWIND's tail and its release word later, from the address it captured, which
//   for a staged buffer is the staging buffer: by then the next staged submission has overwritten
//   it (or a grow replaced it), and the tail parsed garbage ("unsupported PM4 packet type"). The
//   title releases the tail in its own copy, which the driver cannot see, so such a buffer is skipped.
constexpr int RestageMs = 20;

struct StagingProblem {
    std::size_t dword;
    std::uint32_t header;
    const char* what;
    bool rewind;
};

std::optional<StagingProblem> FirstStagingProblem(std::span<const std::uint32_t> words) {
    for (std::size_t cursor = 0; cursor < words.size();) {
        const auto header = words[cursor];
        if (Pm4::FillerPacket(header)) { ++cursor; continue; }
        if ((header & 0xc0000000u) != 0xc0000000u) return StagingProblem{cursor, header, "unsupported PM4 packet type", false};
        const auto count = Pm4::PacketWords(header);
        if (count > words.size() - cursor) return StagingProblem{cursor, header, "PM4 packet runs past the end of the command buffer", false};
        if (((header >> 8u) & 0xffu) == 0x59u) return StagingProblem{cursor, header, "a REWIND outside guest memory cannot be followed from a staged copy", true};
        cursor += count;
    }
    return std::nullopt;
}

std::string Describe(const StagingProblem& problem, std::size_t words) {
    char text[192];
    std::snprintf(text, sizeof(text), "%s: header 0x%08x at DWORD %zu of %zu", problem.what, problem.header, problem.dword, words);
    return text;
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

// Guest GPU memory is bound to Metal per working-set interval (MetalGuestHostAliases::EnableWorkingSet):
// a command buffer wires the host pages of every no-copy alias it uses, so binding a title's whole
// GPU mapping wired all of it (13.5 GiB for the reference title). The budget caps the wired set;
// ANYPS5_METAL_WORKING_SET_MIB overrides it (0 binds the whole mapping, as before).
constexpr std::uint64_t WorkingSetGranule = std::uint64_t{4} << 20;
constexpr std::uint64_t DefaultWorkingSetBudget = std::uint64_t{3} << 30;

std::uint64_t WorkingSetBudget() {
    const char* value = std::getenv("ANYPS5_METAL_WORKING_SET_MIB");
    if (value == nullptr || *value == '\0') return DefaultWorkingSetBudget;
    char* end = nullptr;
    const auto mebibytes = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0' || mebibytes > (std::numeric_limits<std::uint64_t>::max() >> 20))
        throw std::invalid_argument("Rosetta direct path: ANYPS5_METAL_WORKING_SET_MIB must be a number of MiB");
    if (mebibytes == 0) return 0;
    return std::max(mebibytes << 20, WorkingSetGranule);
}

void Configure(DirectState& state, MetalDriver& driver, const std::shared_ptr<const Ranges>& ranges) {
    const auto budget = WorkingSetBudget();
    MetalGuestHostAliases::SetProcessWorkingSet(budget == 0 ? 0 : WorkingSetGranule, budget);
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
        driver.SetSkipInvalidCommandBuffers(!StrictSubmissions());
        state.device = device;
        state.library = library;
    }
    state.applied = ranges;
    state.configured = true;
}

}

bool RangesCover(const std::vector<NativeGuestMemory::BorrowedRange>& ranges, std::uint64_t address, std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - address) return false;
    const auto end = address + bytes;
    auto found = std::upper_bound(ranges.begin(), ranges.end(), address,
        [](std::uint64_t value, const NativeGuestMemory::BorrowedRange& range) { return value < range.guestAddress; });
    if (found == ranges.begin()) return false;
    --found;
    // A buffer may run on through adjacent ranges: mappings with different access are never merged,
    // and long ones are split at the buffer limit, but the driver reads across both.
    for (auto cursor = address;;) {
        const auto rangeEnd = found->guestAddress + found->host.size();
        if (cursor > rangeEnd) return false;
        if (end <= rangeEnd) return true;
        cursor = rangeEnd;
        if (++found == ranges.end() || found->guestAddress != cursor) return false;
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

void DirectSubmit(const Packet& packet, std::uint32_t queue, const char* call) {
    auto* driver = &DirectDriver();
    const auto address = reinterpret_cast<std::uint64_t>(packet.addr);
    const auto bytes = std::size_t{packet.dw_num} * sizeof(std::uint32_t);
    const auto submit = [&](std::uint64_t commands, bool staged) {
        try {
            driver->SubmitCommandBuffer(commands, packet.dw_num, packet.flags, queue);
        } catch (const InvalidCommandBuffer& error) {
            if (StrictSubmissions()) throw;
            SkipSubmission(call, packet, queue, staged, error.what());
        }
    };
    if (bytes == 0 || RangesCover(*DirectGuestRanges(), address, bytes)) return submit(address, false);
    if (packet.addr == nullptr) return SkipSubmission(call, packet, queue, true, "null address");
    if (bytes > MaxStagingBytes) return SkipSubmission(call, packet, queue, true, "larger than the staging limit");
    if (!HostAccessible(address, bytes, false)) return SkipSubmission(call, packet, queue, true, "not readable memory");
    auto& state = State();
    std::lock_guard lock(state.stagingMutex);
    if (bytes > state.stagingBytes) {
        // A worker keeps the ranges it runs with, so it cannot hand the driver a new staging range.
        if (MetalDriver::OnDriverWorkerThread())
            return SkipSubmission(call, packet, queue, true, "needs a larger staging buffer on a driver worker thread");
        GrowStaging(state, bytes);
        driver = &DirectDriver();
    }
    const std::span<const std::uint32_t> staged(reinterpret_cast<const std::uint32_t*>(state.staging), packet.dw_num);
    std::memcpy(state.staging, packet.addr, bytes);
    auto problem = FirstStagingProblem(staged);
    for (int waited = 1; problem && !problem->rewind && waited <= RestageMs; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::memcpy(state.staging, packet.addr, bytes);
        problem = FirstStagingProblem(staged);
    }
    if (problem) {
        if (StrictSubmissions() && !problem->rewind) throw std::runtime_error("AGC driver: " + Describe(*problem, packet.dw_num));
        return SkipSubmission(call, packet, queue, true, Describe(*problem, packet.dw_num));
    }
    submit(reinterpret_cast<std::uint64_t>(state.staging), true);
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

// VideoOut publishes its buffer labels through this export. Guest memory is host memory on this path
// and there is no write watch to notify, so the store is a checked release store.
extern "C" void AgcDriverStoreGuestLabel_nid_postfix(std::uint64_t* pointer, std::uint64_t value) {
    AgcDriverCheckGuestMemory_nid_postfix(pointer, sizeof(*pointer), std::atomic_ref<std::uint64_t>::required_alignment, true);
    std::atomic_ref(*pointer).store(value, std::memory_order_release);
}
