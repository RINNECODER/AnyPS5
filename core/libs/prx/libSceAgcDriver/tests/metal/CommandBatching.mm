#include "MetalTestSupport.hpp"
#include "MetalTimeline.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// MetalDevice command buffer batching (SetBatching) with MetalTimeline: the GPU must run batched
// uploads, dispatches and draws in encoding order, a wait must see every write encoded before it,
// and a flip (a drain, then a present on another queue) must show the frame rendered before it.

namespace {

using AgcDriver::Metal::MetalDevice;
using AgcDriver::Metal::MetalTimeline;
using MetalTests::Require;

constexpr NSUInteger Elements = 64;
constexpr NSUInteger Side = 4;

NSString* const Source = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void scaleAdd(device uint* x [[buffer(0)]], constant uint2& p [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    x[i] = x[i] * p.x + p.y;
}
struct Corner { float4 position [[position]]; };
vertex Corner fullscreen(uint id [[vertex_id]]) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    Corner corner;
    corner.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return corner;
}
fragment uint4 readFirst(Corner in [[stage_in]], const device uint* x [[buffer(0)]]) {
    return uint4(x[0], 0, 0, 1);
}
)";

struct Kernels {
    id<MTLComputePipelineState> scaleAdd;
    id<MTLRenderPipelineState> readFirst;
};

Kernels Build(id<MTLDevice> device) {
    NSError* error = nil;
    auto library = [device newLibraryWithSource:Source options:nil error:&error];
    Require(library != nil, "Batching test shaders did not compile");
    Kernels kernels;
    kernels.scaleAdd = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"scaleAdd"] error:&error];
    auto descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"fullscreen"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"readFirst"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    kernels.readFirst = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    Require(kernels.scaleAdd != nil && kernels.readFirst != nil, "Batching test pipelines did not build");
    return kernels;
}

id<MTLBuffer> Filled(const MetalDevice& backend, std::uint32_t value) {
    auto buffer = backend.Buffer(Elements * sizeof(std::uint32_t));
    auto* words = static_cast<std::uint32_t*>(buffer.contents);
    for (NSUInteger i = 0; i < Elements; ++i) words[i] = value;
    return buffer;
}

id<MTLTexture> Target(id<MTLDevice> device) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:Side height:Side mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    auto texture = [device newTextureWithDescriptor:descriptor];
    Require(texture != nil, "Batching test target allocation failed");
    return texture;
}

// Each helper takes its own command buffer, as the driver's uploads, dispatches and draws do.
void Upload(const MetalDevice& backend, id<MTLBuffer> staging, id<MTLBuffer> destination) {
    auto commands = backend.CommandBuffer();
    auto blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:staging sourceOffset:0 toBuffer:destination destinationOffset:0 size:destination.length];
    [blit endEncoding];
    backend.Submit(commands);
}

id<MTLCommandBuffer> Dispatch(const MetalDevice& backend, const Kernels& kernels, id<MTLBuffer> x, std::uint32_t scale, std::uint32_t add) {
    auto commands = backend.CommandBuffer();
    auto encoder = [commands computeCommandEncoder];
    const std::array<std::uint32_t, 2> parameters{scale, add};
    [encoder setComputePipelineState:kernels.scaleAdd];
    [encoder setBuffer:x offset:0 atIndex:0];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(Elements, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    return commands;
}

id<MTLCommandBuffer> Draw(const MetalDevice& backend, const Kernels& kernels, id<MTLBuffer> x, id<MTLTexture> target) {
    auto commands = backend.CommandBuffer();
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:kernels.readFirst];
    [encoder setFragmentBuffer:x offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return commands;
}

id<MTLCommandBuffer> Clear(const MetalDevice& backend, id<MTLTexture> target, std::uint32_t value) {
    auto commands = backend.CommandBuffer();
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(value, 0, 0, 1);
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    return commands;
}

void CopyTexel(id<MTLCommandBuffer> commands, id<MTLTexture> texture, id<MTLBuffer> destination) {
    auto blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(1, 1, 0) sourceSize:MTLSizeMake(1, 1, 1)
        toBuffer:destination destinationOffset:0 destinationBytesPerRow:sizeof(std::uint32_t) destinationBytesPerImage:sizeof(std::uint32_t)];
    [blit endEncoding];
}

std::uint32_t First(id<MTLBuffer> buffer) { return *static_cast<const std::uint32_t*>(buffer.contents); }

