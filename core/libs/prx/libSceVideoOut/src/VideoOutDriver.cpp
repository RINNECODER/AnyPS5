#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <thread>
#include <limits>
#include <stdexcept>
#include <string>

#include "SDL.h"
#if !defined(ANYPS5_METAL_BACKEND)
#include "SDL_vulkan.h"
#endif
#include "prx/libSceVideoOut/include/PadInput.hpp"
#include "prx/libSceVideoOut/include/MouseInput.hpp"
#include "prx/libSceVideoOut/include/KeyboardInput.hpp"
#include "prx/libScePad/include/PadState.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include "prx/libSceVideoOut/include/VideoOutDriver.hpp"
#if APS5_ENABLE_TIMING_LOG
#include "prx/libSceVideoOut/include/FrameTimingLog.hpp"
#endif
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/PreciseWait.hpp"
#include "prx/libc/include/Shutdown.hpp"

namespace {

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("VideoOut: ") + reason);
}

#if !defined(ANYPS5_METAL_BACKEND)
std::vector<const char*> windowExtensions(SDL_Window* window) {
    unsigned extensionCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, nullptr)) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    std::vector<const char*> extensions(extensionCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extensionCount, extensions.data())) throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
    extensions.resize(extensionCount);
    return extensions;
}

VkSurfaceKHR createWindowSurface(void* context, VkInstance instance) {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(context), instance, &surface)) throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
    return surface;
}

void windowDrawableSize(void* context, std::uint32_t* width, std::uint32_t* height) {
    if ((SDL_GetWindowFlags(static_cast<SDL_Window*>(context)) & SDL_WINDOW_MINIMIZED) != 0) {
        *width = 0;
        *height = 0;
        return;
    }
    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(context), &drawableWidth, &drawableHeight);
    *width = drawableWidth > 0 ? static_cast<std::uint32_t>(drawableWidth) : 0;
    *height = drawableHeight > 0 ? static_cast<std::uint32_t>(drawableHeight) : 0;
}
#endif

void checkConfig(const VideoOutConfig& cfg) {
    cfg.Check();
}

}

VideoOutDriver& VideoOutDriver::Get() {
    static VideoOutDriver instance;
    return instance;
}

VideoOutDriver::VideoOutDriver() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
        throw std::runtime_error(std::string("SDL_InitSubSystem(VIDEO | GAMECONTROLLER) failed: ") + SDL_GetError());
    }
    try {
        AgcDriverWaitIdle_nid_postfix();
        std::promise<void> started;
        auto attached = started.get_future();
        presentThread = std::jthread([this, &started](std::stop_token token) { presentLoop(token, started); });
        attached.get();
        vblankThread = std::jthread([this](std::stop_token token) { vblankLoop(token); });
        LibcRegisterShutdown_nid_postfix([] { VideoOutDriver::Get().Shutdown(); });
    } catch (...) {
        if (vblankThread.joinable()) {
            vblankThread.request_stop();
            flipQueue->changed.notify_all();
            vblankThread.join();
        }
        if (presentThread.joinable()) {
            presentThread.request_stop();
            flipQueue->changed.notify_all();
            presentThread.join();
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        throw;
    }
}

VideoOutDriver::~VideoOutDriver() {
    if (!stopped) Shutdown();
}

