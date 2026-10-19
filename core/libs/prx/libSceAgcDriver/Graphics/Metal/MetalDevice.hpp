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

// Render-pass merging and encoder bookkeeping.
//
// On Apple GPUs every render pass loads and stores its attachments through tile memory and costs
// 10-16 us of fixed GPU time; the live map drew about 1,170 passes per frame, most of them one
// draw each (375 shadow draws into one depth target). A draw may therefore leave its render
// encoder open ("parked") instead of ending it, and the next draw into the same attachments of the
// same batched command buffer encodes into it: one pass instead of one per draw.
//
// Only one encoder may be open in a command buffer, so everything that encodes into one ends the
// encoder parked in it first: every encoder the driver creates comes from the functions below,
// and MetalDevice ends it before a commit (Flush, Wait, Submit). Anything else that encodes into
// a batched command buffer (an event signal) calls EndParked itself.
namespace MetalEncoders {

// The attachments and layout of a render pass, compared to decide whether a draw may continue
// the parked pass.
struct PassAttachment {
    std::uintptr_t texture = 0;
    std::uintptr_t resolve = 0;
    // The textures `texture` and `resolve` are views of, or themselves.
    std::uintptr_t textureRoot = 0;
    std::uintptr_t resolveRoot = 0;
    NSUInteger level = 0, slice = 0, plane = 0;
    NSUInteger resolveLevel = 0, resolveSlice = 0, resolvePlane = 0;
    MTLLoadAction load = MTLLoadActionDontCare;
    MTLStoreAction store = MTLStoreActionDontCare;
};

struct PassKey {
    NSUInteger width = 0, height = 0, layers = 0, samples = 0;
    NSUInteger depthResolveFilter = 0, stencilResolveFilter = 0;
    std::uintptr_t visibility = 0;
    // Eight colors, then depth and stencil.
    std::array<PassAttachment, 10> attachments{};
    NSUInteger positions = 0;
    std::array<MTLSamplePosition, 16> samplePositions{};
};

[[nodiscard]] PassKey KeyOf(MTLRenderPassDescriptor* pass);
// Whether a pass described by `next` may continue the open pass `open`: the same attachments
// (texture, level, slice, plane and resolve target), size, layers, samples, sample positions and
// store actions, no visibility buffer, and every attachment of `next` loads (a clear or a
// don't-care load needs a pass of its own).
[[nodiscard]] bool Continues(const PassKey& open, const PassKey& next);
// Whether `resource` is a texture that is, or is a view of the same texture as, an attachment or
// resolve target of the pass `key` describes. A draw that samples an attachment reads what
// earlier draws of the pass wrote, which Metal orders between passes but not between the draws of
// one pass.
[[nodiscard]] bool IsAttachment(const PassKey& key, id<MTLResource> resource);
// Whether `a` and `b` describe the same attachments at the same level, slice and plane, with the
// same resolve targets and store actions, at the same size, layers, samples and sample positions:
// what a reused draw translation record's pass must still be for its attachments to stand. Views
// of one texture differ between builds, so textures compare by their root (IsAttachment).
[[nodiscard]] bool SamePass(const PassKey& a, const PassKey& b);
// The texture a view stands on: the end of its chain of parent textures. Two views built at
// different times of the same surface compare by this.
[[nodiscard]] id<MTLTexture> RootTexture(id<MTLTexture> texture);

// Upload batching: linear buffer -> texture blit copies (e.g. BC texture uploads after detiling)
// can be grouped to share a single MTLBlitCommandEncoder per command buffer rather than creating
// an encoder per texture.
struct UploadBlitStep {
    NSUInteger sourceOffset = 0;
    NSUInteger sourceBytesPerRow = 0;
    NSUInteger sourceBytesPerImage = 0;
    MTLSize sourceSize{};
    NSUInteger destinationSlice = 0;
    NSUInteger destinationLevel = 0;
    MTLOrigin destinationOrigin{};
};

struct UploadBlitRecord {
    id<MTLBuffer> sourceBuffer = nil;
    id<MTLTexture> destinationTexture = nil;
    std::vector<UploadBlitStep> steps;
};

// Enqueues an upload blit record to be encoded into the shared blit encoder for `commands`.
void EnqueueUploadBlit(id<MTLCommandBuffer> commands, UploadBlitRecord record);
// Encodes all pending upload blits for `commands` into a single MTLBlitCommandEncoder.
void FlushUploadBlits(id<MTLCommandBuffer> commands);
// Whether there are pending upload blits for `commands`.
[[nodiscard]] bool HasPendingUploadBlits(id<MTLCommandBuffer> commands);

// Ends the render encoder parked in `commands`, if any, and flushes pending upload blits.
void EndParked(id<MTLCommandBuffer> commands);
// New encoders, after ending the one parked in `commands`.
[[nodiscard]] id<MTLComputeCommandEncoder> Compute(id<MTLCommandBuffer> commands, bool flushUploads = true);
[[nodiscard]] id<MTLBlitCommandEncoder> Blit(id<MTLCommandBuffer> commands);
[[nodiscard]] id<MTLRenderCommandEncoder> Render(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor* pass);
[[nodiscard]] id<MTLParallelRenderCommandEncoder> ParallelRender(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor* pass);
void EndCompute(id<MTLCommandBuffer> commands, id<MTLComputeCommandEncoder> encoder);
void SetOpenBatch(id<MTLCommandBuffer> commands);

// For draws. The encoder parked in `commands` when `mergeable`, `commands` is `batch`'s open batch
// and `key` continues the parked pass; otherwise a new encoder for `pass` (the parked one ended).
[[nodiscard]] id<MTLRenderCommandEncoder> RenderMerging(const MetalDevice& batch, id<MTLCommandBuffer> commands,
    MTLRenderPassDescriptor* pass, const PassKey& key, bool mergeable, bool* merged = nullptr);
// Instead of endEncoding after a draw: parks `encoder` for the next draw when `mergeable` and
// `commands` is `batch`'s open batch, and ends it otherwise.
void EndOrPark(const MetalDevice& batch, id<MTLCommandBuffer> commands, id<MTLRenderCommandEncoder> encoder,
    const PassKey& key, bool mergeable);

struct Statistics {
    std::uint64_t passes = 0;
    std::uint64_t merged = 0;
    std::uint64_t uploadBlits = 0;
    std::uint64_t uploadBlitPasses = 0;
    std::uint64_t parallelPasses = 0;
    std::uint64_t parallelDraws = 0;
    std::uint64_t computeEncoders = 0;
    std::uint64_t computeReused = 0;
};
// Render passes RenderMerging opened and draws it merged into an open one so far.
[[nodiscard]] Statistics TakeStatistics(bool reset = true);

}

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
    // Encode in pieces: one compute encoder (labelled for GPU traces), then any number of
    // dispatches into it. Encode is one of each. `texture`, when given, binds at texture index 0;
    // the inline parameters bind after the buffers.
    [[nodiscard]] id<MTLComputeCommandEncoder> ComputeEncoder(id<MTLCommandBuffer> commands, NSString* label = nil) const;
    void Dispatch(id<MTLComputeCommandEncoder> encoder, NSString* kernel, NSArray<id<MTLBuffer>>* buffers,
                  const void* parameters, std::size_t parameterBytes, MTLSize grid,
                  const std::array<std::uint32_t, 22>* constants = nullptr, std::span<const NSUInteger> offsets = {},
                  id<MTLTexture> texture = nil) const;
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
    void validateDispatch(NSString* kernel, NSArray<id<MTLBuffer>>* buffers, const void* parameters, std::size_t parameterBytes,
                          std::span<const NSUInteger> offsets) const;

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