bool AllEqual(id<MTLBuffer> buffer, std::uint32_t value) {
    const auto* words = static_cast<const std::uint32_t*>(buffer.contents);
    for (NSUInteger i = 0; i < buffer.length / sizeof(std::uint32_t); ++i) {
        if (words[i] != value) return false;
    }
    return true;
}

// Upload, dispatch, draw, upload over what the draw read, dispatch: one command buffer, and the
// GPU result of running them in order (read-after-write and write-after-read between encoders).
void OrderingInOneCommandBuffer(const MetalTests::Context& context, const Kernels& kernels) {
    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    std::uint64_t signals = 0;
    timeline.SetCompletionSignal([&signals] { ++signals; });
    auto x = Filled(backend, 0);
    auto target = Target(context.device);
    auto drawn = backend.Buffer(sizeof(std::uint32_t));
    std::vector<int> completed;
    const auto before = backend.CommandBuffersCreated();

    Upload(backend, Filled(backend, 1), x);
    timeline.Commit(Dispatch(backend, kernels, x, 2, 1), [&] { completed.push_back(1); });  // x = 3
    auto draw = Draw(backend, kernels, x, target);                                          // texels = 3
    timeline.Commit(draw, [&] { completed.push_back(2); });
    Require(draw.status == MTLCommandBufferStatusNotEnqueued, "Timeline committed a batched command buffer per work");
    Upload(backend, Filled(backend, 10), x);                                                // x = 10
    auto last = Dispatch(backend, kernels, x, 1, 5);                                        // x = 15
    CopyTexel(last, target, drawn);
    timeline.Commit(last, [&] { completed.push_back(3); });
    Require(timeline.Reap() == false && completed.empty(), "Reap ran work whose batch was never committed");
    Require(backend.CommandBuffersCreated() - before == 1, "Batched uploads, dispatches and draws used more than one command buffer");

    timeline.Drain();
    Require(completed == std::vector<int>{1, 2, 3}, "Batched completions did not run once each in commit order");
    Require(First(drawn) == 3, "The draw did not read the dispatch's write encoded before it, or read the later upload");
    Require(AllEqual(x, 15), "Batched uploads and dispatches did not run in encoding order");
    Require(signals == 1, "The completion signal did not fire exactly once for the batch");

    // The committed batch is closed: the next work opens a new command buffer.
    auto next = backend.CommandBuffer();
    Require(next != last && backend.CommandBuffersCreated() - before == 2, "Work was handed a committed batch");
    backend.Wait(next);
}

// A CPU readback waits on a command buffer of its own: with batching that is the open batch, so
// the wait commits it, and the copy sees every write encoded before it.
void ReadbackSeesPriorWrites(const MetalTests::Context& context, const Kernels& kernels) {
    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    auto x = Filled(backend, 4);
    auto readback = Filled(backend, 0);
    bool ran = false;
    timeline.Commit(Dispatch(backend, kernels, x, 3, 0), [&] { ran = true; });  // x = 12
    Upload(backend, Filled(backend, 7), readback);                               // overwritten below
    auto commands = backend.CommandBuffer();
    auto blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:x sourceOffset:0 toBuffer:readback destinationOffset:0 size:readback.length];
    [blit endEncoding];
    Require(backend.OpenBatch(commands), "The readback was not handed the open batch");
    backend.Wait(commands);
    Require(commands.status == MTLCommandBufferStatusCompleted && !backend.OpenBatch(commands), "Waiting did not commit the batch");
    Require(AllEqual(readback, 12), "The readback did not see the dispatch and upload encoded before it");
    Require(!ran, "A wait outside the timeline ran timeline completions");
    timeline.Drain();
    Require(ran, "The drain after the readback did not complete the batched dispatch");
}

