#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("VideoOut: ") + reason);
}

void checkConfig(const VideoOutConfig& cfg) {
    cfg.Check();
}

// Resolved when the GPU reaches the flip, not when the command buffer records it: a title may
// register the buffer in between. An unregistered buffer presents black instead of failing.
void resolveFlipBuffer(FlipRequest& request, const VideoOutConfig& cfg) {
    if (request.index < 0) {
        request.width = cfg.width;
        request.height = cfg.height;
        return;
    }
    request.buffer = cfg.buffers[request.index];
    if (!request.buffer.Occupied()) {
        static std::array<std::atomic<bool>, VIDEO_OUT_BUFFER_NUM_MAX> reported{};
        if (!reported[request.index].exchange(true)) std::fprintf(stderr, "[videoout] GPU flip of unregistered buffer %d on handle %u presents black\n", request.index, request.outputHandle);
        request.unregistered = true;
        request.width = cfg.width;
        request.height = cfg.height;
        return;
    }
    require(request.buffer.groupIndex < VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX, "invalid buffer group");
    request.group = cfg.groups[request.buffer.groupIndex];
    require(request.group.occupied, "buffer group is not registered");
    require(request.buffer.dataAddress != 0, "null registered buffer address");
    static_cast<void>(DescribeVideoOutBuffer(request.buffer, request.group));
    request.width = request.group.attribute.width;
    request.height = request.group.attribute.height;
}

class RenderingWait final : public AgcDriver::IRenderingWait {
    std::shared_ptr<VideoOutConfig> _config;
    std::uint32_t _index;
    std::uint64_t _ticket;
public:
    RenderingWait(std::shared_ptr<VideoOutConfig> config, std::uint32_t index, std::uint64_t ticket)
        : _config(std::move(config)), _index(index), _ticket(ticket) {}
    void Wait() override {
        std::unique_lock lock(_config->mutex);
        _config->vblankCond.wait(lock, _config->shutdownToken, [&] {
            return _config->failure || _config->Closed() ||
                _config->bufferReuse[_index].IsComplete(_ticket);
        });
        _config->CheckAlive();
    }
};

class VideoOutput final : public AgcDriver::IVideoOutput {
public:
    VideoOutput(std::shared_ptr<VideoOutConfig> config, std::shared_ptr<FlipQueue> requests) : cfg(std::move(config)), queue(std::move(requests)) {}

    std::shared_ptr<AgcDriver::IRenderingWait> CaptureRenderingWait(std::uint32_t index) override {
        std::lock_guard lock(cfg->mutex);
        checkConfig(*cfg);
        require(index < VIDEO_OUT_BUFFER_NUM_MAX && cfg->buffers[index].Occupied(), "wait buffer is not registered");
        return std::make_shared<RenderingWait>(cfg, index, cfg->bufferReuse[index].Capture());
    }

    void WaitForFlipRoom() override {
        std::unique_lock queueLock(queue->mutex);
        const bool room = queue->changed.wait_for(queueLock, std::chrono::seconds(60), [&] {
            return queue->failure || queue->stopping || cfg->shutdownToken.stop_requested() || queue->reservations.load() < VIDEO_OUT_FLIP_QUEUE_CAPACITY;
        });
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
        require(room, "flip queue stayed full for 60 s: the presenter is not completing flips");
    }

    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo& info) override {
        require(info.mode >= VIDEO_OUT_FLIP_MODE_VSYNC && info.mode <= 6, "unsupported flip mode");
        require(info.index >= VIDEO_OUT_BUFFER_INDEX_BLACK && info.index < VIDEO_OUT_BUFFER_NUM_MAX, "invalid flip index");
        auto request = std::make_shared<FlipRequest>();
        request->cfg = cfg;
        request->queue = queue;
        request->index = info.index;
        request->outputHandle = info.handle;
        request->flipMode = static_cast<int>(info.mode);
        request->flipArg = info.argument;
        std::lock_guard queueLock(queue->mutex);
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
        std::lock_guard lock(cfg->mutex);
        checkConfig(*cfg);
        request->generation = cfg->generation;
        request->flipRate = cfg->flipRate;
        if (info.index >= 0) request->reuseTicket = cfg->bufferReuse[info.index].Reserve();
        ++queue->reservations;
        ++cfg->flipStatus.flipPendingNum;
        if (info.index >= 0) ++cfg->bufferPending[info.index];
        request->reserved = true;
        return request;
    }

    void Fail(std::exception_ptr error) noexcept override {
        if (!error) std::terminate();
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->failure) queue->failure = error;
        }
        {
            std::lock_guard lock(cfg->mutex);
            if (!cfg->failure) cfg->failure = error;
            cfg->vblankCond.notify_all();
        }
        queue->changed.notify_all();
    }

private:
    std::shared_ptr<VideoOutConfig> cfg;
    std::shared_ptr<FlipQueue> queue;
};

}

FlipRequest::~FlipRequest() {
    if (!reserved) return;
    std::unique_lock lock(cfg->mutex);
    if (terminal) return;
    ReleaseLocked();
    lock.unlock();
    std::lock_guard queueLock(queue->mutex);
    queue->changed.notify_all();
}

void FlipRequest::ReleaseLocked() noexcept {
    if (!reserved || terminal) return;
    --cfg->flipStatus.flipPendingNum;
    --queue->reservations;
    if (index >= 0) {
        --cfg->bufferPending[index];
        cfg->bufferReuse[index].Complete(reuseTicket);
    }
    terminal = true;
    cfg->vblankCond.notify_all();
}

