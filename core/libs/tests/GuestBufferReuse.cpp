#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>

static void Require(bool value) { if (!value) std::abort(); }
static void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

static void TestTicketOrdering() {
    BufferReuseTracker buffer;
    Require(buffer.IsComplete(buffer.Capture()));
    const auto first = buffer.Reserve();
    const auto second = buffer.Reserve();
    const auto fence = buffer.Capture();
    const auto future = buffer.Reserve();
    Require(!buffer.IsComplete(fence));
    buffer.Complete(second); // Out-of-order retirement must not release first.
    Require(!buffer.IsComplete(fence));
    buffer.Complete(first);
    Require(buffer.IsComplete(fence)); // Later reservations do not block this wait.
    Require(!buffer.IsComplete(buffer.Capture()));
    buffer.Complete(future); // Cancellation uses the same retirement operation.
    Require(buffer.IsComplete(buffer.Capture()));
    const auto next = buffer.Reserve();
    Require(next > future && buffer.IsComplete(fence));
    buffer.Complete(next);
    bool rejected = false;
    try { buffer.IsComplete(next + 1); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected);
}

namespace {

struct Fixture {
    std::vector<std::byte> storage = std::vector<std::byte>(65536 + 65535);
    std::shared_ptr<VideoOutConfig> config = std::make_shared<VideoOutConfig>(std::stop_token{});
    std::shared_ptr<FlipQueue> queue = std::make_shared<FlipQueue>();
    std::shared_ptr<AgcDriver::IVideoOutput> output = CreateVideoOutput(config, queue);
    std::vector<std::shared_ptr<AgcDriver::IFlipRequest>> requests;
    unsigned events = 0;
    std::int64_t eventArgument = 0;

    Fixture() {
        config->opened = true;
        const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
        config->buffers[0] = {0, (address + 65535) & ~std::uintptr_t{65535}, 0};
        auto& group = config->groups[0];
        group.occupied = true;
        group.attribute.pixel_format = 0x8000000000000000ull;
        group.attribute.width = 64;
        group.attribute.height = 64;
        requests.push_back(output->Reserve({1, 0, VIDEO_OUT_FLIP_MODE_VSYNC, 123}));
        while (requests.size() < VIDEO_OUT_FLIP_QUEUE_CAPACITY)
            requests.push_back(output->Reserve({1, VIDEO_OUT_BUFFER_INDEX_BLANK, VIDEO_OUT_FLIP_MODE_VSYNC, 0}));
    }

