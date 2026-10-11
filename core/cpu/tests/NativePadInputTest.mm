#include <cpu/NativePadInput.hpp>
#include "prx/libSceVideoOut/include/UnobtrusiveWindows.hpp"
#import <AppKit/AppKit.h>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

@interface AnyPS5FractionalMouseMove : NSEvent
@property(nonatomic, strong) NSWindow* target;
@property(nonatomic) CGFloat dx, dy;
@end
@implementation AnyPS5FractionalMouseMove
- (NSEventType)type { return NSEventTypeMouseMoved; }
- (NSWindow*)window { return self.target; }
- (CGFloat)deltaX { return self.dx; }
- (CGFloat)deltaY { return self.dy; }
@end

namespace {
using namespace AnyPS5::Host;
constexpr std::uint16_t keyW = 26, keyReturn = 40, keyEscape = 41;
constexpr std::uint32_t cross = 0x4000, circle = 0x2000, options = 0x0008;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

InputEvent event(std::uint64_t sequence, EventPayload payload) {
    return {{1, 1}, sequence, std::chrono::steady_clock::now(), std::move(payload)};
}

ControllerEvent controllerEvent(ControllerEventKind kind, bool connected, std::uint32_t buttons) {
    ControllerEvent result{kind, std::chrono::steady_clock::now(), {}};
    result.after.sourceOpen = true;
    result.after.connected = connected;
    result.after.state.buttons = buttons;
    return result;
}

// Contract: one pump turns drained window and controller batches into pad samples; a connected
// controller replaces the keyboard, and dropped window events resynchronise from the snapshot.
void bridge() {
    Cpu::PadHostInput pad;
    Cpu::NativePadInput input;
    EventBatch window;
    KeyboardInputEvent press;
    press.keyCode = keyW;
    press.pressed = true;
    window.events.push_back(event(1, press));
    input.Apply(window, nullptr, pad);
    require(pad.Current().State.Sticks[1] == 0, "a drained W key did not reach the pad");

    ControllerEventBatch controller;
    controller.events.push_back(controllerEvent(ControllerEventKind::Connected, true, 0));
    controller.events.push_back(controllerEvent(ControllerEventKind::StateChanged, true, cross));
    controller.events.push_back(controllerEvent(ControllerEventKind::StateChanged, true, circle));
    controller.after = controller.events.back().after;
    const auto before = pad.Current().Sequence;
    input.Apply({}, &controller, pad);
    const auto samples = pad.Since(before, 64);
    require(before == 1 && samples.size() == 3 && samples[0].State.Sticks[1] == 128 &&
            samples[1].State.Buttons == cross && samples[2].State.Buttons == circle,
            "controller events did not commit one sample each over the keyboard");

    ControllerEventBatch gone;
    gone.events.push_back(controllerEvent(ControllerEventKind::Disconnected, false, 0));
    gone.after = gone.events.back().after;
    input.Apply({}, &gone, pad);
    require(pad.Current().State.Buttons == 0 && pad.Current().State.Sticks[1] == 0,
            "disconnecting the controller did not return to the held keyboard state");

    EventBatch overflowed;
    overflowed.after.droppedEvents = 9;
    overflowed.after.heldKeys[keyReturn] = true;
    overflowed.after.heldMouseButtons = 1;
    input.Apply(overflowed, nullptr, pad);
    require(pad.Current().State.Buttons == (cross | 0x8000) && pad.Current().State.Sticks[1] == 128,
            "dropped window events did not resynchronise held keys and buttons");

    EventBatch tap;
    KeyboardInputEvent down, up;
    down.keyCode = up.keyCode = keyEscape;
    down.pressed = true;
    tap.events = {event(3, down), event(4, up)};
    tap.after.droppedEvents = 9;
    const auto tapStart = pad.Current().Sequence;
    input.Apply(tap, nullptr, pad);
    const auto tapped = pad.Since(tapStart, 64);
    require(tapped.size() == 2 && (tapped[0].State.Buttons & options) && !(pad.Current().State.Buttons & options),
            "a key tapped inside one pump never produced a pressed sample");

    EventBatch lost;
    lost.events.push_back(event(2, FocusChanged{false}));
    lost.after.droppedEvents = 9;
    input.Apply(lost, nullptr, pad);
    require(pad.Current().State.Buttons == 0, "focus loss left keys held on the pad");
}

NSEvent* key(NSWindow* window, bool down) {
    return [NSEvent keyEventWithType:down ? NSEventTypeKeyDown : NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:window.windowNumber context:nil characters:@"w" charactersIgnoringModifiers:@"w"
        isARepeat:NO keyCode:13];
}

// Contract: an undrained window keeps the newest 4096 events and counts the rest instead of failing
// the run (audit XT-05/MEM-19); sub-pixel pointer motion accumulates (MEM-28).
void window() {
    RequireRealFocusForThisProcess();
    auto host = NativeHostWindow::CreateMainThread({"AnyPS5 pad input window", 160, 100});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!host->Snapshot().focused && std::chrono::steady_clock::now() < deadline)
        host->PumpMainThread(std::chrono::milliseconds(10));
    require(host->Snapshot().focused, "the pad input window never became focused");
    static_cast<void>(host->DrainEvents());
    NSWindow* native = nil;
    for (NSWindow* candidate in NSApp.windows)
        if ([candidate.title isEqualToString:@"AnyPS5 pad input window"]) native = candidate;
    require(native != nil, "the pad input window is missing");
    constexpr std::size_t total = 70001;
    for (std::size_t index = 0; index < total; ++index) [native.contentView keyDown:key(native, index % 2 == 0)];
    auto batch = host->DrainEvents();
    require(batch.events.size() == 4096 && batch.after.droppedEvents == total - 4096,
            "the window queue is not bounded to the newest 4096 events");
    require(batch.events.front().sequence == batch.after.lastSequence - 4095 &&
            batch.events.back().sequence == batch.after.lastSequence, "the window queue did not drop the oldest events");
    Cpu::PadHostInput pad;
    Cpu::NativePadInput input;
    input.Apply(batch, nullptr, pad);
    require(pad.Current().State.Sticks[1] == 0, "the held W key was lost across the dropped events");

    for (int index = 0; index < 5; ++index) {
        AnyPS5FractionalMouseMove* move = [AnyPS5FractionalMouseMove new];
        move.target = native;
        move.dx = 0.4;
        move.dy = -0.3;
        [native.contentView mouseMoved:move];
    }
    batch = host->DrainEvents();
    std::int32_t x = 0, y = 0;
    for (const auto& drained : batch.events)
        if (const auto* mouse = std::get_if<MouseInputEvent>(&drained.payload)) { x += mouse->x; y += mouse->y; }
    require(x == 2 && y == -1, "sub-pixel pointer deltas were truncated per event");

    host->RequestCloseMainThread();
    host->CloseAfterGpuDrainMainThread();
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            require(argc == 2, "Usage: NativePadInputTest bridge|window");
            const std::string mode = argv[1];
            if (mode == "bridge") bridge();
            else if (mode == "window") window();
            else throw std::invalid_argument("Unknown native pad input case");
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
