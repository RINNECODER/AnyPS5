#pragma once

#import <Metal/Metal.h>
#include "FrameProfile.hpp"
#include "GpuTiming.hpp"
#include "MetalDevice.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

// Whether a converted shader may store to guest memory through BDA pointers (the recompiler
// defines its written-page note, note_bda_write, only for shaders with such stores). Those stores
// reach pages nobody can name before the work completes.
template <typename Converted>
[[nodiscard]] bool StoresThroughBda(const Converted& shader) {
    return shader.source.find("bda_write") != std::string::npos;
}

// Draws and dispatches committed without waiting (Rosetta direct path performance).
//
// Before, every draw and dispatch committed its command buffer and waited for it, so the queue
// worker spent most of its time in waitUntilCompleted and the GPU sat idle while the worker
// decoded the next packet. Now the worker encodes and commits a run of draws and dispatches on
// one Metal queue (which, with the residency every encoder declares, runs them in order) and
// queues the CPU half of each (fault check, copy of staged bytes into guest memory) here.
// `Drain` waits for the last command buffer and runs those completions in commit order. The
// driver drains wherever the CPU or the guest could observe the GPU's results: before it stores
// to or reads guest memory itself (labels, events, DMA, waits, indirect arguments, conditions),
// before a flip, and at the end of every submission, so the guest sees exactly the order and
// results the synchronous path gave it.
//
// With a batching device (SetBatchDevice), the work shares one command buffer per run instead of
// one each (MetalDevice::SetBatching): Commit leaves the open batch uncommitted, and the batch
// commits when the drain needs it, when the owner calls Flush (at the end of a submission and
// before it blocks), or as soon as the GPU has finished the previous batch, so it never idles
// waiting for the CPU. Completions still run per work, in commit order.
//
// A completion that throws stops the drain: the remaining completions are dropped (their
// command buffers were already waited for) and the exception propagates, as the synchronous
// wait would have thrown it from the work's own packet.
class MetalTimeline {
public:
    using Completion = std::function<void()>;

    MetalTimeline() = default;
    MetalTimeline(const MetalTimeline&) = delete;
    MetalTimeline& operator=(const MetalTimeline&) = delete;
    ~MetalTimeline() {
        // Shutdown or failure: let the GPU finish what it holds; never run completions here.
        for (auto& work : pending) {
            if (work.commands != nil && work.commands.status >= MTLCommandBufferStatusCommitted) [work.commands waitUntilCompleted];
        }
    }

    // Commits `commands` (when not committed yet) and queues `complete` for the next drain.
    // `unknownWrites` marks work that may store to guest memory the driver cannot name in advance
    // (BDA stores): the driver drains before its next CPU read of guest memory.
    // `written` lists the guest ranges [begin, end) the work stores to (written images, writable
    // buffers): a CPU read of guest memory overlapping one drains first (MayHaveWritten).
    void Commit(id<MTLCommandBuffer> commands, Completion complete, bool unknownWrites = false,
        std::vector<std::pair<std::uint64_t, std::uint64_t>> written = {}) {
        if (commands == nil) throw std::invalid_argument("Metal timeline requires a command buffer");
        if (commands.status < MTLCommandBufferStatusCommitted) {
            // One signal per command buffer: a batch carries many works.
            if (auto signal = completionSignal; signal && commands != LastCommitted()) {
                [commands addCompletedHandler:^(id<MTLCommandBuffer>) { (*signal)(); }];
            }
            if (batch == nullptr || !batch->OpenBatch(commands)) [commands commit];
        }
        pending.push_back({commands, std::move(complete), GpuTiming::Current()});
        unknown = unknown || unknownWrites;
        writes.insert(writes.end(), written.begin(), written.end());
        ++committed;
        if (batch != nullptr && batch->GpuStarved(batchMinimum)) batch->Flush();
    }

    // Shares one command buffer among the work committed here while `device` batches; nullptr
    // stops. `minimum` works must be in the open batch before an idle GPU commits it early.
    void SetBatchDevice(const MetalDevice* device, std::uint32_t minimum = 8) {
        if (batch != nullptr) batch->Flush();
        batch = device;
        batchMinimum = minimum;
    }

    // Commits the open batch without waiting, so the GPU runs it (and Reap can retire it) while
    // the owner is idle or blocked.
    void Flush() {
        if (batch != nullptr) batch->Flush();
    }

    // Called (on a Metal thread) whenever a command buffer committed here completes, so an idle
    // owner can Reap. Set before the first Commit.
    void SetCompletionSignal(std::function<void()> signal) {
        completionSignal = std::make_shared<const std::function<void()>>(std::move(signal));
    }

    // Queues CPU work (a label store, an interrupt) to run at the next drain, after the completions
    // of everything committed before it.
    void Defer(Completion complete) {
        pending.push_back({nil, std::move(complete), GpuTiming::Current()});
    }

