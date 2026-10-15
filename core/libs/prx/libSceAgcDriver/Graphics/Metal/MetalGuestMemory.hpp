#pragma once

#import <Metal/Metal.h>
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

// A binding needed guest bytes outside the host-alias working set; they were admitted, so the
// work can be set up again (nothing reached the GPU yet).
class MetalWorkingSetMiss final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// #291: persistent shared Metal buffers that alias the page-aligned host backing of guest
// mappings (newBufferWithBytesNoCopy), so a snapshot binds guest memory without copying it.
// One buffer per physically contiguous host component; a capture keeps the components it used
// and drops the rest, and mapping publications drop the aliases of the host spans they change
// (the alias would otherwise keep the old pages). A component that cannot alias (unaligned,
// over the device limit, or refused by Metal) falls back to the snapshot copy.
class MetalGuestHostAliases {
public:
    explicit MetalGuestHostAliases(id<MTLDevice> device);
    MetalGuestHostAliases(const MetalGuestHostAliases&) = delete;
    MetalGuestHostAliases& operator=(const MetalGuestHostAliases&) = delete;
    // Drops every alias whose host pages overlap [host, host + bytes).
    void Invalidate(const void* host, std::size_t bytes);
    void Clear();
    [[nodiscard]] std::size_t Count() const;

    // Working-set mode (Rosetta direct path). A command buffer that uses a no-copy alias wires every
    // host page under it while the buffer is in flight, so one alias of a title's whole GPU mapping
    // (13.5 GiB) kept it all wired. In this mode a capture binds only the host pages in the working
    // set: the union of `granule`-aligned host intervals admitted so far, one alias per interval.
    // Guest bytes outside it are absent from the capture's table: CPU-side binding raises
    // MetalWorkingSetMiss and a GPU access reports an Unmapped fault; callers admit the bytes and
    // run the work again. When admitting would take the set over `budget` bytes it starts empty.
    void EnableWorkingSet(std::uint64_t granule, std::uint64_t budget);
    // Working-set mode for every MetalGuestHostAliases constructed afterwards (0 granule: off).
    static void SetProcessWorkingSet(std::uint64_t granule, std::uint64_t budget);
    [[nodiscard]] bool WorkingSetEnabled() const;
    // Admits the host pages behind guest [address, address + bytes) in `ranges`. True when that
    // added pages, false when they were all present already or no range holds the bytes.
    bool AdmitGuest(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t address, std::uint64_t bytes);
    // Host bytes in the working set.
    [[nodiscard]] std::uint64_t WorkingSetBytes() const;
    // Disjoint, non-adjacent host intervals of the working set, in address order.
    [[nodiscard]] std::vector<std::pair<std::uintptr_t, std::uintptr_t>> WorkingSet() const;
    // Bridges made so far that are still cached (tests).
    [[nodiscard]] std::size_t BridgeCount() const;

private:
    friend class MetalGuestMemory;
    bool admitHost(std::uintptr_t begin, std::uintptr_t end);
    std::uint64_t granule = 0;
    std::uint64_t budget = 0;
    std::map<std::uintptr_t, std::uintptr_t> intervals;
    std::uint64_t intervalBytes = 0;
    std::uint64_t admissions = 0;
    std::uint64_t resets = 0;
    struct Entry {
        std::uintptr_t begin;
        std::uintptr_t end;
        id<MTLBuffer> buffer;
        std::uint64_t epoch;
    };
    // Under `mutex`: the alias of the page-rounded [begin, end), nil when it cannot alias.
    id<MTLBuffer> alias(std::uintptr_t begin, std::uintptr_t end, std::uint64_t epoch);
    // A no-copy buffer over host pages [first, last), nil when Metal cannot alias them.
    id<MTLBuffer> newAlias(std::uintptr_t first, std::uintptr_t last) const;
    // One alias over host [begin, end) where a binding spans several aliased components (adjacent
    // guest mappings): kept across captures, up to MaxBridges, until its pages change.
    id<MTLBuffer> bridge(std::uintptr_t begin, std::uintptr_t end);
    static constexpr std::size_t MaxBridges = 32;
    id<MTLDevice> device;
    std::uintptr_t pageBytes;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::vector<Entry> bridges;
    std::uint64_t bridgeUses = 0;
    std::uint64_t epoch = 0;
    // The last BDA table a capture built, and its entries (DispatchSnapshot reuses it unchanged).
    id<MTLBuffer> table = nil;
    std::vector<ShaderRecompiler::BdaAbi::Range> tableEntries;
};

