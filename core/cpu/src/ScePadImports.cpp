#include <cpu/ScePadImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace Cpu {
namespace {

// Status codes and record layouts follow the upstream HLE export (core/libs/prx/libScePad)
// and SceTypes.hpp: ScePadData is 0x78 bytes, ScePadControllerInformation 0x1c.
constexpr std::uint32_t PadInvalidArg = 0x80920001;
constexpr std::uint32_t PadInvalidHandle = 0x80920003;
constexpr std::uint32_t PadNotInitialized = 0x80920005;
constexpr std::uint32_t PadDeviceNoHandle = 0x80920008;
constexpr std::size_t PadDataSize = 0x78;
constexpr std::size_t ControllerInformationSize = 0x1c;
constexpr std::size_t ExtControllerInformationSize = 0x2c;
constexpr std::size_t PadInfoSize = 0x59;
constexpr std::size_t DeviceClassDataSize = 0x18;
constexpr std::size_t DeviceClassExtendedInformationSize = 0x14;
constexpr std::size_t TriggerEffectParamSize = 0x78;
constexpr std::int32_t MaximumReadCount = 64;

constexpr std::uint32_t ButtonL3 = 0x0002, ButtonR3 = 0x0004, ButtonOptions = 0x0008;
constexpr std::uint32_t ButtonUp = 0x0010, ButtonRight = 0x0020, ButtonDown = 0x0040, ButtonLeft = 0x0080;
constexpr std::uint32_t ButtonL2 = 0x0100, ButtonR2 = 0x0200, ButtonL1 = 0x0400, ButtonR1 = 0x0800;
constexpr std::uint32_t ButtonTriangle = 0x1000, ButtonCircle = 0x2000, ButtonCross = 0x4000, ButtonSquare = 0x8000;
constexpr std::uint32_t ButtonTouchPad = 0x100000;
constexpr std::uint32_t MouseLeft = 1, MouseRight = 2, MouseMiddle = 4;

enum class Control : std::uint8_t {
    None, Button, LeftStickLeft, LeftStickRight, LeftStickUp, LeftStickDown,
    RightStickLeft, RightStickRight, RightStickUp, RightStickDown, TouchLeft, TouchRight
};
struct Binding { std::uint16_t Usage; Control Kind; std::uint32_t Button = 0; };

// Upstream's default keyboard mapping (libScePad InputMapping.hpp) by USB HID usage, which is the
// code the native host window reports.
constexpr std::array KeyBindings{
    Binding{40, Control::Button, ButtonCross}, Binding{44, Control::Button, ButtonCross},
    Binding{41, Control::Button, ButtonOptions}, Binding{12, Control::Button, ButtonTriangle},
    Binding{6, Control::Button, ButtonCircle}, Binding{20, Control::Button, ButtonL1},
    Binding{8, Control::Button, ButtonR1}, Binding{226, Control::Button, ButtonR1},
    Binding{230, Control::Button, ButtonR1}, Binding{225, Control::Button, ButtonL3},
    Binding{229, Control::Button, ButtonL3}, Binding{224, Control::Button, ButtonR3},
    Binding{228, Control::Button, ButtonR3}, Binding{82, Control::Button, ButtonUp},
    Binding{79, Control::Button, ButtonRight}, Binding{81, Control::Button, ButtonDown},
    Binding{80, Control::Button, ButtonLeft},
    Binding{4, Control::LeftStickLeft}, Binding{7, Control::LeftStickRight},
    Binding{26, Control::LeftStickUp}, Binding{22, Control::LeftStickDown},
    Binding{9, Control::RightStickLeft}, Binding{11, Control::RightStickRight},
    Binding{23, Control::RightStickUp}, Binding{10, Control::RightStickDown},
    Binding{42, Control::TouchLeft}, Binding{43, Control::TouchRight}};

std::uint64_t nowMicroseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint8_t axis(bool negative, bool positive) {
    if (negative == positive) return 128;
    return negative ? 0 : 255;
}

// Upstream PadInput: the deflection grows with distance between 64 and 128 so small motions
// clear the stick dead zone.
std::array<std::uint8_t, 2> mouseDeflection(std::int64_t dx, std::int64_t dy) {
    const double distance = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
    const double scale = std::clamp(distance + 16.0, 64.0, 128.0) / distance;
    const auto map = [scale](std::int64_t delta) {
        return static_cast<std::uint8_t>(std::clamp<long>(128L + std::lround(static_cast<double>(delta) * scale), 0L, 255L));
    };
    return {map(dx), map(dy)};
}

std::uint64_t argument(Machine& guest, unsigned index) {
    constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
    return guest.Get(registers.at(index));
}

std::int32_t integer(Machine& guest, unsigned index) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(argument(guest, index)));
}

