#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Cpu {

struct SceImport;

struct PadControllerSample {
    std::uint32_t Buttons = 0;
    std::array<std::uint8_t, 4> Sticks{128, 128, 128, 128};
    std::uint8_t L2 = 0, R2 = 0;
    bool TouchLeft = false, TouchRight = false;
    friend bool operator==(const PadControllerSample&, const PadControllerSample&) = default;
};

struct PadSample {
    PadControllerSample State;
    std::uint64_t TimestampUs = 0;
    std::uint64_t Sequence = 0;
};

struct PadHostOutput {
    std::uint64_t Sequence = 0;
    bool LightBarValid = false;
    std::array<std::uint8_t, 3> LightBar{};
    std::uint8_t LargeMotor = 0, SmallMotor = 0;
    bool MotionEnabled = true;
};

// Host side of the virtual pad. The host pump feeds controller, keyboard (USB HID usage codes)
// and mouse input, then calls Commit. A connected controller replaces keyboard and mouse input.
// Mouse look holds its deflection for MouseHold and the wheel presses Up/Down for WheelHold, as
// upstream PadInput does. Committed state changes get increasing sequence numbers and queue in a
// ring of SampleCapacity that drops the oldest; readers keep their own cursor. Thread-safe.
class PadHostInput {
public:
    static constexpr std::size_t SampleCapacity = 64;
    static constexpr std::chrono::milliseconds MouseHold{33}, WheelHold{80};
    PadHostInput();
    void SetController(const PadControllerSample& state);
    void DisconnectController();
    void Key(std::uint16_t usage, bool pressed);
    void MouseButton(std::uint32_t button, bool pressed);
    void MouseMotion(std::int32_t dx, std::int32_t dy);
    void MouseWheel(std::int32_t wheel);
    // Replaces held keys and mouse buttons without toggling mouse look (queue overflow recovery).
    void SyncHeld(const std::array<bool, 256>& keys, std::uint32_t mouseButtons);
    // Focus loss: releases everything and leaves mouse look.
    void ResetKeyboardMouse();
    void Commit();
    PadSample Current() const;
    std::vector<PadSample> Since(std::uint64_t sequence, std::size_t maximum) const;
    std::uint64_t DroppedSamples() const;
    PadHostOutput Output() const;
    void SetLightBar(std::optional<std::array<std::uint8_t, 3>> color);
    void SetVibration(std::uint8_t large, std::uint8_t small);
    void SetMotionEnabled(bool enabled);
private:
    PadControllerSample effectiveLocked(std::chrono::steady_clock::time_point now);
    mutable std::mutex mutex;
    std::optional<PadControllerSample> controller;
    std::array<bool, 256> keys{};
    std::uint32_t mouseButtons = 0;
    bool mouseLook = false;
    std::int64_t motionX = 0, motionY = 0;
    std::array<std::uint8_t, 2> mouseStick{128, 128};
    std::chrono::steady_clock::time_point mouseStickUntil{}, wheelUntil{};
    std::uint32_t wheelButton = 0;
    PadSample current;
    std::array<PadSample, SampleCapacity> ring{};
    std::size_t head = 0, count = 0;
    std::uint64_t dropped = 0;
    PadHostOutput output;
};

// libScePad on the virtual pad: Init, Open/OpenExt, GetHandle, Close, Read, ReadState, controller
// information, light bar, vibration, motion and trigger-effect requests. Results are signed SCE
// status codes. Unknown NIDs return nullopt. Machine must outlive this provider.
class ScePadImports {
public:
    explicit ScePadImports(Machine& machine, std::uint64_t gateBase = 0x7ffd98000000);
    ~ScePadImports();
    ScePadImports(const ScePadImports&) = delete;
    ScePadImports& operator=(const ScePadImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
    std::shared_ptr<PadHostInput> Input() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
