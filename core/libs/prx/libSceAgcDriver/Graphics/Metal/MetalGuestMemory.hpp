#pragma once

#import <Metal/Metal.h>
#include "BdaAbi.hpp"
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace AgcDriver::Metal {

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
        };
        explicit DispatchSnapshot(id<MTLDevice> device, const std::vector<HostRange>& ranges);

    public:
        DispatchSnapshot(const DispatchSnapshot&) = delete;
        DispatchSnapshot& operator=(const DispatchSnapshot&) = delete;
        DispatchSnapshot(DispatchSnapshot&&) noexcept = default;
        DispatchSnapshot& operator=(DispatchSnapshot&&) noexcept = default;

        [[nodiscard]] id<MTLBuffer> Table() const;
        [[nodiscard]] id<MTLBuffer> FaultBuffer() const;
        [[nodiscard]] NSArray<id<MTLBuffer>>* Buffers() const;
        void DeclareResources(id<MTLComputeCommandEncoder> encoder) const;
        [[nodiscard]] ShaderRecompiler::BdaAbi::Fault CompleteAndCopyDirtyPagesToBorrowedHost(id<MTLCommandBuffer> completedCommands);

    private:
        id<MTLDevice> device;
        id<MTLBuffer> table;
        id<MTLBuffer> fault;
        std::vector<Mirror> mirrors;
        bool completed = false;
    };

    explicit MetalGuestMemory(id<MTLDevice> device);
    MetalGuestMemory(const MetalGuestMemory&) = delete;
    MetalGuestMemory& operator=(const MetalGuestMemory&) = delete;
    void RegisterBorrowedHostSpanUntilSnapshotsComplete(std::uint64_t guestAddress, std::span<std::byte> host, bool writable);
    void Unregister(std::uint64_t guestAddress);
    [[nodiscard]] DispatchSnapshot CaptureAfterPriorSnapshotsComplete() const;

private:
    id<MTLDevice> device;
    mutable std::mutex mutex;
    std::vector<HostRange> ranges;
};

}