bool accessible(Machine& guest, std::uint64_t address, std::size_t size, Permission permission) {
    if (!address || size > std::numeric_limits<std::uint64_t>::max() - address) return false;
    try { guest.CheckAccess(address, size, permission); }
    catch (const std::runtime_error&) { return false; }
    return true;
}

template<class Value> void put(std::vector<std::byte>& bytes, std::size_t offset, Value value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(Value));
}

}

PadHostInput::PadHostInput() { current.TimestampUs = nowMicroseconds(); }

void PadHostInput::SetController(const PadControllerSample& state) {
    std::lock_guard lock(mutex);
    controller = state;
}

void PadHostInput::DisconnectController() {
    std::lock_guard lock(mutex);
    controller.reset();
}

void PadHostInput::Key(std::uint16_t usage, bool pressed) {
    std::lock_guard lock(mutex);
    if (usage < keys.size()) keys[usage] = pressed;
}

void PadHostInput::MouseButton(std::uint32_t button, bool pressed) {
    std::lock_guard lock(mutex);
    if (button == MouseMiddle && pressed && !(mouseButtons & MouseMiddle)) {
        mouseLook = !mouseLook;
        motionX = motionY = 0;
    }
    if (pressed) mouseButtons |= button;
    else mouseButtons &= ~button;
}

void PadHostInput::MouseMotion(std::int32_t dx, std::int32_t dy) {
    std::lock_guard lock(mutex);
    if (!mouseLook) return;
    constexpr std::int64_t limit = std::int64_t{1} << 40;
    motionX = std::clamp<std::int64_t>(motionX + dx, -limit, limit);
    motionY = std::clamp<std::int64_t>(motionY + dy, -limit, limit);
}

void PadHostInput::MouseWheel(std::int32_t wheel) {
    std::lock_guard lock(mutex);
    if (!wheel) return;
    wheelButton = wheel > 0 ? ButtonUp : ButtonDown;
    wheelUntil = std::chrono::steady_clock::now() + WheelHold;
}

void PadHostInput::SyncHeld(const std::array<bool, 256>& held, std::uint32_t buttons) {
    std::lock_guard lock(mutex);
    keys = held;
    mouseButtons = buttons;
}

void PadHostInput::ResetKeyboardMouse() {
    std::lock_guard lock(mutex);
    keys.fill(false);
    mouseButtons = 0;
    mouseLook = false;
    motionX = motionY = 0;
    mouseStick = {128, 128};
    mouseStickUntil = wheelUntil = {};
    wheelButton = 0;
}

