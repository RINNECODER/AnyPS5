#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceVideoOut/include/BufferMetadata.hpp"
#include "prx/libSceVideoOut/include/BufferReuseTracker.hpp"
#include "prx/libc/include/Shutdown.hpp"

namespace AgcDriver {
class PerformanceTimer;
}

static constexpr int VIDEO_OUT_ERROR_INVALID_VALUE = -2144796671;
static constexpr int VIDEO_OUT_ERROR_INVALID_ADDRESS = -2144796670;
static constexpr int VIDEO_OUT_ERROR_INVALID_HANDLE = -2144796661;
static constexpr int VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE = -2144796660;
static constexpr int VIDEO_OUT_ERROR_INVALID_INDEX = -2144796662;
static constexpr int VIDEO_OUT_ERROR_INVALID_OPTION = -2144796646;
static constexpr int VIDEO_OUT_ERROR_INVALID_CATEGORY = -2144796643;
static constexpr int VIDEO_OUT_ERROR_SLOT_OCCUPIED = -2144796656;
static constexpr int VIDEO_OUT_ERROR_RESOURCE_BUSY = -2144796663;
static constexpr int VIDEO_OUT_ERROR_FLIP_QUEUE_FULL = -2144796654;
static constexpr int VIDEO_OUT_ERROR_UNSUPPORTED_OUTPUT_MODE = -2144796650;
static constexpr int VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE = -2144796647;
static constexpr int VIDEO_OUT_ERROR_INVALID_EVENT = -2144796659;
static constexpr int VIDEO_OUT_ERROR_UNKNOWN_OUTPUT_MODE = -2144796642;

static constexpr int VIDEO_OUT_BUS_TYPE_MAIN = 0;
static constexpr int VIDEO_OUT_BUS_TYPE_OVERLAY = 1;
static constexpr int VIDEO_OUT_BUS_TYPE_SUB = 2;

static constexpr uint32_t VIDEO_OUT_DEFAULT_WIDTH = 1920;
static constexpr uint32_t VIDEO_OUT_DEFAULT_HEIGHT = 1080;

static constexpr std::uint32_t VIDEO_OUT_OPEN_PARAM_FIRST_WORD = 16;
static constexpr std::int32_t VIDEO_OUT_SERVICE_THREAD_PRIORITY_HIGHEST = 256;
static constexpr std::int32_t VIDEO_OUT_SERVICE_THREAD_PRIORITY_LOWEST = 767;
static constexpr std::uint64_t VIDEO_OUT_SERVICE_THREAD_AFFINITY_ALL = 0x1FFF;

struct VideoOutOpenParam {
    std::uint32_t firstWord;
    std::uint32_t setPriority;
    std::int32_t priority;
    std::uint32_t setAffinity;
    std::uint64_t affinity;
};
static_assert(offsetof(VideoOutOpenParam, affinity) == 16);

static constexpr int VIDEO_OUT_BUFFER_NUM_MAX = 16;
static constexpr int VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX = 4;
static constexpr int VIDEO_OUT_NUM_MAX = 4;
static constexpr std::size_t VIDEO_OUT_FLIP_QUEUE_CAPACITY = 16;


static constexpr int VIDEO_OUT_EVENT_FLIP = 0;
static constexpr int VIDEO_OUT_EVENT_VBLANK = 1;
static constexpr int VIDEO_OUT_EVENT_PRE_VBLANK_START = 2;
static constexpr int VIDEO_OUT_EVENT_SET_MODE = 8;
static constexpr int VIDEO_OUT_EVENT_VRR_STATUS = 16;

static constexpr int VIDEO_OUT_FLIP_MODE_VSYNC = 1;
static constexpr int VIDEO_OUT_FLIP_MODE_VSYNC_MULTI = 4;

static constexpr int VIDEO_OUT_BUFFER_INDEX_BLACK = -2;
static constexpr int VIDEO_OUT_BUFFER_INDEX_BLANK = -1;

static constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_DEFAULT = 0x0000000000000001ULL;
static constexpr uint64_t VIDEO_OUT_OUTPUT_MODE_119_88HZ = 0x000000000000000FULL;

static constexpr uint64_t VIDEO_OUT_REFRESH_RATE_59_94HZ = 3;
static constexpr uint64_t VIDEO_OUT_REFRESH_RATE_119_88HZ = 13;

