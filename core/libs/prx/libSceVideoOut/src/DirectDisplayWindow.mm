// Fork-only: DisplayWindow for the Rosetta direct path (macOS x86_64), replacing the SDL window
// of DisplayWindow.cpp. AppKit windows exist only on the main thread, which the presentation
// thread cannot block on (the guest may be waiting on it), so:
//  - the window is the native host window (NativeHostWindow: AppKit window + CAMetalLayer), created
//    and pumped asynchronously on the main dispatch queue, which the direct-path launcher must
//    service (the guest runs off the main thread, as in the TCG engine's native runner);
//  - until it exists, or after it is closed, frames go to an offscreen CAMetalLayer. The Metal
//    presenter (and ANYPS5_FRAME_CAPTURE_DIR frame capture) work the same on either layer.
// ANYPS5_ROSETTA_HEADLESS=1 keeps every frame offscreen and never touches AppKit.
// SDL stays initialized for game controllers only; its video driver is the offscreen one, so the
// presentation thread's SDL event pump never calls into AppKit off the main thread.
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceVideoOut/include/NativeHostWindow.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "SDL_hints.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>

namespace {

struct DirectWindow {
    std::mutex mutex;
    CAMetalLayer* offscreen = nil;
    std::unique_ptr<AnyPS5::Host::NativeHostWindow> window;
    dispatch_source_t pump = nil;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool destroyed = false;
};

bool Headless() {
    const char* value = std::getenv("ANYPS5_ROSETTA_HEADLESS");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

__attribute__((constructor)) void UseOffscreenSdlVideo() {
    if (std::getenv("SDL_VIDEODRIVER") == nullptr) SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
}

void Report(const char* what, const std::exception_ptr& error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[videoout] %s: %s; presenting offscreen\n", what, exception.what());
    } catch (...) {
        std::fprintf(stderr, "[videoout] %s; presenting offscreen\n", what);
    }
}

// Main queue only. The window keeps its own copy of the state pointer alive.
void CreateWindow(const std::shared_ptr<DirectWindow>& state) {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    {
        std::lock_guard lock(state->mutex);
        if (state->destroyed || state->window) return;
        width = state->width;
        height = state->height;
    }
    std::unique_ptr<AnyPS5::Host::NativeHostWindow> window;
    try {
        window = AnyPS5::Host::NativeHostWindow::CreateMainThread({"AnyPS5", width, height});
    } catch (...) {
        Report("cannot create the native window", std::current_exception());
        return;
    }
    dispatch_source_t pump = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    std::weak_ptr<DirectWindow> weak = state;
    dispatch_source_set_event_handler(pump, ^{
        const auto live = weak.lock();
        if (!live) return;
        std::lock_guard lock(live->mutex);
        if (!live->window) return;
        try {
            live->window->PumpMainThread(std::chrono::milliseconds(0));
        } catch (...) {
            Report("native window pump failed", std::current_exception());
            dispatch_source_cancel(live->pump);
        }
    });
    dispatch_source_set_timer(pump, dispatch_time(DISPATCH_TIME_NOW, 0), 4 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    std::lock_guard lock(state->mutex);
    state->window = std::move(window);
    state->pump = pump;
    dispatch_resume(pump);
}

// Main queue only.
void CloseWindow(std::unique_ptr<AnyPS5::Host::NativeHostWindow> window) {
    if (!window) return;
    try {
        window->RequestCloseMainThread();
        window->CloseAfterGpuDrainMainThread();
    } catch (...) {
        Report("native window close failed", std::current_exception());
        // A window that failed to close must not run the destructor's open-window check.
        static_cast<void>(window.release());
    }
}

// The state lives in the metalView slot, which the SDL window of DisplayWindow.cpp would use.
std::shared_ptr<DirectWindow>* Slot(SDL_MetalView view) { return static_cast<std::shared_ptr<DirectWindow>*>(view); }

// A presentation view of the live window, or nothing when frames go offscreen.
bool WindowPresentation(DirectWindow& state, AgcDriver::PresentationWindow& presentation) {
    if (!state.window) return false;
    try {
        presentation = state.window->Presentation(state.width, state.height);
        return true;
    } catch (const std::exception&) {
        return false;  // closed or close requested: keep presenting offscreen
    }
}

}

DisplayWindow::~DisplayWindow() { Destroy(); }

void DisplayWindow::Ensure(std::uint32_t sourceWidth, std::uint32_t sourceHeight) {
    if (sourceWidth == 0 || sourceHeight == 0) throw std::invalid_argument("display window extent is empty");
    if (metalView == nullptr) metalView = new std::shared_ptr<DirectWindow>(std::make_shared<DirectWindow>());
    const auto state = *Slot(metalView);
    {
        std::lock_guard lock(state->mutex);
        state->width = sourceWidth;
        state->height = sourceHeight;
        if (state->offscreen == nil) {
            state->offscreen = [CAMetalLayer layer];
            state->offscreen.framebufferOnly = NO;
        }
        state->offscreen.drawableSize = CGSizeMake(sourceWidth, sourceHeight);
        if (state->window || Headless()) return;
    }
    aspectWidth = sourceWidth;
    aspectHeight = sourceHeight;
    if ([NSThread isMainThread]) {
        CreateWindow(state);
    } else {
        dispatch_async(dispatch_get_main_queue(), ^{ CreateWindow(state); });
    }
}

void DisplayWindow::Destroy() noexcept {
    if (metalView == nullptr) return;
    auto* slot = Slot(metalView);
    metalView = nullptr;
    const auto state = std::move(*slot);
    delete slot;
    std::unique_ptr<AnyPS5::Host::NativeHostWindow> window;
    dispatch_source_t pump = nil;
    {
        std::lock_guard lock(state->mutex);
        state->destroyed = true;
        window = std::move(state->window);
        pump = state->pump;
        state->pump = nil;
    }
    if (pump != nil) dispatch_source_cancel(pump);
    if (!window) return;
    if ([NSThread isMainThread]) {
        CloseWindow(std::move(window));
    } else {
        auto* owned = window.release();
        dispatch_async(dispatch_get_main_queue(), ^{ CloseWindow(std::unique_ptr<AnyPS5::Host::NativeHostWindow>(owned)); });
    }
}

SDL_Window* DisplayWindow::Handle() const {
    return window;
}

void* DisplayWindow::MetalLayer() const {
    if (metalView == nullptr) throw std::logic_error("display window must exist before querying its layer");
    auto& state = **Slot(metalView);
    std::lock_guard lock(state.mutex);
    AgcDriver::PresentationWindow presentation{};
    if (WindowPresentation(state, presentation)) return presentation.metalLayer(presentation.context);
    return (__bridge void*)state.offscreen;
}

void DisplayWindow::DrawableSize(std::uint32_t& width, std::uint32_t& height) const {
    if (metalView == nullptr) throw std::logic_error("display window must exist before querying its size");
    auto& state = **Slot(metalView);
    std::lock_guard lock(state.mutex);
    AgcDriver::PresentationWindow presentation{};
    if (WindowPresentation(state, presentation)) {
        presentation.getDrawableSize(presentation.context, &width, &height);
        if (width != 0 && height != 0) return;
    }
    const auto size = state.offscreen.drawableSize;
    width = static_cast<std::uint32_t>(size.width);
    height = static_cast<std::uint32_t>(size.height);
}

void DisplayWindow::UpdateTitle() {}

void DisplayWindow::ToggleFullscreen() {}