PadControllerSample PadHostInput::effectiveLocked(std::chrono::steady_clock::time_point now) {
    if (motionX || motionY) {
        mouseStick = mouseDeflection(motionX, motionY);
        mouseStickUntil = now + MouseHold;
    } else if (now >= mouseStickUntil) mouseStick = {128, 128};
    motionX = motionY = 0;
    if (now >= wheelUntil) wheelButton = 0;
    if (controller) return *controller;
    PadControllerSample state;
    bool held[12]{};
    for (const auto& binding : KeyBindings) {
        if (!keys[binding.Usage]) continue;
        if (binding.Kind == Control::Button) state.Buttons |= binding.Button;
        else held[static_cast<std::size_t>(binding.Kind)] = true;
    }
    const auto is = [&](Control control) { return held[static_cast<std::size_t>(control)]; };
    if (mouseButtons & MouseLeft) state.Buttons |= ButtonSquare;
    if (mouseButtons & MouseRight) state.Buttons |= ButtonR2;
    state.Buttons |= wheelButton;
    if (state.Buttons & ButtonL2) state.L2 = 255;
    if (state.Buttons & ButtonR2) state.R2 = 255;
    state.Sticks[0] = axis(is(Control::LeftStickLeft), is(Control::LeftStickRight));
    state.Sticks[1] = axis(is(Control::LeftStickUp), is(Control::LeftStickDown));
    const bool rightKeys = is(Control::RightStickLeft) || is(Control::RightStickRight) ||
                           is(Control::RightStickUp) || is(Control::RightStickDown);
    state.Sticks[2] = rightKeys ? axis(is(Control::RightStickLeft), is(Control::RightStickRight)) : mouseStick[0];
    state.Sticks[3] = rightKeys ? axis(is(Control::RightStickUp), is(Control::RightStickDown)) : mouseStick[1];
    state.TouchLeft = is(Control::TouchLeft);
    state.TouchRight = is(Control::TouchRight);
    return state;
}

void PadHostInput::Commit() {
    std::lock_guard lock(mutex);
    const auto state = effectiveLocked(std::chrono::steady_clock::now());
    if (state == current.State) return;
    auto timestamp = nowMicroseconds();
    if (timestamp <= current.TimestampUs) timestamp = current.TimestampUs + 1;
    current = {state, timestamp, current.Sequence + 1};
    if (count == ring.size()) {
        head = (head + 1) % ring.size();
        --count;
        ++dropped;
    }
    ring[(head + count) % ring.size()] = current;
    ++count;
}

PadSample PadHostInput::Current() const {
    std::lock_guard lock(mutex);
    return current;
}

std::vector<PadSample> PadHostInput::Since(std::uint64_t sequence, std::size_t maximum) const {
    std::lock_guard lock(mutex);
    std::vector<PadSample> result;
    for (std::size_t index = 0; index < count && result.size() < maximum; ++index) {
        const auto& sample = ring[(head + index) % ring.size()];
        if (sample.Sequence > sequence) result.push_back(sample);
    }
    return result;
}

std::uint64_t PadHostInput::DroppedSamples() const {
    std::lock_guard lock(mutex);
    return dropped;
}

PadHostOutput PadHostInput::Output() const {
    std::lock_guard lock(mutex);
    return output;
}

void PadHostInput::SetLightBar(std::optional<std::array<std::uint8_t, 3>> color) {
    std::lock_guard lock(mutex);
    if (output.LightBarValid == color.has_value() && (!color || output.LightBar == *color)) return;
    output.LightBarValid = color.has_value();
    output.LightBar = color.value_or(std::array<std::uint8_t, 3>{});
    ++output.Sequence;
}

void PadHostInput::SetVibration(std::uint8_t large, std::uint8_t small) {
    std::lock_guard lock(mutex);
    if (output.LargeMotor == large && output.SmallMotor == small) return;
    output.LargeMotor = large;
    output.SmallMotor = small;
    ++output.Sequence;
}

void PadHostInput::SetMotionEnabled(bool enabled) {
    std::lock_guard lock(mutex);
    if (output.MotionEnabled == enabled) return;
    output.MotionEnabled = enabled;
    ++output.Sequence;
}

