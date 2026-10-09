#include "prx/libSceVideoOut/include/NativeHostWindow.hpp"
#include "prx/libSceVideoOut/include/UnobtrusiveWindows.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "SDL_scancode.h"
#include "../src/events/scancodes_darwin.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <Carbon/Carbon.h>
#include <IOKit/hidsystem/ev_keymap.h>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace AnyPS5::Host {
namespace {

bool nativeWindowLive = false;

void requireMainThread() {
    if (![NSThread isMainThread]) throw std::logic_error("Native host window requires the AppKit main thread");
}

std::uint64_t nextIdentity() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load();
    for (;;) {
        if (value == 0 || value == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Native host window identity exhausted");
        if (next.compare_exchange_weak(value, value + 1)) return value;
    }
}

std::int32_t integerDelta(double value) {
    if (!std::isfinite(value) || value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
        throw std::overflow_error("Native host mouse delta exceeds the input representation");
    return static_cast<std::int32_t>(value);
}

bool modifierPressed(NSUInteger flags, NSUInteger target, NSUInteger other, NSUInteger either) {
    const bool left = (flags & target) != 0;
    const bool right = (flags & other) != 0;
    const bool both = (flags & either) != 0;
    return both != (left || right) ? both : left;
}

struct HostWindowState {
    mutable std::mutex mutex;
    WindowSnapshot snapshot;
    std::vector<InputEvent> pending;
    std::exception_ptr failure;
    std::thread::id consumer;
    bool hasConsumer = false;
    bool ownsWindowSlot = false;
    double wheelX = 0, wheelY = 0;
    NSWindow* window = nil;
    CAMetalLayer* layer = nil;
    id windowDelegate = nil;
    id applicationDelegate = nil;
    id previousApplicationDelegate = nil;
    id applicationInactiveObserver = nil;
    id applicationActiveObserver = nil;

    void check() const {
        if (failure) std::rethrow_exception(failure);
    }

    void fail(std::exception_ptr error) noexcept {
        std::lock_guard lock(mutex);
        if (!failure) failure = std::move(error);
    }

    template<class T> void append(T payload) {
        check();
        if (pending.size() >= 65536 || snapshot.lastSequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Native host input event queue or sequence exhausted");
        const auto sequence = snapshot.lastSequence + 1;
        pending.push_back({snapshot.window, sequence, std::chrono::steady_clock::now(), std::move(payload)});
        snapshot.lastSequence = sequence;
    }

    void key(std::uint16_t code, bool pressed) {
        if (snapshot.heldKeys[code] == pressed) return;
        KeyboardInputEvent event;
        event.keyCode = code;
        event.pressed = pressed;
        event.led = snapshot.keyboardLed;
        append(event);
        snapshot.heldKeys[code] = pressed;
    }

    void resets() {
        KeyboardInputEvent keyboard;
        keyboard.resetKeys = true;
        MouseInputEvent mouse;
        mouse.resetButtons = true;
        append(keyboard);
        append(mouse);
        snapshot.heldKeys.fill(false);
        snapshot.heldMouseButtons = 0;
        wheelX = wheelY = 0;
    }

    void focus(bool focused) {
        std::lock_guard lock(mutex);
        check();
        if (!snapshot.open || snapshot.closeRequested || snapshot.focused == focused) return;
        if (!focused) resets();
        else {
            KeyboardInputEvent keyboard;
            keyboard.connectionChange = true;
            MouseInputEvent mouse;
            mouse.connectionChange = true;
            append(keyboard);
            append(mouse);
        }
        append(FocusChanged{focused});
        snapshot.focused = focused;
    }

    void closeRequest() {
        std::lock_guard lock(mutex);
        check();
        if (!snapshot.open || snapshot.closeRequested) return;
        resets();
        KeyboardInputEvent keyboard;
        keyboard.connectionChange = true;
        keyboard.connected = false;
        MouseInputEvent mouse;
        mouse.connectionChange = true;
        mouse.connected = false;
        append(keyboard);
        append(mouse);
        append(CloseRequested{});
        snapshot.focused = false;
        snapshot.closeRequested = true;
    }

    void keyboard(NSEvent* event) {
        std::lock_guard lock(mutex);
        check();
        if (!snapshot.open || !snapshot.focused || snapshot.closeRequested || event.window != window) return;
        snapshot.keyboardLed = (event.modifierFlags & NSEventModifierFlagCapsLock) ? KEYBOARD_LED_CAPS_LOCK : 0;
        if (event.type == NSEventTypeFlagsChanged) {
            const auto flags = event.modifierFlags;
            key(SDL_SCANCODE_LSHIFT, modifierPressed(flags, NX_DEVICELSHIFTKEYMASK, NX_DEVICERSHIFTKEYMASK, NX_SHIFTMASK));
            key(SDL_SCANCODE_LCTRL, modifierPressed(flags, NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK, NX_CONTROLMASK));
            key(SDL_SCANCODE_LALT, modifierPressed(flags, NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK, NX_ALTERNATEMASK));
            key(SDL_SCANCODE_LGUI, modifierPressed(flags, NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK, NX_COMMANDMASK));
            key(SDL_SCANCODE_RSHIFT, modifierPressed(flags, NX_DEVICERSHIFTKEYMASK, NX_DEVICELSHIFTKEYMASK, NX_SHIFTMASK));
            key(SDL_SCANCODE_RCTRL, modifierPressed(flags, NX_DEVICERCTLKEYMASK, NX_DEVICELCTLKEYMASK, NX_CONTROLMASK));
            key(SDL_SCANCODE_RALT, modifierPressed(flags, NX_DEVICERALTKEYMASK, NX_DEVICELALTKEYMASK, NX_ALTERNATEMASK));
            key(SDL_SCANCODE_RGUI, modifierPressed(flags, NX_DEVICERCMDKEYMASK, NX_DEVICELCMDKEYMASK, NX_COMMANDMASK));
            key(SDL_SCANCODE_CAPSLOCK, (flags & NSEventModifierFlagCapsLock) != 0);
            return;
        }
        if (event.isARepeat) return;
        auto nativeCode = event.keyCode;
        if ((nativeCode == 10 || nativeCode == 50) && KBGetLayoutType(LMGetKbdType()) == kKeyboardISO)
            nativeCode = static_cast<unsigned short>(60 - nativeCode);
        const auto code = nativeCode < std::size(darwin_scancode_table) ? darwin_scancode_table[nativeCode] : SDL_SCANCODE_UNKNOWN;
        const bool pressed = event.type == NSEventTypeKeyDown;
        if (code <= SDL_SCANCODE_UNKNOWN || code > SDL_SCANCODE_RGUI) {
            append(UnmappedKey{event.keyCode, pressed});
            return;
        }
        key(static_cast<std::uint16_t>(code), pressed);
    }

    static std::int32_t scrollDelta(double value, double& residual) {
        if ((value > 0 && residual < 0) || (value < 0 && residual > 0)) residual = 0;
        residual += value;
        const auto result = integerDelta(std::trunc(residual));
        residual -= result;
        return result;
    }

    void mouse(NSEvent* event) {
        std::lock_guard lock(mutex);
        check();
        if (!snapshot.open || !snapshot.focused || snapshot.closeRequested || event.window != window) return;
        MouseInputEvent input;
        switch (event.type) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            input.x = integerDelta(event.deltaX);
            input.y = integerDelta(event.deltaY);
            break;
        case NSEventTypeLeftMouseDown:
        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseDown:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseDown:
        case NSEventTypeOtherMouseUp:
            if (event.buttonNumber < 0 || event.buttonNumber > 2) return;
            input.button = event.buttonNumber == 0 ? 1u : event.buttonNumber == 1 ? 2u : 4u;
            input.pressed = event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeOtherMouseDown;
            if (((snapshot.heldMouseButtons & input.button) != 0) == input.pressed) return;
            break;
        case NSEventTypeScrollWheel: {
            auto x = -event.deltaX, y = event.deltaY;
            if (!event.hasPreciseScrollingDeltas) {
                x = x > 0 ? std::ceil(x) : std::floor(x);
                y = y > 0 ? std::ceil(y) : std::floor(y);
            }
            input.tilt = scrollDelta(x, wheelX);
            input.wheel = scrollDelta(y, wheelY);
            if (event.isDirectionInvertedFromDevice) {
                if (input.tilt == std::numeric_limits<std::int32_t>::min() || input.wheel == std::numeric_limits<std::int32_t>::min())
                    throw std::overflow_error("Native host wheel inversion exceeds the input representation");
                input.tilt = -input.tilt;
                input.wheel = -input.wheel;
            }
            if (input.tilt == 0 && input.wheel == 0) return;
            break;
        }
        default: return;
        }
        append(input);
        if (input.button != 0) {
            if (input.pressed) snapshot.heldMouseButtons |= input.button;
            else snapshot.heldMouseButtons &= ~input.button;
        }
    }

    void size() {
        const auto bounds = [window.contentView convertRectToBacking:window.contentView.bounds];
        const auto width = window.miniaturized ? 0.0 : bounds.size.width;
        const auto height = window.miniaturized ? 0.0 : bounds.size.height;
        if (!std::isfinite(width) || !std::isfinite(height) || width < 0 || height < 0 ||
            width > std::numeric_limits<std::uint32_t>::max() || height > std::numeric_limits<std::uint32_t>::max())
            throw std::overflow_error("Native host backing extent exceeds presentation addressing");
        layer.contentsScale = window.backingScaleFactor;
        std::lock_guard lock(mutex);
        check();
        snapshot.drawableWidth = static_cast<std::uint32_t>(width);
        snapshot.drawableHeight = static_cast<std::uint32_t>(height);
    }
};

template<class T> void capture(HostWindowState* state, T operation) noexcept {
    if (state == nullptr) return;
    try { operation(*state); }
    catch (...) { state->fail(std::current_exception()); }
}

}
}

