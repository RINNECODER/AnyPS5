#include "MetalTestSupport.hpp"
#include "MetalTimeline.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

// MetalTimeline::DeferSignal: a label point (RELEASE_MEM, WRITE_DATA) the GPU signals on a shared
// event. GPU work on another queue that waits for the event sees what the producer wrote without the
// CPU draining, and the label's CPU store runs as soon as the work before it completed, not behind
// work encoded after it, so a guest polling the label sees it without a drain.

namespace {

using AgcDriver::Metal::MetalDevice;
using AgcDriver::Metal::MetalTimeline;
using MetalTests::Require;

constexpr NSUInteger Elements = 1024;

NSString* const Source = @R"(
#include <metal_stdlib>
using namespace metal;
// Slow on purpose (rounds of dependent integer work), so a consumer that did not wait reads stale words.
kernel void produce(device uint* x [[buffer(0)]], constant uint2& p [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    uint value = x[i];
    for (uint round = 0; round < p.y; ++round) value = value * 1664525u + 1013904223u;
    x[i] = value == p.x + 12345u ? 0u : p.x;
}
)";

id<MTLComputePipelineState> Build(id<MTLDevice> device) {
    NSError* error = nil;
    auto library = [device newLibraryWithSource:Source options:nil error:&error];
    Require(library != nil, "Label event test shader did not compile");
    auto pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"produce"] error:&error];
    Require(pipeline != nil, "Label event test pipeline did not build");
    return pipeline;
}

id<MTLBuffer> Filled(const MetalDevice& backend, std::uint32_t value) {
    auto buffer = backend.Buffer(Elements * sizeof(std::uint32_t));
    auto* words = static_cast<std::uint32_t*>(buffer.contents);
    for (NSUInteger i = 0; i < Elements; ++i) words[i] = value;
    return buffer;
}

// The event's value reaches the CPU on its own schedule (it may trail a command buffer's completion).
bool SignalledWithin(const MetalTimeline& timeline, std::uint64_t value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (timeline.LabelsSignalled() < value && std::chrono::steady_clock::now() < deadline) {}
    return timeline.LabelsSignalled() >= value;
}

bool AllEqual(id<MTLBuffer> buffer, std::uint32_t value) {
    const auto* words = static_cast<const std::uint32_t*>(buffer.contents);
    for (NSUInteger i = 0; i < buffer.length / sizeof(std::uint32_t); ++i) {
        if (words[i] != value) return false;
    }
    return true;
}

id<MTLCommandBuffer> Produce(const MetalDevice& backend, id<MTLComputePipelineState> pipeline, id<MTLBuffer> x, std::uint32_t value,
    std::uint32_t rounds) {
    auto commands = backend.CommandBuffer();
    auto encoder = [commands computeCommandEncoder];
    const std::array<std::uint32_t, 2> parameters{value, rounds};
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:x offset:0 atIndex:0];
    [encoder setBytes:parameters.data() length:sizeof(parameters) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(Elements, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    return commands;
}

// Waits at most 10 s, so a consumer whose event never comes fails instead of hanging.
void WaitFor(id<MTLCommandBuffer> commands) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (commands.status < MTLCommandBufferStatusCompleted && std::chrono::steady_clock::now() < deadline) {}
    Require(commands.status == MTLCommandBufferStatusCompleted, "Label event test command buffer failed or never ran");
}

// A consumer on a second queue, committed before its producer, waits for the producer's label on the
// GPU and copies what the producer wrote; the CPU never drains the producer's timeline meanwhile.
void ConsumerOnAnotherQueueWaitsForTheLabel(const MetalTests::Context& context, id<MTLComputePipelineState> pipeline) {
    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    auto x = Filled(backend, 0);
    auto copy = Filled(backend, 0xdeadu);
    // The event exists from the first label point on: a fresh timeline's first label signals 1.
    timeline.Commit(Produce(backend, pipeline, Filled(backend, 0), 1, 1), [] {});
    Require(timeline.DeferSignal([] {}, true) == 1 && timeline.LabelEvent() != nil, "The first label point did not signal 1 on a new shared event");
    id<MTLCommandQueue> other = [context.device newCommandQueue];
    id<MTLCommandBuffer> consumer = [other commandBuffer];
    [consumer encodeWaitForEvent:timeline.LabelEvent() value:2];
    auto blit = [consumer blitCommandEncoder];
    [blit copyFromBuffer:x sourceOffset:0 toBuffer:copy destinationOffset:0 size:copy.length];
    [blit endEncoding];
    [consumer commit];

    bool labelled = false;
    timeline.Commit(Produce(backend, pipeline, x, 7, 20000), [] {});
    const auto value = timeline.DeferSignal([&] { labelled = true; }, true);
    Require(value == 2, "The second label point did not signal 2");
    Require(!timeline.Empty() && !labelled, "Deferring the label ran it or drained the timeline");
    WaitFor(consumer);
    Require(AllEqual(copy, 7), "The consumer on another queue did not see the producer's writes before its label");
    Require(SignalledWithin(timeline, 2), "The consumer ran before the GPU signalled the label it waited for");
    timeline.Drain();
    Require(labelled && timeline.Empty(), "The drain did not run the label's store");
}