    // Whether pending work may store to guest [begin, end) (or anywhere, with unknown writes).
    [[nodiscard]] bool MayHaveWritten(std::uint64_t begin, std::uint64_t end) const {
        if (pending.empty()) return false;
        if (unknown) return true;
        for (const auto& [writeBegin, writeEnd] : writes) {
            if (begin < writeEnd && writeBegin < end) return true;
        }
        return false;
    }

    [[nodiscard]] bool Empty() const { return pending.empty(); }
    [[nodiscard]] std::size_t Pending() const { return pending.size(); }
    // Some pending work may have stored to guest memory nobody can name before it completes.
    [[nodiscard]] bool UnknownWritesPending() const { return unknown; }
    [[nodiscard]] std::uint64_t Committed() const { return committed; }
    [[nodiscard]] std::uint64_t Drains() const { return drains; }
    // Moves whenever completions ran (a drain or a reap): CPU work queued before it may be visible.
    [[nodiscard]] std::uint64_t Epoch() const { return epoch; }
    // The command buffer committed last, if any is pending.
    [[nodiscard]] id<MTLCommandBuffer> LastCommitted() const {
        for (auto work = pending.rbegin(); work != pending.rend(); ++work) {
            if (work->commands != nil) return work->commands;
        }
        return nil;
    }

    // Runs, in order, the completions whose command buffers already completed, without waiting;
    // stops at the first that has not. Returns whether any ran.
    bool Reap() {
        bool ran = false;
        while (!pending.empty()) {
            auto& head = pending.front();
            if (head.commands != nil && head.commands.status < MTLCommandBufferStatusCompleted) break;
            auto work = std::move(head);
            pending.pop_front();
            if (!ran) ++epoch;
            ran = true;
            GpuTiming::Scope timing(work.kind);
            if (work.commands != nil) WaitCommitted(work.commands);
            if (work.complete) work.complete();
        }
        if (pending.empty()) {
            unknown = false;
            writes.clear();
        }
        return ran;
    }

    // Waits for every pending command buffer, then runs the completions in commit order.
    // `cause` says why (FrameProfile.hpp lists the codes) for ANYPS5_METAL_GPU_TIMING.
    void Drain(std::uint32_t cause = 0x10f) {
        Flush();
        if (pending.empty()) return;
        ++drains;
        ++epoch;
        // The queue completes command buffers in commit order; waiting for the last one first
        // keeps this to one round trip.
        double waited = 0;
        for (auto work = pending.rbegin(); work != pending.rend(); ++work) {
            if (work->commands == nil) continue;
            const auto start = std::chrono::steady_clock::now();
            const double hostStart = FrameProfile::Enabled() ? FrameProfile::Now() : 0;
            if (work->commands.status < MTLCommandBufferStatusCompleted) [work->commands waitUntilCompleted];
            if (FrameProfile::Enabled()) FrameProfile::Shared().Waited(cause, hostStart, FrameProfile::Now());
            waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            break;
        }
        auto works = std::move(pending);
        pending.clear();
        unknown = false;
        writes.clear();
        for (auto& work : works) {
            GpuTiming::Scope timing(work.kind);
            if (work.commands != nil) {
                WaitCommitted(work.commands, waited);
                waited = 0;
            }
            if (work.complete) work.complete();
        }
    }

    // Waits for a committed command buffer and throws MetalGpuExecutionError if it failed.
    // `waitedMs` is wait time already spent on its behalf (for ANYPS5_METAL_GPU_TIMING).
    static void WaitCommitted(id<MTLCommandBuffer> commands, double waitedMs = 0) {
        const auto start = std::chrono::steady_clock::now();
        if (commands.status < MTLCommandBufferStatusCompleted) [commands waitUntilCompleted];
        if (GpuTiming::Enabled()) {
            GpuTiming::Record(waitedMs + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
                commands.GPUEndTime - commands.GPUStartTime, commands.GPUStartTime, commands.GPUEndTime);
        }
        if (commands.status == MTLCommandBufferStatusError) {
            NSString* text = commands.error.localizedDescription;
            throw MetalGpuExecutionError(std::string("Metal GPU execution failed: ") + (text != nil ? text.UTF8String : "unknown error"));
        }
        if (commands.status != MTLCommandBufferStatusCompleted) throw MetalGpuExecutionError("Metal command buffer did not complete");
    }

private:
    struct Work {
        id<MTLCommandBuffer> commands;
        Completion complete;
        GpuTiming::Kind kind;
    };
    std::deque<Work> pending;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    bool unknown = false;
    std::uint64_t committed = 0;
    std::uint64_t drains = 0;
    std::uint64_t epoch = 0;
    std::shared_ptr<const std::function<void()>> completionSignal;
    const MetalDevice* batch = nullptr;
    std::uint32_t batchMinimum = 8;
};

}