void FlipRequest::Cancel() noexcept {
    {
        std::lock_guard lock(cfg->mutex);
        ReleaseLocked();
    }
    queue->changed.notify_all();
}

void FlipRequest::GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>& frameTiming) {
    require(frameTiming != nullptr, "missing frame timing");
    timing = frameTiming;
    bool released = false;
    {
        AgcDriver::PerformanceContext timingContext(timing.get());
        AgcDriver::PerformanceTimer readiness("VideoOut.Readiness");
        std::lock_guard queueLock(queue->mutex);
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || cfg->shutdownToken.stop_requested()) throw ProcessShutdown{};
        std::lock_guard lock(cfg->mutex);
        cfg->CheckAlive();
        if (cfg->Closed()) {
            // The title closed the port: drop the reservation so producers waiting for room wake.
            ReleaseLocked();
            released = true;
        } else {
            require(reserved && !ready && !terminal && cfg->generation == generation, "invalid flip readiness transition");
            resolveFlipBuffer(*this, *cfg);
            readiness.Mark("locks_validate");
            queuedAt = AgcDriver::FrameTiming::Clock::now();
            queue->requests.push_back(shared_from_this());
            ready = true;
        }
    }
    queue->changed.notify_all();
    if (released) return;
    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    if (!syncFlip) return;
    std::unique_lock lock(cfg->mutex);
    cfg->vblankCond.wait(lock, cfg->shutdownToken, [&] { return gpuComplete || cfg->failure || cfg->closing; });
    checkConfig(*cfg);
    require(gpuComplete, "flip preparation did not complete on GPU");
}

void FlipRequest::Fail(std::exception_ptr error) noexcept {
    if (!error) std::terminate();
    std::lock_guard lock(cfg->mutex);
    if (!cfg->failure) cfg->failure = error;
    ReleaseLocked();
    cfg->vblankCond.notify_all();
    queue->changed.notify_all();
}

std::shared_ptr<AgcDriver::IVideoOutput> CreateVideoOutput(
    std::shared_ptr<VideoOutConfig> config, std::shared_ptr<FlipQueue> queue) {
    require(config != nullptr && queue != nullptr, "missing video output config or queue");
    return std::make_shared<VideoOutput>(std::move(config), std::move(queue));
}

bool WaitForFlipVblank(FlipRequest& req, AgcDriver::PerformanceTimer* timing) {
    std::unique_lock lock(req.cfg->mutex);
    if (timing) timing->Mark("config_mutex_wait");
    req.cfg->CheckAlive();
    if (req.cfg->Closed()) {
        req.ReleaseLocked();
        return false;
    }
    require(req.ready && !req.terminal && req.generation == req.cfg->generation, "stale or incomplete flip request");
    const auto interval = static_cast<uint64_t>(req.flipRate + 1);
    require(req.cfg->lastFlipVblank <= std::numeric_limits<uint64_t>::max() - interval, "flip interval overflow");
    const auto target = req.cfg->lastFlipVblank + interval;
    if (timing) timing->Mark("validate");
    req.cfg->vblankCond.wait(lock, req.cfg->shutdownToken, [&] { return req.cfg->vblankStatus.count >= target || req.cfg->failure || req.cfg->closing; });
    if (timing) timing->Mark("vblank_wait");
    req.cfg->CheckAlive();
    if (req.cfg->Closed()) {
        req.ReleaseLocked();
        return false;
    }
    req.cfg->lastFlipVblank = req.cfg->vblankStatus.count;
    return true;
}

void MarkFlipGpuComplete(FlipRequest& request) {
    std::lock_guard lock(request.cfg->mutex);
    request.cfg->CheckAlive();
    // The port closed while the flip was in flight; CompleteFlip releases it.
    if (request.cfg->Closed()) return;
    require(!request.terminal && !request.gpuComplete, "invalid GPU completion transition");
    request.gpuComplete = true;
    request.cfg->vblankCond.notify_all();
}

void CompleteFlip(FlipRequest& req, const VideoOutCompletionCallbacks& callbacks,
                  AgcDriver::PerformanceTimer* timing) {
    std::unique_lock lock(req.cfg->mutex);
    if (timing) timing->Mark("completion_mutex_wait");
    req.cfg->CheckAlive();
    if (req.cfg->Closed()) {
        req.ReleaseLocked();
        lock.unlock();
        std::lock_guard queueLock(req.queue->mutex);
        req.queue->changed.notify_all();
        return;
    }
    require(!req.terminal && req.cfg->generation == req.generation, "flip cancelled during presentation");
    require(req.gpuComplete, "flip submitted before GPU completion");
    require(req.cfg->flipStatus.count != std::numeric_limits<uint64_t>::max(), "flip counter overflow");
    require(callbacks.processTime != nullptr && callbacks.processTimeCounter != nullptr && callbacks.flipEvent != nullptr, "missing flip completion callbacks");
    callbacks.flipEvent(callbacks.context, *req.cfg, req.flipArg);
    ++req.cfg->flipStatus.count;
    req.cfg->flipStatus.processTime = callbacks.processTime(callbacks.context);
    req.cfg->flipStatus.processTimeCounter = callbacks.processTimeCounter(callbacks.context);
    req.cfg->flipStatus.flipArg = req.flipArg;
    req.cfg->flipStatus.currentBuffer = req.index;
    req.cfg->width = req.width;
    req.cfg->height = req.height;
    req.ReleaseLocked();
    if (timing) timing->Mark("notify_game");
    lock.unlock();
    std::lock_guard queueLock(req.queue->mutex);
    req.queue->changed.notify_all();
}