// The label's store runs at a Reap once the work before it completed, while work encoded after it
// is not even committed: a guest polling the label sees it with no drain.
void GuestPollingSeesTheLabelBeforeLaterWork(const MetalTests::Context& context, id<MTLComputePipelineState> pipeline) {
    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    std::uint64_t signals = 0;
    timeline.SetCompletionSignal([&signals] { ++signals; });
    auto x = Filled(backend, 0);
    auto later = Filled(backend, 0);
    std::vector<int> order;
    volatile std::uint32_t label = 0;  // the guest's view of the label
    auto before = Produce(backend, pipeline, x, 3, 2000);
    timeline.Commit(before, [&] { order.push_back(1); });
    Require(before.status == MTLCommandBufferStatusNotEnqueued, "The batched work was committed before its label");
    timeline.DeferSignal([&] {
        Require(AllEqual(x, 3), "The label was stored before the work before it finished");
        label = 1;
        order.push_back(2);
    }, true);
    Require(before.status >= MTLCommandBufferStatusCommitted, "The label point did not commit the open batch");
    auto after = Produce(backend, pipeline, later, 9, 1);
    timeline.Commit(after, [&] { order.push_back(3); });
    Require(after.status == MTLCommandBufferStatusNotEnqueued && after != before, "Work after the label joined the label's command buffer");

    // The guest's poll loop: the worker reaps between packets and never drains.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (label == 0 && std::chrono::steady_clock::now() < deadline) timeline.Reap();
    Require(label == 1, "A polled label was not stored without a drain");
    Require(order == std::vector<int>{1, 2}, "The label's store ran out of order or ran later work's completion");
    Require(after.status == MTLCommandBufferStatusNotEnqueued, "Polling the label committed work encoded after it");
    timeline.Drain();
    Require(order == std::vector<int>{1, 2, 3} && AllEqual(later, 9), "The work after the label did not complete in order");
    Require(signals >= 2, "Completion signals did not fire for the label's command buffer and the next");
}

// Without batching (or with the work before the label committed already), a command buffer that only
// signals goes behind that work; and a label with no GPU work pending runs at the next Reap.
void LabelBehindCommittedWork(const MetalTests::Context& context, id<MTLComputePipelineState> pipeline) {
    MetalDevice backend(context.device, context.library);
    MetalTimeline timeline;
    std::vector<int> order;
    Require(timeline.DeferSignal([&] { order.push_back(0); }) == 0, "A label with no GPU work pending asked the GPU to signal");
    Require(timeline.Reap() && order == std::vector<int>{0}, "A label with no GPU work pending did not run at the next Reap");
    auto x = Filled(backend, 0);
    auto work = Produce(backend, pipeline, x, 5, 2000);
    timeline.Commit(work, [&] { order.push_back(1); });
    Require(work.status >= MTLCommandBufferStatusCommitted, "Unbatched work was not committed");
    const auto value = timeline.DeferSignal([&] {
        Require(AllEqual(x, 5), "The label behind committed work ran before it");
        order.push_back(2);
    }, true);
    Require(value == 1, "The label behind committed work did not signal");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (order.size() < 3 && std::chrono::steady_clock::now() < deadline) timeline.Reap();
    Require(order == std::vector<int>{0, 1, 2} && timeline.Empty(), "The label behind committed work was not stored in order");
    Require(SignalledWithin(timeline, 1) && timeline.LabelsSignalled() == 1, "The signal-only command buffer did not signal the label value");
}

// Without commitNow a label in a small open batch rides it: signalled there, stored once the batch
// committed and completed; behind committed work it needs no GPU signal.
void LabelRidesItsBatch(const MetalTests::Context& context, id<MTLComputePipelineState> pipeline) {
    MetalDevice backend(context.device, context.library);
    backend.SetBatching(true);
    MetalTimeline timeline;
    timeline.SetBatchDevice(&backend, 1u << 20);
    auto x = Filled(backend, 0);
    auto work = Produce(backend, pipeline, x, 4, 10);
    timeline.Commit(work, [] {});
    bool labelled = false;
    Require(timeline.DeferSignal([&] { labelled = true; }) == 1, "A label in the open batch was not signalled");
    Require(work.status == MTLCommandBufferStatusNotEnqueued, "A label without commitNow committed a small batch");
    Require(!timeline.Reap() && !labelled, "A label ran before its batch was committed");
    timeline.Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!labelled && std::chrono::steady_clock::now() < deadline) timeline.Reap();
    Require(labelled && AllEqual(x, 4) && SignalledWithin(timeline, 1), "A label riding its batch was not stored after it");
    bool after = false;
    Require(timeline.DeferSignal([&] { after = true; }) == 0, "A label behind committed work asked for a GPU signal without commitNow");
    timeline.Drain();
    Require(after, "A label behind committed work did not run at the drain");
}

}

void RunLabelEventTests(const MetalTests::Context& context) {
    const auto pipeline = Build(context.device);
    ConsumerOnAnotherQueueWaitsForTheLabel(context, pipeline);
    GuestPollingSeesTheLabelBeforeLaterWork(context, pipeline);
    LabelBehindCommittedWork(context, pipeline);
    LabelRidesItsBatch(context, pipeline);
}