void VideoOutDriver::Shutdown() {
    require(std::this_thread::get_id() != presentThread.get_id(), "presentation thread cannot stop itself");
    std::lock_guard shutdownLock(shutdownMutex);
    if (stopped) return;
    LibcRequestShutdown_nid_postfix();
    {
        std::lock_guard queueLock(flipQueue->mutex);
        flipQueue->stopping = true;
    }
    presentThread.request_stop();
    vblankThread.request_stop();
    flipQueue->changed.notify_all();
    presentThread.join();
    vblankThread.join();
    {
        std::lock_guard lock(mutex);
        for (int handle = 1; handle < VIDEO_OUT_NUM_MAX; ++handle) {
            if (outputs[handle]) close(handle);
        }
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    stopped = true;
    std::lock_guard lock(flipQueue->mutex);
    if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
}

int VideoOutDriver::Open(int busType) {
    std::lock_guard lock(mutex);
    {
        std::lock_guard queueLock(flipQueue->mutex);
        if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
        if (flipQueue->stopping || LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    }
    const int handle = busType + 1;
    require(handle > 0 && handle < VIDEO_OUT_NUM_MAX, "invalid output bus");
    auto previous = contexts[handle];
    uint64_t generation = 1;
    if (previous) {
        std::lock_guard cfgLock(previous->mutex);
        require(!previous->opened, "port already open");
        require(previous->generation != std::numeric_limits<uint64_t>::max(), "port generation overflow");
        generation = previous->generation + 1;
    }
    auto cfg = std::make_shared<VideoOutConfig>(LibcShutdownToken_nid_postfix());
    cfg->generation = generation;
    cfg->busType = busType;
    cfg->opened = true;
    cfg->flipStatus.flipArg = -1;
    cfg->flipStatus.currentBuffer = -1;
    auto output = CreateVideoOutput(cfg, flipQueue);
    AgcDriverRegisterVideoOutput_nid_postfix(static_cast<uint32_t>(handle), output);
    contexts[handle] = std::move(cfg);
    outputs[handle] = std::move(output);
    return handle;
}

bool VideoOutDriver::Close(int handle) {
    std::lock_guard lock(mutex);
    if (LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    return close(handle);
}

bool VideoOutDriver::close(int handle) {
    require(handle > 0 && handle < VIDEO_OUT_NUM_MAX && outputs[handle] != nullptr, "invalid close handle");
    AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<uint32_t>(handle), outputs[handle]);
    outputs[handle].reset();
    auto cfg = contexts[handle];
    std::lock_guard cfgLock(cfg->mutex);
    cfg->opened = false;
    cfg->closing = true;
    const auto removeEvents = [](const auto& events, int kind) {
        for (const auto& event : events) {
            const auto result = EqueueDeleteEvent_nid_postfix(event.eq, static_cast<uintptr_t>(kind), EVFILT_VIDEO_OUT);
            require(result == EQUEUE_OK || result == SCE_KERNEL_ERROR_EBADF || result == SCE_KERNEL_ERROR_ENOENT, "event removal during close failed");
        }
    };
    removeEvents(cfg->flipEvents, VIDEO_OUT_EVENT_FLIP);
    removeEvents(cfg->vblankEvents, VIDEO_OUT_EVENT_VBLANK);
    removeEvents(cfg->preVblankEvents, VIDEO_OUT_EVENT_PRE_VBLANK_START);
    removeEvents(cfg->outputModeEvents, VIDEO_OUT_EVENT_SET_MODE);
    removeEvents(cfg->vrrStatusEvents, VIDEO_OUT_EVENT_VRR_STATUS);
    cfg->flipEvents.clear();
    cfg->vblankEvents.clear();
    cfg->preVblankEvents.clear();
    cfg->outputModeEvents.clear();
    cfg->vrrStatusEvents.clear();
    cfg->vblankCond.notify_all();
    flipQueue->changed.notify_all();
    return true;
}

std::shared_ptr<VideoOutConfig> VideoOutDriver::GetConfig(int handle) {
    {
        std::lock_guard lock(flipQueue->mutex);
        if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
        if (flipQueue->stopping || LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    }
    std::lock_guard lock(mutex);
    require(handle > 0 && handle < VIDEO_OUT_NUM_MAX && contexts[handle] != nullptr, "invalid output handle");
    auto cfg = contexts[handle];
    std::lock_guard cfgLock(cfg->mutex);
    checkConfig(*cfg);
    return cfg;
}

bool VideoOutDriver::IsOpen(int handle) {
    return GetConfig(handle) != nullptr;
}

bool VideoOutDriver::HasConfig(int handle) {
    std::shared_ptr<VideoOutConfig> cfg;
    {
        std::lock_guard lock(mutex);
        if (handle <= 0 || handle >= VIDEO_OUT_NUM_MAX || contexts[handle] == nullptr) return false;
        cfg = contexts[handle];
    }
    std::lock_guard cfgLock(cfg->mutex);
    return cfg->opened && !cfg->closing;
}

int VideoOutDriver::SubmitFlip(int handle, int index, int flipMode, int64_t flipArg) {
    if (LibcShutdownToken_nid_postfix().stop_requested()) throw ProcessShutdown{};
    // A title that does not pace on flipPendingNum can run ahead of the presenter now that the queue
    // worker no longer waits per flip; a full queue is the documented error, not a failure.
    if (flipQueue->reservations.load() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    std::array<uint32_t, AgcDriver::FlipPacketWords> words{AgcDriver::FlipPacketHeader, static_cast<uint32_t>(handle), static_cast<uint32_t>(index), static_cast<uint32_t>(flipMode), static_cast<uint32_t>(static_cast<uint64_t>(flipArg)), static_cast<uint32_t>(static_cast<uint64_t>(flipArg) >> 32u)};
    Packet packet{words.data(), static_cast<uint32_t>(words.size()), 0, {}};
    const auto result = sceAgcDriverSubmitDcb(&packet);
    require(result == 0, "driver rejected flip submission");
    return 0;
}

void VideoOutDriver::triggerEvents(VideoOutConfig& cfg, int eventKind, void* triggerData) {
    std::vector<EventRegistration>* events = nullptr;
    if (eventKind == VIDEO_OUT_EVENT_FLIP) events = &cfg.flipEvents;
    else if (eventKind == VIDEO_OUT_EVENT_VBLANK) events = &cfg.vblankEvents;
    else if (eventKind == VIDEO_OUT_EVENT_PRE_VBLANK_START) events = &cfg.preVblankEvents;
    else if (eventKind == VIDEO_OUT_EVENT_SET_MODE) events = &cfg.outputModeEvents;
    else throw std::runtime_error("VideoOut: unknown event kind");
    for (auto it = events->begin(); it != events->end();) {
        require(it->generation == cfg.generation, "stale event registration");
        const auto result = EqueueTriggerEvent_nid_postfix(it->eq, static_cast<uintptr_t>(eventKind), EVFILT_VIDEO_OUT, triggerData);
        if (result == SCE_KERNEL_ERROR_EBADF || result == SCE_KERNEL_ERROR_ENOENT) it = events->erase(it);
        else {
            require(result == EQUEUE_OK, "event delivery failed");
            ++it;
        }
    }
}

void VideoOutDriver::vblankEnd() {
    std::lock_guard lock(mutex);
    for (const auto& cfg : contexts) {
        if (!cfg) continue;
        std::lock_guard cfgLock(cfg->mutex);
        if (!cfg->opened || cfg->failure) continue;
        require(cfg->vblankStatus.count != std::numeric_limits<uint64_t>::max(), "vblank counter overflow");
        ++cfg->preVblankStatus.count;
        cfg->preVblankStatus.processTime = sceKernelGetProcessTime();
        cfg->preVblankStatus.processTimeCounter = sceKernelGetProcessTimeCounter();
        triggerEvents(*cfg, VIDEO_OUT_EVENT_PRE_VBLANK_START, reinterpret_cast<void*>(cfg->preVblankStatus.count));
        ++cfg->vblankStatus.count;
        cfg->vblankStatus.processTime = sceKernelGetProcessTime();
        cfg->vblankStatus.processTimeCounter = sceKernelGetProcessTimeCounter();
        triggerEvents(*cfg, VIDEO_OUT_EVENT_VBLANK, reinterpret_cast<void*>(cfg->vblankStatus.count));
        cfg->vblankCond.notify_all();
    }
}

void VideoOutDriver::processFlip(FlipRequest& req) {
    AgcDriver::PerformanceContext timingContext(req.timing.get());
    AgcDriver::PerformanceTimer timing("VideoOut.Flip");
    // false: the title closed the port; the flip was released without presenting.
    if (!WaitForFlipVblank(req, &timing)) return;
    require(req.width != 0 && req.height != 0 && req.width <= static_cast<uint32_t>(std::numeric_limits<int>::max()) && req.height <= static_cast<uint32_t>(std::numeric_limits<int>::max()), "invalid window dimensions");
    if (req.cfg->busType == VIDEO_OUT_BUS_TYPE_OVERLAY) {
        // Overlay-bus flips complete without replacing the main output.
        MarkFlipGpuComplete(req);
    } else {
        window.Ensure(req.width, req.height);
#if defined(ANYPS5_METAL_BACKEND)
        const AgcDriver::PresentationWindow target{&window, {}, nullptr, [](void* context, std::uint32_t* width, std::uint32_t* height) {
            static_cast<DisplayWindow*>(context)->DrawableSize(*width, *height);
        }, req.width, req.height, req.timing, [](void* context) {
            return static_cast<DisplayWindow*>(context)->MetalLayer();
        }};
#else
        const auto extensions = windowExtensions(window.Handle());
        const AgcDriver::PresentationWindow target{window.Handle(), extensions, &createWindowSurface, &windowDrawableSize, req.width, req.height, req.timing};
#endif
        timing.Mark("window_prepare");
        const auto gpuReady = [](void* context) {
            auto& request = *static_cast<FlipRequest*>(context);
            MarkFlipGpuComplete(request);
        };
        if (req.index >= 0 && !req.unregistered) {
            const auto display = DescribeVideoOutBuffer(req.buffer, req.group);
            AgcDriverPresentBuffer_nid_postfix(target, display, gpuReady, &req);
        } else {
            AgcDriverPresentClear_nid_postfix(target, req.unregistered || req.index == VIDEO_OUT_BUFFER_INDEX_BLACK, gpuReady, &req);
        }
        timing.Mark("present");
        window.UpdateTitle();
        timing.Mark("window_title");
    }
    const VideoOutCompletionCallbacks callbacks{
        this,
        [](void*) { return sceKernelGetProcessTime(); },
        [](void*) { return sceKernelGetProcessTimeCounter(); },
        [](void* context, VideoOutConfig& cfg, std::int64_t argument) {
            static_cast<VideoOutDriver*>(context)->triggerEvents(cfg, VIDEO_OUT_EVENT_FLIP,
                reinterpret_cast<void*>(argument));
        }};
    CompleteFlip(req, callbacks, &timing);
}

void VideoOutDriver::presentLoop(std::stop_token token, std::promise<void>& started) {
    try {
        window.Ensure(VIDEO_OUT_DEFAULT_WIDTH, VIDEO_OUT_DEFAULT_HEIGHT);
#if !defined(ANYPS5_METAL_BACKEND)
        // The Metal backend binds its layer on the first present instead.
        const auto extensions = windowExtensions(window.Handle());
        AgcDriverAttachWindow_nid_postfix({window.Handle(), extensions, &createWindowSurface, &windowDrawableSize, VIDEO_OUT_DEFAULT_WIDTH, VIDEO_OUT_DEFAULT_HEIGHT, nullptr});
#endif
    } catch (...) {
        window.Destroy();
        started.set_exception(std::current_exception());
        return;
    }
    started.set_value();
    std::shared_ptr<FlipRequest> current;
#if APS5_ENABLE_TIMING_LOG
    std::unique_ptr<FrameTimingLog> timingLog;
#endif
    try {
#if APS5_ENABLE_TIMING_LOG
        timingLog = std::make_unique<FrameTimingLog>();
#endif
        PadInput padInput;
        MouseInput mouseInput;
        KeyboardInput keyboardInput;
        while (!token.stop_requested()) {
            {
                std::unique_lock lock(flipQueue->mutex);
                flipQueue->changed.wait_for(lock, std::chrono::milliseconds(10), [&] { return token.stop_requested() || flipQueue->failure || !flipQueue->requests.empty(); });
                if (flipQueue->failure) std::rethrow_exception(flipQueue->failure);
                if (token.stop_requested()) break;
                if (!flipQueue->requests.empty()) {
                    current = std::move(flipQueue->requests.front());
                    flipQueue->requests.pop_front();
                }
            }

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    LibcRequestExit_nid_postfix(0);
                    throw ProcessShutdown{};
                }
                padInput.HandleEvent(event, window);
                if (window.Handle() != nullptr) {
                    mouseInput.HandleEvent(event, SDL_GetWindowID(window.Handle()));
                    keyboardInput.HandleEvent(event, SDL_GetWindowID(window.Handle()));
                }
            }
            padInput.Update();
            if (current) {
                require(current->timing != nullptr, "missing presentation timing");
                const auto dequeued = AgcDriver::FrameTiming::Clock::now();
                current->timing->Add(current->timing->Get("VideoOut", "queue"), dequeued - current->queuedAt);
                processFlip(*current);
                const auto finished = AgcDriver::FrameTiming::Clock::now();
                AgcDriver::FrameTiming::Clock::duration interval{};
                {
                    std::lock_guard lock(current->cfg->mutex);
                    const auto previous = current->cfg->lastTimingFlip;
                    if (previous != AgcDriver::FrameTiming::Clock::time_point{}) interval = finished - previous;
                    current->cfg->lastTimingFlip = finished;
                }
#if APS5_ENABLE_TIMING_LOG
                timingLog->Enqueue(current->timing->Capture(current->outputHandle, current->index, current->flipArg, finished, interval));
#endif
            }
            if (current) {
                current.reset();
                flipQueue->changed.notify_all();
            }
        }
    } catch (const ProcessShutdown&) {
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] presentation failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    } catch (...) {
        std::fprintf(stderr, "[videoout] presentation failed with a non-standard exception\n");
        std::fflush(stderr);
        std::terminate();
    }
#if APS5_ENABLE_TIMING_LOG
    try {
        timingLog->Finish();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] frame timing log failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    }
#endif
    current.reset();
    {
        std::list<std::shared_ptr<FlipRequest>> cancelled;
        {
            std::lock_guard lock(flipQueue->mutex);
            cancelled.swap(flipQueue->requests);
        }
    }
#if defined(ANYPS5_METAL_BACKEND)
    AgcDriverReleaseWindow_nid_postfix(&window);
#endif
    try {
        AgcDriverShutdown_nid_postfix();
    } catch (...) {
        std::lock_guard lock(flipQueue->mutex);
        if (!flipQueue->failure) flipQueue->failure = std::current_exception();
    }
    window.Destroy();
}

void VideoOutDriver::vblankLoop(std::stop_token token) {
    using Frame = std::chrono::duration<int64_t, std::ratio<1001, 60000>>;
    const auto start = std::chrono::steady_clock::now();
    try {
        for (int64_t frame = 1; !token.stop_requested(); ++frame) {
            const auto next = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(Frame(frame));
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(next - std::chrono::steady_clock::now()).count();
            if (remaining > 0) PreciseSleepUs(static_cast<unsigned long long>(remaining));
            {
                std::lock_guard lock(flipQueue->mutex);
                if (token.stop_requested() || flipQueue->failure) return;
            }
            vblankEnd();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[videoout] vblank failed: %s\n", error.what());
        std::fflush(stderr);
        std::terminate();
    } catch (...) {
        std::fprintf(stderr, "[videoout] vblank failed with a non-standard exception\n");
        std::fflush(stderr);
        std::terminate();
    }
}