struct ScePadImports::Impl {
    using Handler = std::function<std::uint32_t(Impl&, Machine&)>;
    using Port = std::tuple<std::int32_t, std::int32_t, std::int32_t>;
    std::shared_ptr<SceHostTrampolines> trampolines;
    std::shared_ptr<PadHostInput> input = std::make_shared<PadHostInput>();
    std::map<std::string, Handler> functions;
    std::map<std::string, std::uint64_t> gates;
    std::map<Port, std::int32_t> ports;
    struct Handle { Port Port; std::uint64_t Cursor; };
    std::map<std::int32_t, Handle> handles;
    std::int32_t nextHandle = 1;
    bool initialized = false;

    static bool validPort(std::int32_t user, std::int32_t type, std::int32_t index) {
        const bool personal = type == 0 || type == 2;
        const bool systemRemote = user == 0xff && type == 16;
        return index == 0 && (personal || systemRemote);
    }

    bool open(Machine& guest) const { return handles.contains(integer(guest, 0)); }

    std::uint32_t openPort(Machine& guest) {
        if (!initialized) return PadNotInitialized;
        const Port port{integer(guest, 0), integer(guest, 1), integer(guest, 2)};
        if (!validPort(std::get<0>(port), std::get<1>(port), std::get<2>(port))) return PadInvalidArg;
        if (const auto found = ports.find(port); found != ports.end()) return static_cast<std::uint32_t>(found->second);
        if (nextHandle == std::numeric_limits<std::int32_t>::max()) return PadInvalidArg;
        const auto handle = nextHandle++;
        ports.emplace(port, handle);
        handles.emplace(handle, Handle{port, input->Current().Sequence});
        return static_cast<std::uint32_t>(handle);
    }

    static std::vector<std::byte> padData(const PadSample& sample) {
        std::vector<std::byte> bytes(PadDataSize);
        const auto& state = sample.State;
        auto buttons = state.Buttons;
        const bool touch = state.TouchLeft || state.TouchRight;
        if (touch) buttons |= ButtonTouchPad;
        put(bytes, 0x00, buttons);
        for (std::size_t index = 0; index < state.Sticks.size(); ++index) put(bytes, 0x04 + index, state.Sticks[index]);
        put(bytes, 0x08, state.L2);
        put(bytes, 0x09, state.R2);
        put(bytes, 0x18, 1.0f);
        put(bytes, 0x20, 1.0f);
        if (touch) {
            put(bytes, 0x34, std::uint8_t{1});
            put(bytes, 0x3c, std::uint16_t{state.TouchRight ? std::uint16_t{1440} : std::uint16_t{480}});
            put(bytes, 0x3e, std::uint16_t{471});
        }
        put(bytes, 0x4c, std::uint8_t{1});
        put(bytes, 0x50, sample.TimestampUs);
        put(bytes, 0x68, std::uint8_t{1});
        return bytes;
    }

    static std::vector<std::byte> controllerInformation(std::size_t size) {
        std::vector<std::byte> bytes(size);
        put(bytes, 0x00, 44.86f);
        put(bytes, 0x04, std::uint16_t{1920});
        put(bytes, 0x06, std::uint16_t{943});
        put(bytes, 0x08, std::uint8_t{2});
        put(bytes, 0x09, std::uint8_t{2});
        put(bytes, 0x0b, std::uint8_t{1});
        put(bytes, 0x0c, std::uint8_t{1});
        return bytes;
    }

    template<class Make> static std::uint32_t write(Machine& guest, std::uint64_t address, std::size_t size, Make make) {
        if (!accessible(guest, address, size, Permission::Write)) return PadInvalidArg;
        guest.Write(address, make());
        return 0;
    }