// A flip drains the timeline, then presents on another queue (the driver's presentation device):
// each present must show the frame rendered before its flip, never an earlier or later one.
void FlipPresentsTheRightFrame(const MetalTests::Context& context, const Kernels& kernels) {
    MetalDevice backend(context.device, context.library);
    MetalDevice presentation(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    auto display = Target(context.device);
    auto x = Filled(backend, 0);
    const auto present = [&] {
        timeline.Drain();
        auto shown = presentation.Buffer(sizeof(std::uint32_t));
        auto commands = presentation.CommandBuffer();
        CopyTexel(commands, display, shown);
        presentation.Wait(commands);
        return First(shown);
    };
    for (std::uint32_t frame = 1; frame <= 3; ++frame) {
        timeline.Commit(Clear(backend, display, 0), [] {});
        Upload(backend, Filled(backend, frame * 100), x);
        timeline.Commit(Draw(backend, kernels, x, display), [] {});
        Require(present() == frame * 100, "A flip presented a frame other than the one rendered before it");
    }
    // Work after the last flip stays batched until the next one.
    timeline.Commit(Clear(backend, display, 7), [] {});
    Require(present() == 7, "A flip after a clear presented a stale frame");
}

// Without batching every caller gets a command buffer of its own (the previous behavior), and an
// idle GPU makes the timeline commit an open batch early once it holds enough work.
void BatchingOffAndEarlyCommit(const MetalTests::Context& context, const Kernels& kernels) {
    MetalDevice plain(context.device, context.library);
    auto x = Filled(plain, 1);
    auto a = plain.CommandBuffer();
    auto b = plain.CommandBuffer();
    Require(a != b && !plain.OpenBatch(a), "A device without batching shared a command buffer");
    plain.Wait(a);
    plain.Wait(b);

    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 2);
    auto first = Dispatch(backend, kernels, x, 2, 0);
    timeline.Commit(first, [] {});
    Require(first.status == MTLCommandBufferStatusNotEnqueued, "One work was committed early");
    timeline.Commit(Dispatch(backend, kernels, x, 2, 0), [] {});
    Require(first.status >= MTLCommandBufferStatusCommitted, "An idle GPU did not get the batch once it held enough work");
    timeline.Drain();
    Require(AllEqual(x, 4), "Early-committed batch produced the wrong result");
    backend.SetBatching(false);
    auto c = backend.CommandBuffer();
    Require(!backend.OpenBatch(c) && c != backend.CommandBuffer(), "Turning batching off kept sharing command buffers");
}

}

// A work that encodes into the batch and then stops (a skip) before any hand-off: its encoded
// pass still runs with the batch, so its memory must not be reused before that. The guard waits,
// and a CPU reuse of the work's buffer afterwards is never overwritten by the GPU.
void AbandonedWorkReleasesAfterTheBatch(const MetalTests::Context& context, const Kernels& kernels) {
    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    auto earlier = Filled(backend, 1);
    timeline.Commit(Dispatch(backend, kernels, earlier, 1, 1), [] {});  // earlier = 2
    auto scratch = Filled(backend, 5);
    id<MTLCommandBuffer> abandoned = nil;
    try {
        abandoned = backend.CommandBuffer();
        AgcDriver::Metal::MetalBatchedWork batched(backend, abandoned);
        auto encoder = [abandoned computeCommandEncoder];
        const std::array<std::uint32_t, 2> parameters{3, 0};
        [encoder setComputePipelineState:kernels.scaleAdd];
        [encoder setBuffer:scratch offset:0 atIndex:0];
        [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(Elements, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        throw std::runtime_error("skipped");
    } catch (const std::runtime_error&) {
    }
    Require(abandoned.status == MTLCommandBufferStatusCompleted, "An abandoned batched work did not wait for its batch");
    Require(AllEqual(scratch, 15), "The abandoned work's encoded pass did not run with its batch");
    std::memset(scratch.contents, 0, scratch.length);  // reused, as a pooled buffer would be
    timeline.Commit(Dispatch(backend, kernels, earlier, 1, 1), [] {});  // earlier = 3
    timeline.Drain();
    Require(AllEqual(scratch, 0), "The GPU wrote a buffer after its work was abandoned and the buffer reused");
    Require(AllEqual(earlier, 3), "Work around the abandoned one did not run in order");

    // A work handed off normally does not wait.
    auto handed = backend.CommandBuffer();
    {
        AgcDriver::Metal::MetalBatchedWork batched(backend, handed);
        batched.Done();
    }
    Require(backend.OpenBatch(handed), "A handed-off work committed the batch");
    timeline.Drain();
    backend.Flush();
}

void RunCommandBatchingTests(const MetalTests::Context& context) {
    const auto kernels = Build(context.device);
    OrderingInOneCommandBuffer(context, kernels);
    ReadbackSeesPriorWrites(context, kernels);
    FlipPresentsTheRightFrame(context, kernels);
    BatchingOffAndEarlyCommit(context, kernels);
    AbandonedWorkReleasesAfterTheBatch(context, kernels);
}
