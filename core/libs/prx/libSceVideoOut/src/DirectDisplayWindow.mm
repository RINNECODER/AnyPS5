// Fork-only: DisplayWindow for the Rosetta direct path (macOS x86_64), replacing the SDL window
// of DisplayWindow.cpp. AppKit windows exist only on the main thread, which the presentation
// thread cannot block on (the guest may be waiting on it), so:
//  - the window is the native host window (NativeHostWindow: AppKit window + CAMetalLayer), created
//    and pumped asynchronously on the main dispatch queue. The main thread stays in the AppKit run
//    loop of DirectHostApplication while the guest runs on its own thread (libkernel Aps5StartGuest);
//  - the window opens with VideoOut, titled with the game's title, sized to the output (scaled to
//    fit the screen) and centred; closing it or quitting (Cmd-Q) ends the game with exit code 0;
//  - until it exists, frames go to an offscreen CAMetalLayer. The Metal presenter (and
//    ANYPS5_FRAME_CAPTURE_DIR frame capture) work the same on either layer.
// ANYPS5_ROSETTA_HEADLESS=1 keeps every frame offscreen and never touches AppKit.
// SDL stays initialized for game controllers only; its video driver is the offscreen one, so the
// presentation thread's SDL event pump never calls into AppKit off the main thread.
// Keyboard and mouse input reach the window, not SDL: the main-queue pump drains the window's
// events, publishes them to libSceKeyboard/libSceMouse and pushes keys and focus loss into SDL's
// queue, where the presentation thread's PadInput maps them to pad buttons as on the SDL window.
#include "prx/libSceVideoOut/include/DisplayWindow.hpp"
#include "prx/libSceVideoOut/include/DirectHostApplication.hpp"
#include "prx/libSceVideoOut/include/NativeHostWindow.hpp"
#include "prx/libSceVideoOut/include/UnobtrusiveWindows.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "SDL_events.h"
#include "SDL_hints.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <variant>

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

void CloseWindow(std::unique_ptr<AnyPS5::Host::NativeHostWindow> window);

// Main queue only. SDL_PushEvent is thread-safe and drops events before SDL's queue exists.
void ForwardInput(const AnyPS5::Host::EventBatch& batch) {
    for (const auto& event : batch.events) {
        if (const auto* key = std::get_if<KeyboardInputEvent>(&event.payload)) {
            KeyboardPublishInput_nid_postfix(*key);
            if (key->keyCode == 0 || key->resetKeys || key->connectionChange) continue;
            SDL_Event sdl{};
            sdl.type = key->pressed ? SDL_KEYDOWN : SDL_KEYUP;
            sdl.key.state = key->pressed ? SDL_PRESSED : SDL_RELEASED;
            sdl.key.keysym.scancode = static_cast<SDL_Scancode>(key->keyCode);
            SDL_PushEvent(&sdl);
        } else if (const auto* mouse = std::get_if<MouseInputEvent>(&event.payload)) {
            MousePublishInput_nid_postfix(*mouse);
        } else if (const auto* focus = std::get_if<AnyPS5::Host::FocusChanged>(&event.payload); focus != nullptr && !focus->focused) {
            // PadInput releases every held key on focus loss.
            SDL_Event sdl{};
            sdl.type = SDL_WINDOWEVENT;
            sdl.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
            SDL_PushEvent(&sdl);
        }
    }
}

// Content size in points: the output at one point per backing pixel, scaled down to fit the screen.
NSSize FittedContentSize(std::uint32_t width, std::uint32_t height) {
    NSScreen* screen = NSScreen.mainScreen ?: NSScreen.screens.firstObject;
    if (screen == nil) return NSMakeSize(width, height);
    const CGFloat scale = screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 1;
    const NSRect visible = screen.visibleFrame;
    CGFloat fit = std::min<CGFloat>(1, std::min(visible.size.width * 0.9 * scale / width, (visible.size.height - 32) * 0.9 * scale / height));
    if (!(fit > 0)) fit = 1;
    return NSMakeSize(std::max<CGFloat>(160, std::floor(width * fit / scale)), std::max<CGFloat>(90, std::floor(height * fit / scale)));
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
        const NSSize content = FittedContentSize(width, height);
        AnyPS5::Host::WindowConfiguration configuration{AnyPS5::Host::DirectApplicationTitle(),
            static_cast<std::uint32_t>(content.width), static_cast<std::uint32_t>(content.height)};
        configuration.gameWindow = true;
        window = AnyPS5::Host::NativeHostWindow::CreateMainThread(configuration);
        // CreateMainThread's activateIgnoringOtherApps is only a request on macOS 14+.
        if (AnyPS5::Host::CurrentWindowEtiquette() != AnyPS5::Host::WindowEtiquette::Unobtrusive) {
            if (@available(macOS 14.0, *)) [NSApp activate];
        }
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
            ForwardInput(live->window->DrainEvents());
            // The close button or Quit (the window's application delegate turns it into a close request).
            if (live->window->Snapshot().closeRequested) AnyPS5::Host::EndDirectSession("the game window was closed");
        } catch (...) {
            Report("native window pump failed", std::current_exception());
            dispatch_source_cancel(live->pump);
        }
    });
    dispatch_source_set_timer(pump, dispatch_time(DISPATCH_TIME_NOW, 0), 4 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    {
        std::lock_guard lock(state->mutex);
        if (!state->destroyed) {
            state->window = std::move(window);
            state->pump = pump;
            dispatch_resume(pump);
            return;
        }
    }
    // Destroy ran while the window was being created: nobody else owns it, so close it here.
    dispatch_source_cancel(pump);
    dispatch_resume(pump);  // a suspended source must be resumed before it is released
    CloseWindow(std::move(window));
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
