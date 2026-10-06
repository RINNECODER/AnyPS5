#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace AnyPS5::Host {

struct ControllerIdentity {
    std::uint64_t id{};
    std::uint64_t generation{};
    friend bool operator==(ControllerIdentity, ControllerIdentity) = default;
};
enum class ControllerKind : std::uint8_t { Unknown, DualSense, DualShock, ExtendedGamepad };
enum class ControllerButton : std::uint32_t {
    L3 = 0x0002, R3 = 0x0004, Options = 0x0008,
    Up = 0x0010, Right = 0x0020, Down = 0x0040, Left = 0x0080,
    L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800,
    Triangle = 0x1000, Circle = 0x2000, Cross = 0x4000, Square = 0x8000,
    TouchPad = 0x100000
};
struct ControllerState {
    std::uint32_t buttons{};
    std::array<std::uint8_t, 4> sticks{128, 128, 128, 128};
    std::uint8_t leftTrigger{}, rightTrigger{};
    friend bool operator==(const ControllerState&, const ControllerState&) = default;
};
struct ControllerCapabilities {
    bool leftStickButton{}, rightStickButton{}, optionsButton{}, touchpadButton{};
    bool motion{}, touchCoordinates{}, haptics{};
};
struct ControllerSnapshot {
    ControllerIdentity controller;
    ControllerKind kind{ControllerKind::Unknown};
    ControllerCapabilities capabilities;
    ControllerState state;
    std::string vendorName;
    std::uint64_t lastSequence{};
    std::uint32_t unsupportedControllerCount{};
    bool sourceOpen{}, connected{};
};
enum class ControllerEventKind : std::uint8_t { Connected, StateChanged, Disconnected, SourceClosed, AvailabilityChanged };
struct ControllerEvent {
    ControllerEventKind kind;
    std::chrono::steady_clock::time_point capturedAt;
    ControllerSnapshot after;
};
struct ControllerEventBatch {
    std::vector<ControllerEvent> events;
    ControllerSnapshot after;
};
class NativeControllerSource {
public:
    static std::unique_ptr<NativeControllerSource> CreateMainThread();
    ~NativeControllerSource();
    NativeControllerSource(const NativeControllerSource&) = delete;
    NativeControllerSource& operator=(const NativeControllerSource&) = delete;
    void CloseMainThread();
    ControllerEventBatch DrainEvents();
    ControllerSnapshot Snapshot() const;
private:
    struct Impl;
    explicit NativeControllerSource(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl;
};
}