@interface AnyPS5HostView : NSView
@property(nonatomic, assign) AnyPS5::Host::HostWindowState* hostState;
@end
@implementation AnyPS5HostView
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { AnyPS5::Host::capture(self.hostState, [&](auto& state) { state.keyboard(event); }); }
- (void)keyUp:(NSEvent*)event { AnyPS5::Host::capture(self.hostState, [&](auto& state) { state.keyboard(event); }); }
- (void)flagsChanged:(NSEvent*)event { AnyPS5::Host::capture(self.hostState, [&](auto& state) { state.keyboard(event); }); }
- (void)mouseMoved:(NSEvent*)event { AnyPS5::Host::capture(self.hostState, [&](auto& state) { state.mouse(event); }); }
- (void)mouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self mouseMoved:event]; }
- (void)mouseDown:(NSEvent*)event { [self mouseMoved:event]; }
- (void)mouseUp:(NSEvent*)event { [self mouseMoved:event]; }
- (void)rightMouseDown:(NSEvent*)event { [self mouseMoved:event]; }
- (void)rightMouseUp:(NSEvent*)event { [self mouseMoved:event]; }
- (void)otherMouseDown:(NSEvent*)event { [self mouseMoved:event]; }
- (void)otherMouseUp:(NSEvent*)event { [self mouseMoved:event]; }
- (void)scrollWheel:(NSEvent*)event { [self mouseMoved:event]; }
@end

