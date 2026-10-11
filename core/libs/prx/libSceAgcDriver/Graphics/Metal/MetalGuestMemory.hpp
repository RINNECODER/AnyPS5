#pragma once

#import <Metal/Metal.h>
#include "BdaAbi.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::Metal {

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

private:
    friend class MetalGuestMemory;
    struct Entry {
        std::uintptr_t begin;
        std::uintptr_t end;
        id<MTLBuffer> buffer;
        std::uint64_t epoch;
    };
    // Under `mutex`: the alias of the page-rounded [begin, end), nil when it cannot alias.
    id<MTLBuffer> alias(std::uintptr_t begin, std::uintptr_t end, std::uint64_t epoch);
    id<MTLDevice> device;
    std::uintptr_t pageBytes;
    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::uint64_t epoch = 0;
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
        DispatchSnapshot& operator=(DispatchSnapshot&&) noexcept = default;

        [[nodiscard]] id<MTLBuffer> Table() const;
        [[nodiscard]] id<MTLBuffer> FaultBuffer() const;
        [[nodiscard]] NSArray<id<MTLBuffer>>* Buffers() const;
        void DeclareResources(id<MTLComputeCommandEncoder> encoder) const;
        // Guest bytes this snapshot copied into fresh buffers (0 when every component aliases).
        [[nodiscard]] std::uint64_t CopiedBytes() const;
        [[nodiscard]] ShaderRecompiler::BdaAbi::Fault CompleteAndCopyDirtyPagesToBorrowedHost(id<MTLCommandBuffer> completedCommands);

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
