#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP

#import <Metal/Metal.h>

#include <array>
#include <atomic>
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

class MetalDevice;

// Held by a work between taking a command buffer and handing it to a wait or the timeline. With
// batching, a work that stops in between (a throw that skips it, an early return) leaves what it
// already encoded in the shared command buffer, which still runs; before, its own command buffer
// was simply dropped. The pooled and transient memory that work binds (its fault buffer, staged
// guest bytes) goes back when the work is destroyed, so the guard waits for the batch first and
// nothing is reused while the GPU may still touch it. Done() marks the hand-off.
class MetalBatchedWork {
public:
    MetalBatchedWork(const MetalDevice& backend, id<MTLCommandBuffer> commands) : backend(backend), commands(commands) {}
    MetalBatchedWork(const MetalBatchedWork&) = delete;
    MetalBatchedWork& operator=(const MetalBatchedWork&) = delete;
    ~MetalBatchedWork();
    void Done() { done = true; }

private:
    const MetalDevice& backend;
    id<MTLCommandBuffer> commands;
    bool done = false;
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
    // Commits `commands` (when not committed yet) and waits for it; the open batch commits first.
    void Wait(id<MTLCommandBuffer> commands) const;

    // Command buffer batching, for the driver's packet workers (off by default).
    //
    // Metal charges every command buffer a fixed CPU cost (FramePacing bookkeeping on create and
    // commit, an IOGPU submit), which dominated the driver thread when each draw, dispatch and
    // upload had its own. While batching is on, CommandBuffer() hands every caller the same open
    // command buffer until it is committed: by Flush, by Wait on it, or by any caller's own commit
    // (the next CommandBuffer() then opens a new one). Encoders in one command buffer run in
    // encoding order and Metal tracks hazards between them exactly as between command buffers on
    // one queue, so the GPU runs the same work in the same order as before.
    //
    // Not thread-safe: the owner serializes every use of the device while batching (the driver
    // holds its gpuMutex), and only one encoder is ever open at a time.
    void SetBatching(bool on);
    [[nodiscard]] bool Batching() const { return batching; }
    // Whether `commands` is the open batch (handed out, not committed yet).
    [[nodiscard]] bool OpenBatch(id<MTLCommandBuffer> commands) const;
    // Commits the open batch, if any, without waiting.
    void Flush() const;
    // Commits `commands` unless it is the open batch, which commits at the next Flush or Wait.
    void Submit(id<MTLCommandBuffer> commands) const;
    // Whether the open batch holds at least `minimum` works and the batch committed before it has
    // completed: committing now keeps the GPU busy while the CPU encodes on.
    [[nodiscard]] bool GpuStarved(std::uint32_t minimum) const;
    // Works handed the open batch so far (0 without one).
    [[nodiscard]] std::uint32_t OpenUses() const { return open != nil && open.status < MTLCommandBufferStatusCommitted ? openUses : 0; }
    // Metal command buffers this device created.
    [[nodiscard]] std::uint64_t CommandBuffersCreated() const { return created.load(std::memory_order_relaxed); }

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
    bool batching = false;
    mutable id<MTLCommandBuffer> open;
    mutable id<MTLCommandBuffer> flushed;
    mutable std::uint32_t openUses = 0;
    mutable std::atomic<std::uint64_t> created{0};
};

}

#endif