struct EventRegistration {
    KernelEqueue eq = 0;
    uint64_t generation = 0;
};

struct VideoOutConfig {
    std::mutex mutex;
    std::condition_variable_any vblankCond;

    std::vector<EventRegistration> flipEvents;
    std::vector<EventRegistration> vblankEvents;
    std::vector<EventRegistration> preVblankEvents;
    std::vector<EventRegistration> outputModeEvents;
    std::vector<EventRegistration> vrrStatusEvents;

    uint32_t width = VIDEO_OUT_DEFAULT_WIDTH;
    uint32_t height = VIDEO_OUT_DEFAULT_HEIGHT;
    uint64_t generation = 0;
    int busType = VIDEO_OUT_BUS_TYPE_MAIN;
    bool opened = false;
    bool closing = false;
    std::exception_ptr failure;
    std::stop_token shutdownToken;
    explicit VideoOutConfig(std::stop_token token) : shutdownToken(token) {}
    int flipRate = 0;
    uint64_t lastFlipVblank = 0;
    std::chrono::steady_clock::time_point lastTimingFlip{};
    uint64_t outputMode = VIDEO_OUT_OUTPUT_MODE_DEFAULT;
    float gamma = 1.0f;

    VideoOutFlipStatus flipStatus{};
    VideoOutVblankStatus vblankStatus{};
    VideoOutVblankStatus preVblankStatus{};

    std::array<VideoOutBuffer, VIDEO_OUT_BUFFER_NUM_MAX> buffers{};
    std::array<uint32_t, VIDEO_OUT_BUFFER_NUM_MAX> bufferPending{};
    std::array<BufferReuseTracker, VIDEO_OUT_BUFFER_NUM_MAX> bufferReuse;
    std::array<BufferAttributeGroup, VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX> groups{};

    bool Closed() const { return !opened || closing; }

    void CheckAlive() const {
        if (failure) std::rethrow_exception(failure);
        if (shutdownToken.stop_requested()) throw ProcessShutdown{};
    }

    void Check() const {
        CheckAlive();
        if (Closed()) throw std::runtime_error("VideoOut: port is closed");
    }
};

struct FlipQueue;

struct FlipRequest final : AgcDriver::IFlipRequest, std::enable_shared_from_this<FlipRequest> {
    std::uint64_t reuseTicket = 0;
    std::shared_ptr<VideoOutConfig> cfg;
    std::shared_ptr<FlipQueue> queue;
    uint64_t generation = 0;
    std::uint32_t outputHandle = 0;
    int index = 0;
    int flipMode = 0;
    int flipRate = 0;
    int64_t flipArg = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VideoOutBuffer buffer;
    BufferAttributeGroup group;
    bool reserved = false;
    bool unregistered = false;
    bool ready = false;
    bool gpuComplete = false;
    bool terminal = false;

    std::shared_ptr<AgcDriver::FrameTiming> timing;
    std::chrono::steady_clock::time_point queuedAt;

    ~FlipRequest() override;
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>& frameTiming) override;
    void Fail(std::exception_ptr error) noexcept override;
    void Cancel() noexcept;
    void ReleaseLocked() noexcept;
};

struct FlipQueue {
    std::mutex mutex;
    std::condition_variable changed;
    std::list<std::shared_ptr<FlipRequest>> requests;
    std::atomic<std::size_t> reservations = 0;
    std::exception_ptr failure;
    bool stopping = false;
};

struct VideoOutCompletionCallbacks {
    void* context = nullptr;
    std::uint64_t (*processTime)(void*) = nullptr;
    std::uint64_t (*processTimeCounter)(void*) = nullptr;
    void (*flipEvent)(void*, VideoOutConfig&, std::int64_t) = nullptr;
};

std::shared_ptr<AgcDriver::IVideoOutput> CreateVideoOutput(
    std::shared_ptr<VideoOutConfig> config, std::shared_ptr<FlipQueue> queue);
// false when the title closed the port: the flip was released and must not be presented.
bool WaitForFlipVblank(FlipRequest& request, AgcDriver::PerformanceTimer* timing = nullptr);
void MarkFlipGpuComplete(FlipRequest& request);
void CompleteFlip(FlipRequest& request, const VideoOutCompletionCallbacks& callbacks,
                  AgcDriver::PerformanceTimer* timing = nullptr);
