#include <cpu/NativePadInput.hpp>
#include <type_traits>
#include <variant>

namespace Cpu {

PadControllerSample PadSampleFromController(const AnyPS5::Host::ControllerState& state) {
    PadControllerSample sample;
    sample.Buttons = state.buttons;
    sample.Sticks = state.sticks;
    sample.L2 = state.leftTrigger;
    sample.R2 = state.rightTrigger;
    return sample;
}

void NativePadInput::Apply(const AnyPS5::Host::EventBatch& window, const AnyPS5::Host::ControllerEventBatch* controller,
                           PadHostInput& pad) {
    const auto controllerState = [&](const AnyPS5::Host::ControllerSnapshot& snapshot) {
        if (snapshot.sourceOpen && snapshot.connected) pad.SetController(PadSampleFromController(snapshot.state));
        else pad.DisconnectController();
    };
    if (controller) {
        for (const auto& event : controller->events) {
            controllerState(event.after);
            pad.Commit();
        }
        controllerState(controller->after);
    }
    for (const auto& event : window.events) {
        std::visit([&](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, KeyboardInputEvent>) {
                if (payload.resetKeys || (payload.connectionChange && !payload.connected)) pad.ResetKeyboardMouse();
                else if (!payload.connectionChange) {
                    pad.Key(payload.keyCode, payload.pressed);
                    pad.Commit();
                }
            } else if constexpr (std::is_same_v<Payload, MouseInputEvent>) {
                if (payload.resetButtons || (payload.connectionChange && !payload.connected)) pad.ResetKeyboardMouse();
                else if (!payload.connectionChange) {
                    if (payload.x || payload.y) pad.MouseMotion(payload.x, payload.y);
                    if (payload.wheel) pad.MouseWheel(payload.wheel);
                    if (payload.button || payload.wheel) {
                        if (payload.button) pad.MouseButton(payload.button, payload.pressed);
                        pad.Commit();
                    }
                }
            } else if constexpr (std::is_same_v<Payload, AnyPS5::Host::FocusChanged>) {
                if (!payload.focused) pad.ResetKeyboardMouse();
            }
        }, event.payload);
    }
    if (window.after.droppedEvents != windowDropped) {
        windowDropped = window.after.droppedEvents;
        pad.SyncHeld(window.after.heldKeys, window.after.heldMouseButtons);
    }
    pad.Commit();
}

}