class MetalGuestMemory {
    struct HostRange {
        std::uint64_t begin;
        std::span<std::byte> borrowedHost;
        bool writable;
    };

public:
    class DispatchSnapshot {
        friend class MetalGuestMemory;
        struct Mirror {
            HostRange range;
            id<MTLBuffer> buffer;
            std::size_t bufferOffset;
            bool aliased;
        };
        explicit DispatchSnapshot(id<MTLDevice> device, const std::vector<HostRange>& ranges, MetalGuestHostAliases* aliases);

    public:
        DispatchSnapshot(const DispatchSnapshot&) = delete;
        DispatchSnapshot& operator=(const DispatchSnapshot&) = delete;
        DispatchSnapshot(DispatchSnapshot&&) noexcept = default;
        DispatchSnapshot& operator=(DispatchSnapshot&&) noexcept = delete;
        // Gives the fault buffer back to MetalBufferPool: a snapshot outlives its GPU work.
        ~DispatchSnapshot();

        [[nodiscard]] id<MTLBuffer> Table() const;
        [[nodiscard]] id<MTLBuffer> FaultBuffer() const;
        [[nodiscard]] NSArray<id<MTLBuffer>>* Buffers() const;
        // Buffers()[index] and Buffers().count without building the array (a draw looks up a
        // mirror for every buffer and texture it binds).
        [[nodiscard]] id<MTLBuffer> Buffer(std::size_t index) const { return mirrors.at(index).buffer; }
        [[nodiscard]] std::size_t BufferCount() const { return mirrors.size(); }
        void DeclareResources(id<MTLComputeCommandEncoder> encoder) const;
        // Guest bytes this snapshot copied into fresh buffers (0 when every component aliases).
        [[nodiscard]] std::uint64_t CopiedBytes() const;
        [[nodiscard]] ShaderRecompiler::BdaAbi::Fault CompleteAndCopyDirtyPagesToBorrowedHost(id<MTLCommandBuffer> completedCommands);
        // Guest [address, address + bytes) as one buffer and offset when it spans several mirrors
        // (adjacent guest mappings) that alias consecutive host bytes: a bridge alias of `aliases`
        // over the same host pages, so nothing is copied. Empty when it cannot be bridged.
        [[nodiscard]] std::optional<std::pair<id<MTLBuffer>, std::size_t>> Bridge(std::uint64_t address, std::uint64_t bytes,
            MetalGuestHostAliases* aliases) const;

    private:
        id<MTLDevice> device;
        id<MTLBuffer> table;
        id<MTLBuffer> fault;
        std::vector<Mirror> mirrors;
        std::uint64_t copiedBytes = 0;
        bool completed = false;
    };

    explicit MetalGuestMemory(id<MTLDevice> device);
    MetalGuestMemory(const MetalGuestMemory&) = delete;
    MetalGuestMemory& operator=(const MetalGuestMemory&) = delete;
    void RegisterBorrowedHostSpanUntilSnapshotsComplete(std::uint64_t guestAddress, std::span<std::byte> host, bool writable);
    void Unregister(std::uint64_t guestAddress);
    // With `aliases`, components bind persistent no-copy aliases of the host backing instead of copies.
    [[nodiscard]] DispatchSnapshot CaptureAfterPriorSnapshotsComplete(MetalGuestHostAliases* aliases = nullptr) const;

private:
    id<MTLDevice> device;
    mutable std::mutex mutex;
    std::vector<HostRange> ranges;
};

}
