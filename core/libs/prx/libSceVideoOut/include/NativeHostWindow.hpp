#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "prx/libSceKeyboard/include/KeyboardState.hpp"
#include "prx/libSceMouse/include/MouseState.hpp"

namespace AgcDriver { struct PresentationWindow; }
namespace AnyPS5::Host {

struct WindowIdentity {
    std::uint64_t id{};
    std::uint64_t generation{};
    friend bool operator==(WindowIdentity, WindowIdentity) = default;
};
enum class SourceAvailability : std::uint8_t {
    Unavailable, WindowEvents, Unsupported
};
struct InputSources {
    SourceAvailability keyboard{SourceAvailability::Unavailable};
    SourceAvailability mouse{SourceAvailability::Unavailable};
    SourceAvailability controller{SourceAvailability::Unsupported};
};
struct FocusChanged { bool focused{}; };
struct CloseRequested {};
struct WindowClosed {};
struct UnmappedKey { std::uint16_t appKitVirtualCode{}; bool pressed{}; };
using EventPayload = std::variant<KeyboardInputEvent, MouseInputEvent,
    FocusChanged, CloseRequested, WindowClosed, UnmappedKey>;
struct InputEvent {
    WindowIdentity window;
    std::uint64_t sequence{};
    std::chrono::steady_clock::time_point capturedAt;
    EventPayload payload;
};
struct WindowSnapshot {
    WindowIdentity window;
    InputSources sources;
    std::uint64_t lastSequence{};
    bool open{}, focused{}, closeRequested{};
    std::uint32_t drawableWidth{}, drawableHeight{};
    std::array<bool, 256> heldKeys{};
    std::uint32_t heldMouseButtons{};
    std::uint32_t keyboardLed{};
};
struct EventBatch {
    std::vector<InputEvent> events;
    WindowSnapshot after;
};
struct WindowConfiguration {
    std::string title;
    std::uint32_t contentWidth{}, contentHeight{};
};

class NativeHostWindow {
public:
    static std::unique_ptr<NativeHostWindow> CreateMainThread(const WindowConfiguration&);
    ~NativeHostWindow();
    NativeHostWindow(const NativeHostWindow&) = delete;
    NativeHostWindow& operator=(const NativeHostWindow&) = delete;

    void PumpMainThread(std::chrono::milliseconds maximumWait,
                        std::size_t maximumEvents = 256);
    void RequestCloseMainThread();
    void CloseAfterGpuDrainMainThread();

    AgcDriver::PresentationWindow Presentation(std::uint32_t sourceWidth,
                                               std::uint32_t sourceHeight) const;
    EventBatch DrainEvents();
    WindowSnapshot Snapshot() const;
private:
    struct Impl;
    explicit NativeHostWindow(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};
}