@interface AnyPS5HostDelegate : NSObject<NSWindowDelegate, NSApplicationDelegate>
@property(nonatomic, assign) AnyPS5::Host::HostWindowState* hostState;
@property(nonatomic, strong) id previousDelegate;
@end
@implementation AnyPS5HostDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    AnyPS5::Host::capture(self.hostState, [](auto& state) { state.closeRequest(); });
    return NO;
}
- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    AnyPS5::Host::capture(self.hostState, [](auto& state) { state.focus(NSApp.active); });
}
- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    AnyPS5::Host::capture(self.hostState, [](auto& state) { state.focus(false); });
}
- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    AnyPS5::Host::capture(self.hostState, [](auto& state) { state.size(); });
}
- (void)windowDidChangeBackingProperties:(NSNotification*)notification { [self windowDidResize:notification]; }
- (void)windowDidMiniaturize:(NSNotification*)notification { [self windowDidResize:notification]; }
- (void)windowDidDeminiaturize:(NSNotification*)notification { [self windowDidResize:notification]; }
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    AnyPS5::Host::capture(self.hostState, [](auto& state) { state.closeRequest(); });
    return NSTerminateCancel;
}
- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [self.previousDelegate respondsToSelector:selector];
}
- (id)forwardingTargetForSelector:(SEL)selector {
    return [self.previousDelegate respondsToSelector:selector] ? self.previousDelegate : [super forwardingTargetForSelector:selector];
}
@end