    void CheckPending(std::size_t pending, bool bufferPending, unsigned completed) {
        std::lock_guard lock(config->mutex);
        Check(queue->reservations.load() == pending && config->flipStatus.flipPendingNum == static_cast<std::int32_t>(pending),
              "reservation and flip pending counts disagree");
        Check(config->bufferPending[0] == unsigned(bufferPending), "buffer pending count is wrong");
        Check(config->bufferReuse[0].IsComplete(config->bufferReuse[0].Capture()) == !bufferPending,
              "buffer reuse ticket retirement is wrong");
        Check(config->flipStatus.count == completed && events == completed, "successful flip/event count is wrong");
    }
};

class CapacityWaiter {
    std::promise<void> entered;
    std::promise<bool> finished;
    std::future<bool> result = finished.get_future();
    std::thread worker;
public:
    explicit CapacityWaiter(const std::shared_ptr<FlipQueue>& queue) {
        auto entry = entered.get_future();
        worker = std::thread([this, queue] {
            std::unique_lock lock(queue->mutex);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            entered.set_value();
            while (queue->reservations.load() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) {
                if (queue->changed.wait_until(lock, deadline) == std::cv_status::timeout) {
                    finished.set_value(false);
                    return;
                }
            }
            finished.set_value(true);
        });
        entry.wait();
        std::lock_guard lock(queue->mutex);
    }
    ~CapacityWaiter() { worker.join(); }
    bool Woke() { return result.get(); }
};

enum class Retirement { Cancellation, Completion, Failure };

void TestRetirement(Retirement retirement) {
    Fixture fixture;
    const auto full = fixture.requests.size();
    auto renderingWait = fixture.output->CaptureRenderingWait(0);
    auto* request = static_cast<FlipRequest*>(fixture.requests.front().get());
    if (retirement == Retirement::Completion) {
        request->GpuReady(std::make_shared<AgcDriver::FrameTiming>(1));
        {
            std::lock_guard lock(fixture.queue->mutex);
            Check(fixture.queue->requests.size() == 1, "GPU-ready flip was not queued");
            fixture.queue->requests.pop_front();
        }
        MarkFlipGpuComplete(*request);
    }
    fixture.CheckPending(full, true, 0);
    CapacityWaiter waiter(fixture.queue);
    const auto error = std::make_exception_ptr(std::runtime_error("retirement failure"));
    if (retirement == Retirement::Cancellation) {
        fixture.requests.front().reset();
    } else if (retirement == Retirement::Completion) {
        const VideoOutCompletionCallbacks callbacks{
            &fixture,
            [](void*) -> std::uint64_t { return 111; },
            [](void*) -> std::uint64_t { return 222; },
            [](void* context, VideoOutConfig&, std::int64_t argument) {
                auto& fixture = *static_cast<Fixture*>(context);
                ++fixture.events;
                fixture.eventArgument = argument;
            }};
        CompleteFlip(*request, callbacks);
        bool rejected = false;
        try { CompleteFlip(*request, callbacks); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "duplicate flip completion succeeded");
        Check(fixture.eventArgument == 123 && fixture.config->flipStatus.flipArg == 123 &&
              fixture.config->flipStatus.processTime == 111 && fixture.config->flipStatus.processTimeCounter == 222,
              "completion status/callback data changed");
    } else {
        request->Fail(error);
        request->Fail(std::make_exception_ptr(std::runtime_error("replacement failure")));
        Check(fixture.config->failure == error, "repeated failure replaced the original error");
    }
    const bool woke = waiter.Woke();
    const unsigned completed = retirement == Retirement::Completion;
    fixture.CheckPending(full - 1, false, completed);
    fixture.requests.front().reset();
    fixture.CheckPending(full - 1, false, completed);
    fixture.output->WaitForFlipRoom();
    if (retirement == Retirement::Failure) {
        bool propagated = false;
        try { renderingWait->Wait(); }
        catch (const std::runtime_error& failure) { propagated = std::string(failure.what()) == "retirement failure"; }
        Check(propagated, "rendering wait lost the original failure");
    } else {
        Check(!fixture.config->failure && !fixture.queue->failure, "retirement introduced a failure");
        renderingWait->Wait();
        bool rejected = false;
        try { fixture.output->Reserve({1, 1, VIDEO_OUT_FLIP_MODE_VSYNC, 0}); }
        catch (const std::runtime_error& failure) {
            rejected = std::string(failure.what()).find("buffer is not registered") != std::string::npos;
        }
        Check(rejected, "invalid reservation was not rejected for its missing buffer");
        fixture.CheckPending(full - 1, false, completed);
        auto replacement = fixture.output->Reserve({1, 0, VIDEO_OUT_FLIP_MODE_VSYNC, 456});
        fixture.CheckPending(full, true, completed);
        replacement.reset();
        fixture.CheckPending(full - 1, false, completed);
    }
    const char* name = retirement == Retirement::Cancellation ? "cancellation" :
                       retirement == Retirement::Completion ? "completion" : "failure";
    if (!woke) throw std::runtime_error(std::string(name) + " did not wake the capacity waiter");
    std::printf("%s: waiter woke, one reservation/ticket retired, successful flips/events=%u\n", name, completed);
}

}

int main() {
    try {
        // This standalone retirement fixture has no concurrent presenter. An inherited
        // APS5_SYNC_FLIP (even "0") blocks serial readiness before GPU completion;
        // concurrent presentation tests do not cover this fixture prerequisite.
        // Normalize before production caches the mode on its first GpuReady call.
#if defined(_WIN32)
        Check(_putenv_s("APS5_SYNC_FLIP", "") == 0, "could not unset APS5_SYNC_FLIP");
#else
        Check(unsetenv("APS5_SYNC_FLIP") == 0, "could not unset APS5_SYNC_FLIP");
#endif
        TestTicketOrdering();
        TestRetirement(Retirement::Completion);
        TestRetirement(Retirement::Failure);
        TestRetirement(Retirement::Cancellation);
        std::puts("Guest buffer reuse tests passed (ticket ordering + 3 retirement cases)");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