    Impl(Machine& machine, std::uint64_t base) : trampolines(std::make_shared<SceHostTrampolines>(machine, base, 64)) {
        const auto ok = [](Impl&, Machine&) -> std::uint32_t { return 0; };
        const auto handleOnly = [](Impl& self, Machine& guest) -> std::uint32_t {
            return self.open(guest) ? 0 : PadInvalidHandle;
        };
        const auto withHandle = [](auto body) {
            return [body](Impl& self, Machine& guest) -> std::uint32_t {
                return self.open(guest) ? body(self, guest) : PadInvalidHandle;
            };
        };
        const auto openPort = [](Impl& self, Machine& guest) { return self.openPort(guest); };
        const std::initializer_list<std::pair<std::string_view, Handler>> table{
            {"scePadInit", [](Impl& self, Machine&) -> std::uint32_t { self.initialized = true; return 0; }},
            {"scePadOpen", openPort},
            {"scePadOpenExt", openPort},
            {"scePadGetHandle", [](Impl& self, Machine& guest) -> std::uint32_t {
                const auto found = self.ports.find({integer(guest, 0), integer(guest, 1), integer(guest, 2)});
                return found == self.ports.end() ? PadDeviceNoHandle : static_cast<std::uint32_t>(found->second); }},
            {"scePadClose", [](Impl& self, Machine& guest) -> std::uint32_t {
                const auto found = self.handles.find(integer(guest, 0));
                if (found == self.handles.end()) return PadInvalidHandle;
                self.ports.erase(found->second.Port);
                self.handles.erase(found);
                return 0; }},
            {"scePadReadState", withHandle([](Impl& self, Machine& guest) {
                return write(guest, argument(guest, 1), PadDataSize, [&] { return padData(self.input->Current()); }); })},
            {"scePadRead", withHandle([](Impl& self, Machine& guest) -> std::uint32_t {
                const auto address = argument(guest, 1);
                const auto count = integer(guest, 2);
                if (count < 1 || count > MaximumReadCount ||
                    !accessible(guest, address, PadDataSize * static_cast<std::size_t>(count), Permission::Write))
                    return PadInvalidArg;
                auto& cursor = self.handles.at(integer(guest, 0)).Cursor;
                auto samples = self.input->Since(cursor, static_cast<std::size_t>(count));
                if (samples.empty()) samples.push_back(self.input->Current());
                cursor = samples.back().Sequence;
                for (std::size_t index = 0; index < samples.size(); ++index)
                    guest.Write(address + index * PadDataSize, padData(samples[index]));
                return static_cast<std::uint32_t>(samples.size()); })},
            {"scePadGetControllerInformation", withHandle([](Impl&, Machine& guest) {
                return write(guest, argument(guest, 1), ControllerInformationSize,
                             [] { return controllerInformation(ControllerInformationSize); }); })},
            {"scePadGetExtControllerInformation", withHandle([](Impl&, Machine& guest) {
                return write(guest, argument(guest, 1), ExtControllerInformationSize,
                             [] { return controllerInformation(ExtControllerInformationSize); }); })},
            {"scePadGetInfo", [](Impl&, Machine& guest) {
                return write(guest, argument(guest, 0), PadInfoSize, [] {
                    std::vector<std::byte> bytes(PadInfoSize);
                    put(bytes, 0x00, std::uint8_t{4});
                    put(bytes, 0x01, std::uint8_t{1});
                    put(bytes, 0x06, std::uint8_t{1});
                    put(bytes, 0x07, std::uint8_t{3});
                    put(bytes, 0x08, std::uint8_t{2});
                    return bytes; }); }},
            {"scePadSetLightBar", withHandle([](Impl& self, Machine& guest) -> std::uint32_t {
                const auto address = argument(guest, 1);
                if (!accessible(guest, address, 3, Permission::Read)) return PadInvalidArg;
                std::array<std::uint8_t, 3> color{};
                guest.Read(address, std::as_writable_bytes(std::span(color)));
                self.input->SetLightBar(color);
                return 0; })},
            {"scePadResetLightBar", withHandle([](Impl& self, Machine&) -> std::uint32_t {
                self.input->SetLightBar(std::nullopt);
                return 0; })},
            {"scePadSetVibration", withHandle([](Impl& self, Machine& guest) -> std::uint32_t {
                const auto address = argument(guest, 1);
                if (!accessible(guest, address, 2, Permission::Read)) return PadInvalidArg;
                std::array<std::uint8_t, 2> motors{};
                guest.Read(address, std::as_writable_bytes(std::span(motors)));
                self.input->SetVibration(motors[0], motors[1]);
                return 0; })},
            {"scePadSetVibrationMode", withHandle([](Impl&, Machine& guest) -> std::uint32_t {
                const auto mode = integer(guest, 1);
                return mode == 0 || mode == 1 ? 0 : PadInvalidArg; })},
            {"scePadSetMotionSensorState", withHandle([](Impl& self, Machine& guest) -> std::uint32_t {
                self.input->SetMotionEnabled((argument(guest, 1) & 0xff) != 0);
                return 0; })},
            {"scePadSetTiltCorrectionState", handleOnly},
            {"scePadSetAngularVelocityDeadbandState", handleOnly},
            {"scePadResetOrientation", handleOnly},
            {"scePadSetTriggerEffect", withHandle([](Impl&, Machine& guest) -> std::uint32_t {
                const auto address = argument(guest, 1);
                if (!accessible(guest, address, TriggerEffectParamSize, Permission::Read)) return PadInvalidArg;
                std::array<std::uint8_t, TriggerEffectParamSize> bytes{};
                guest.Read(address, std::as_writable_bytes(std::span(bytes)));
                std::uint32_t left = 0, right = 0;
                std::memcpy(&left, bytes.data() + 8, 4);
                std::memcpy(&right, bytes.data() + 64, 4);
                return (bytes[0] & ~0x3u) || left > 6 || right > 6 ? PadInvalidArg : 0; })},
            {"scePadGetTriggerEffectState", withHandle([](Impl&, Machine& guest) {
                return write(guest, argument(guest, 1), 8, [] { return std::vector<std::byte>(8); }); })},
            {"scePadIsRemoteController", withHandle([](Impl&, Machine& guest) {
                return write(guest, argument(guest, 1), 1, [] { return std::vector<std::byte>(1); }); })},
            {"scePadDeviceClassGetExtendedInformation", withHandle([](Impl&, Machine& guest) {
                return write(guest, argument(guest, 1), DeviceClassExtendedInformationSize,
                             [] { return std::vector<std::byte>(DeviceClassExtendedInformationSize); }); })},
            {"scePadDeviceClassParseData", withHandle([](Impl&, Machine& guest) -> std::uint32_t {
                const auto source = argument(guest, 1);
                if (!accessible(guest, source, PadDataSize, Permission::Read)) return PadInvalidArg;
                std::array<std::uint8_t, 1> connected{};
                guest.Read(source + 0x4c, std::as_writable_bytes(std::span(connected)));
                return write(guest, argument(guest, 2), DeviceClassDataSize, [&] {
                    std::vector<std::byte> bytes(DeviceClassDataSize);
                    put(bytes, 0x04, std::uint8_t{connected[0] ? std::uint8_t{1} : std::uint8_t{0}});
                    return bytes; }); })},
            {"scePadSetProcessPrivilege", ok},
            {"scePadSetParticularMode", ok},
            {"scePadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse", ok},
        };
        for (const auto& [name, handler] : table)
            if (!functions.emplace(Nid::ComputeNid(std::string(name), ""), handler).second)
                throw std::logic_error("Duplicate libScePad NID for " + std::string(name));
    }
};

ScePadImports::ScePadImports(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
ScePadImports::~ScePadImports() = default;

std::shared_ptr<PadHostInput> ScePadImports::Input() const { return impl->input; }

std::optional<std::uint64_t> ScePadImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libScePad" || import.ModuleName != "libScePad" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        return std::nullopt;
    const auto found = impl->functions.find(import.Nid);
    if (found == impl->functions.end()) return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    const auto gate = impl->trampolines->Add([state = std::weak_ptr<Impl>(impl), run = found->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE libScePad import runtime has expired");
        guest.Set(Register::Rax, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(run(*context, guest)))));
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