namespace AnyPS5::Host {
struct NativeHostWindow::Impl : HostWindowState {
    ~Impl() {
        if (window != nil) {
            if (![NSThread isMainThread]) std::terminate();
            if ([window.contentView isKindOfClass:AnyPS5HostView.class])
                ((AnyPS5HostView*)window.contentView).hostState = nullptr;
            ((AnyPS5HostDelegate*)windowDelegate).hostState = nullptr;
            if (NSApp.delegate == applicationDelegate) NSApp.delegate = previousApplicationDelegate;
            if (applicationInactiveObserver) [NSNotificationCenter.defaultCenter removeObserver:applicationInactiveObserver];
            if (applicationActiveObserver) [NSNotificationCenter.defaultCenter removeObserver:applicationActiveObserver];
            window.delegate = nil;
            [window orderOut:nil];
            [window close];
        }
        if (ownsWindowSlot) nativeWindowLive = false;
    }
};

NativeHostWindow::NativeHostWindow(std::unique_ptr<Impl> value) : impl(std::move(value)) {}

NativeHostWindow::~NativeHostWindow() {
    if (impl->snapshot.open) std::terminate();
}

std::unique_ptr<NativeHostWindow> NativeHostWindow::CreateMainThread(const WindowConfiguration& configuration) {
    requireMainThread();
    if (configuration.contentWidth == 0 || configuration.contentHeight == 0 || configuration.title.empty())
        throw std::invalid_argument("Native host window requires a title and positive content extent");
    if (nativeWindowLive) throw std::logic_error("Only one native host window may be live");
    const auto etiquette = CurrentWindowEtiquette();
    // Taken before any window state exists, so a lock failure leaves nothing half-created.
    if (etiquette == WindowEtiquette::UnobtrusiveFocus) AcquireMachineFocusLock();
    auto state = std::make_unique<Impl>();
    state->ownsWindowSlot = true;
    nativeWindowLive = true;
    [NSApplication sharedApplication];
    if (NSApp.activationPolicy == NSApplicationActivationPolicyProhibited)
        [NSApp setActivationPolicy:etiquette == WindowEtiquette::Default ? NSApplicationActivationPolicyRegular
                                                                          : NSApplicationActivationPolicyAccessory];
    const auto identity = nextIdentity();
    state->snapshot.window = {identity, identity};
    state->window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, configuration.contentWidth, configuration.contentHeight)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    if (state->window == nil) throw std::runtime_error("Native host NSWindow allocation failed");
    state->window.releasedWhenClosed = NO;
    auto title = [[NSString alloc] initWithBytes:configuration.title.data() length:configuration.title.size() encoding:NSUTF8StringEncoding];
    if (title == nil) throw std::invalid_argument("Native host window title is not UTF-8");
    state->window.title = title;
    auto view = [[AnyPS5HostView alloc] initWithFrame:NSMakeRect(0, 0, configuration.contentWidth, configuration.contentHeight)];
    state->layer = [CAMetalLayer layer];
    if (view == nil || state->layer == nil) throw std::runtime_error("Native host Metal view allocation failed");
    view.hostState = state.get();
    view.wantsLayer = YES;
    view.layer = state->layer;
    state->window.contentView = view;
    state->window.acceptsMouseMovedEvents = YES;
    auto delegate = [AnyPS5HostDelegate new];
    delegate.hostState = state.get();
    delegate.previousDelegate = NSApp.delegate;
    state->previousApplicationDelegate = NSApp.delegate;
    state->windowDelegate = delegate;
    state->applicationDelegate = delegate;
    state->window.delegate = delegate;
    NSApp.delegate = delegate;
    state->snapshot.open = true;
    state->snapshot.sources.keyboard = SourceAvailability::WindowEvents;
    state->snapshot.sources.mouse = SourceAvailability::WindowEvents;
    auto* pointer = state.get();
    state->applicationInactiveObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidResignActiveNotification
        object:NSApp queue:nil usingBlock:^(NSNotification*) { capture(pointer, [](auto& value) { value.focus(false); }); }];
    state->applicationActiveObserver = [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification
        object:NSApp queue:nil usingBlock:^(NSNotification*) { capture(pointer, [](auto& value) { value.focus(value.window.keyWindow); }); }];
    [state->window makeFirstResponder:view];
    ParkUnobtrusively(state->window);
    [state->window makeKeyAndOrderFront:nil];
    if (etiquette != WindowEtiquette::Unobtrusive) [NSApp activateIgnoringOtherApps:YES];
    state->size();
    state->focus(state->window.keyWindow && NSApp.active);
    [CATransaction flush];
    return std::unique_ptr<NativeHostWindow>(new NativeHostWindow(std::move(state)));
}

