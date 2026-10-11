#ifndef CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_VIDEOOUTDRIVER_HPP
#define CORE_LIBS_PRX_LIBSCEVIDEOOUT_INCLUDE_VIDEOOUTDRIVER_HPP

#include <future>
#include <thread>
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"

class VideoOutDriver {
public:
    static VideoOutDriver& Get();

    VideoOutDriver();
    ~VideoOutDriver();
    void Shutdown();

    VideoOutDriver(const VideoOutDriver&) = delete;
    VideoOutDriver& operator=(const VideoOutDriver&) = delete;

    int Open(int busType);
    bool Close(int handle);
    std::shared_ptr<VideoOutConfig> GetConfig(int handle);
    bool IsOpen(int handle);
    bool HasConfig(int handle);

    // 0, or VIDEO_OUT_ERROR_FLIP_QUEUE_FULL when the title has VIDEO_OUT_FLIP_QUEUE_CAPACITY flips pending.
    int SubmitFlip(int handle, int index, int flipMode, int64_t flipArg);

private:
    bool close(int handle);
    void presentLoop(std::stop_token token, std::promise<void>& started);
    void vblankLoop(std::stop_token token);
    void vblankEnd();
    void processFlip(FlipRequest& req);
    void triggerEvents(VideoOutConfig& cfg, int eventKind, void* triggerData);

    std::mutex mutex;
    std::mutex shutdownMutex;
    bool stopped = false;
    std::array<std::shared_ptr<VideoOutConfig>, VIDEO_OUT_NUM_MAX> contexts;
    std::array<std::shared_ptr<AgcDriver::IVideoOutput>, VIDEO_OUT_NUM_MAX> outputs;
    std::shared_ptr<FlipQueue> flipQueue = std::make_shared<FlipQueue>();

    DisplayWindow window;

    std::jthread presentThread;
    std::jthread vblankThread;
};

#endif
