#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP

#import <Metal/Metal.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

// A committed command buffer failed, the device could not create a command
// buffer or encoder, or work that already ran on the GPU faulted or could not
// be completed: the GPU or the guest memory it wrote is in doubt. The driver
// keeps this sticky; per-draw and per-dispatch failures before any result is
// copied back to guest memory (unsupported state, shader translation, pipeline
// creation) are skipped instead.
class MetalGpuExecutionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Runs the completion of a committed command buffer (fault readback, copy-back
// to guest memory). The GPU work has executed by then, so any failure is a
// MetalGpuExecutionError rather than a skippable packet failure.
template <typename Work>
decltype(auto) CompleteCommittedWork(Work&& work) {
    try {
        return work();
    } catch (const MetalGpuExecutionError&) {
        throw;
    } catch (const std::exception& error) {
        throw MetalGpuExecutionError(error.what());
    }
}

// Shared-storage buffers recycled by exact length. Creating a Metal buffer is a kernel call
// (IOGPUResourceCreate) and a large one is also zero-filled, and every draw and dispatch used to
// create several (staging for each surface upload and copy-back, the BDA fault buffer and table),
// a large share of the queue worker's time. A recycled buffer holds stale bytes: callers write
// what they read. A buffer goes back with Give once nothing uses it any more, or with
// GiveWhenCompleted once a command buffer that uses it completed (only for buffers the CPU does not
// read afterwards). Kept buffers are bounded in total bytes; the rest are released.
class MetalBufferPool {
public:
    static MetalBufferPool& Shared();
    [[nodiscard]] id<MTLBuffer> Take(id<MTLDevice> device, std::size_t bytes);
    void Give(id<MTLBuffer> buffer);
    void GiveWhenCompleted(id<MTLBuffer> buffer, id<MTLCommandBuffer> commands);

private:
    std::mutex mutex;
    std::map<std::pair<std::uintptr_t, std::size_t>, std::vector<id<MTLBuffer>>> free;
    std::size_t freeBytes = 0;
};

class MetalDevice {
public:
    // `queue`, when given, is shared with another MetalDevice (the driver's draws and dispatches
    // must run on one queue so work committed without waiting stays in order); nil makes a new one.
    explicit MetalDevice(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandQueue> queue = nil);
    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

    [[nodiscard]] id<MTLBuffer> Buffer(std::size_t bytes) const;
    // A buffer of `bytes` (stale contents) that returns to MetalBufferPool once `commands`
    // completed: for staging the GPU fills and reads within `commands` only.
    [[nodiscard]] id<MTLBuffer> TransientBuffer(std::size_t bytes, id<MTLCommandBuffer> commands) const;
    [[nodiscard]] id<MTLCommandBuffer> CommandBuffer() const;
    void Encode(id<MTLCommandBuffer> commands, NSString* kernel, NSArray<id<MTLBuffer>>* buffers,
                const void* parameters, std::size_t parameterBytes, MTLSize grid,
                const std::array<std::uint32_t, 22>* constants = nullptr, std::span<const NSUInteger> offsets = {}) const;
    void Wait(id<MTLCommandBuffer> commands) const;
    [[nodiscard]] id<MTLDevice> Device() const;
    [[nodiscard]] id<MTLCommandQueue> Queue() const;

private:
    struct PipelineKey {
        std::string kernel;
        std::array<std::uint32_t, 22> constants{};
        bool specialized = false;
        auto operator<=>(const PipelineKey&) const = default;
    };

    id<MTLComputePipelineState> pipeline(NSString* kernel,
                                       const std::array<std::uint32_t, 22>* constants) const;
    void validateCommands(id<MTLCommandBuffer> commands) const;

    id<MTLDevice> device;
    id<MTLLibrary> library;
    id<MTLCommandQueue> queue;
    mutable std::mutex pipelineMutex;
    mutable std::map<PipelineKey, id<MTLComputePipelineState>> pipelines;
};

}

#endif