void NativeHostWindow::PumpMainThread(std::chrono::milliseconds maximumWait, std::size_t maximumEvents) {
    requireMainThread();
    if (maximumWait.count() < 0 || maximumEvents == 0) throw std::invalid_argument("Native host event pump bounds are invalid");
    {
        std::lock_guard lock(impl->mutex);
        impl->check();
    }
    @autoreleasepool {
        auto deadline = [NSDate dateWithTimeIntervalSinceNow:std::chrono::duration<double>(maximumWait).count()];
        for (std::size_t count = 0; count < maximumEvents; ++count) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:count == 0 ? deadline : NSDate.distantPast
                inMode:NSDefaultRunLoopMode dequeue:YES];
            if (event == nil) break;
            [NSApp sendEvent:event];
            std::lock_guard lock(impl->mutex);
            impl->check();
        }
        [NSApp updateWindows];
    }
    std::lock_guard lock(impl->mutex);
    impl->check();
}

void NativeHostWindow::RequestCloseMainThread() {
    requireMainThread();
    try { impl->closeRequest(); }
    catch (...) { impl->fail(std::current_exception()); throw; }
}

void NativeHostWindow::CloseAfterGpuDrainMainThread() {
    requireMainThread();
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->snapshot.open) return;
        if (!impl->snapshot.closeRequested && !impl->failure)
            throw std::logic_error("Native host close requires a close request and prior CPU/GPU drain");
    }
    AgcDriverReleaseWindow_nid_postfix(impl.get());
    {
        std::lock_guard lock(impl->mutex);
        if (!impl->failure) {
            try { impl->append(WindowClosed{}); }
            catch (...) { impl->failure = std::current_exception(); }
        }
        impl->snapshot.open = false;
        impl->snapshot.focused = false;
        impl->snapshot.sources.keyboard = SourceAvailability::Unavailable;
        impl->snapshot.sources.mouse = SourceAvailability::Unavailable;
        impl->snapshot.drawableWidth = impl->snapshot.drawableHeight = 0;
    }
    ((AnyPS5HostView*)impl->window.contentView).hostState = nullptr;
    ((AnyPS5HostDelegate*)impl->windowDelegate).hostState = nullptr;
    if (NSApp.delegate == impl->applicationDelegate) NSApp.delegate = impl->previousApplicationDelegate;
    if (impl->applicationInactiveObserver) [NSNotificationCenter.defaultCenter removeObserver:impl->applicationInactiveObserver];
    if (impl->applicationActiveObserver) [NSNotificationCenter.defaultCenter removeObserver:impl->applicationActiveObserver];
    impl->applicationInactiveObserver = impl->applicationActiveObserver = nil;
    impl->window.delegate = nil;
    [impl->window orderOut:nil];
    [impl->window close];
    impl->ownsWindowSlot = false;
    nativeWindowLive = false;
}

AgcDriver::PresentationWindow NativeHostWindow::Presentation(std::uint32_t sourceWidth, std::uint32_t sourceHeight) const {
    std::lock_guard lock(impl->mutex);
    impl->check();
    if (!impl->snapshot.open || impl->snapshot.closeRequested || sourceWidth == 0 || sourceHeight == 0)
        throw std::invalid_argument("Native host presentation requires an open window and positive source extent");
    return {impl.get(), {}, nullptr, [](void* context, std::uint32_t* width, std::uint32_t* height) {
        auto& state = *static_cast<Impl*>(context);
        std::lock_guard lock(state.mutex);
        state.check();
        *width = state.snapshot.drawableWidth;
        *height = state.snapshot.drawableHeight;
    }, sourceWidth, sourceHeight, {}, [](void* context) -> void* {
        auto& state = *static_cast<Impl*>(context);
        std::lock_guard lock(state.mutex);
        state.check();
        if (!state.snapshot.open) throw std::logic_error("Native host presentation context is closed");
        return (__bridge void*)state.layer;
    }};
}

EventBatch NativeHostWindow::DrainEvents() {
    std::lock_guard lock(impl->mutex);
    impl->check();
    const auto current = std::this_thread::get_id();
    if (impl->hasConsumer && impl->consumer != current) throw std::logic_error("Native host events require one CPU consumer");
    impl->consumer = current;
    impl->hasConsumer = true;
    EventBatch result{std::move(impl->pending), impl->snapshot};
    impl->pending.clear();
    return result;
}

WindowSnapshot NativeHostWindow::Snapshot() const {
    std::lock_guard lock(impl->mutex);
    impl->check();
    return impl->snapshot;
}
}
