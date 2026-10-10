#include <cpu/ScePadImports.hpp>
#include <cpu/SceElf.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t user = 0x10000000;
constexpr std::uint64_t data = 0x3000;
constexpr std::uint64_t unmapped = 0x9000;
constexpr std::uint32_t invalidArg = 0x80920001;
constexpr std::uint32_t invalidHandle = 0x80920003;
constexpr std::uint32_t notInitialized = 0x80920005;
constexpr std::uint32_t noHandle = 0x80920008;
constexpr std::uint32_t cross = 0x4000, square = 0x8000, l1 = 0x0400, r2 = 0x0200, up = 0x0010, options = 0x0008;
constexpr std::uint16_t keyW = 26, keyD = 7, keyReturn = 40, keyEscape = 41, keyUp = 82;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Cpu::SceImport import(const char* name, const char* library = "libScePad") {
    Cpu::SceImport value;
    value.Nid = Nid::ComputeNid(name, "");
    value.LibraryName = library;
    value.ModuleName = library;
    value.LibraryId = 26;
    value.ModuleId = 27;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::ScePadImports> pad = std::make_unique<Cpu::ScePadImports>(machine);
    std::shared_ptr<Cpu::PadHostInput> input = pad->Input();

    Session() {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(data, 0x4000, rw);
        machine.Map(0x7000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0,
            0x48, 0x89, 0x05, 0x03, 0x10, 0, 0,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }

    std::uint32_t call(const char* name, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0, std::uint64_t d = 0) {
        const auto gate = pad->Resolve(import(name));
        require(gate.has_value(), std::string("libScePad provider does not resolve ") + name);
        machine.Write(0x2000, std::as_bytes(std::span(&*gate, 1)));
        machine.Set(Register::Rdi, a);
        machine.Set(Register::Rsi, b);
        machine.Set(Register::Rdx, c);
        machine.Set(Register::Rcx, d);
        machine.Set(Register::Rsp, 0x7ff0);
        machine.Set(Register::Rbx, 1);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address && machine.Get(Register::Rbx) == 2,
                std::string(name) + " did not return to its x86 caller");
        std::uint64_t stored = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Guest did not store the pad RAX result");
        const auto result = static_cast<std::uint32_t>(stored);
        require(stored == static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(result))),
                std::string(name) + " result is not a sign-extended 32-bit status");
        return result;
    }

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t count) {
        std::vector<std::uint8_t> result(count);
        machine.Read(address, std::as_writable_bytes(std::span(result)));
        return result;
    }
    void fill(std::uint64_t address, std::size_t count) {
        const std::vector<std::uint8_t> sentinel(count, 0xa7);
        machine.Write(address, std::as_bytes(std::span(sentinel)));
    }
    template<class T> T at(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    std::uint32_t open() {
        require(call("scePadInit") == 0, "scePadInit failed");
        const auto handle = call("scePadOpen", user, 0, 0, 0);
        require(static_cast<std::int32_t>(handle) > 0, "scePadOpen did not return a positive handle");
        return handle;
    }
    struct Pad { std::uint32_t buttons; std::array<std::uint8_t, 4> sticks; std::uint8_t l2, r2; std::uint64_t timestamp; };
    Pad read(std::uint32_t handle) {
        fill(data, 0x80);
        require(call("scePadReadState", handle, data) == 0, "scePadReadState failed");
        return decode(data);
    }
    Pad decode(std::uint64_t base) {
        const auto raw = bytes(base, 0x78);
        Pad result{};
        std::memcpy(&result.buttons, raw.data(), 4);
        std::memcpy(result.sticks.data(), raw.data() + 4, 4);
        result.l2 = raw[8];
        result.r2 = raw[9];
        std::memcpy(&result.timestamp, raw.data() + 80, 8);
        return result;
    }
};

// Contract: ScePadData is the 0x78-byte PS5 record: buttons, sticks, analog triggers, orientation,
// acceleration, touch, connected flag at 0x4c, timestamp at 0x50 and connected count at 0x68.
// Regression: host-struct padding, a missing connected flag or a write past the record.
void dataLayout() {
    Session session;
    const auto handle = session.open();
    session.input->SetController({cross | l1 | r2, {10, 20, 230, 240}, 0, 200});
    session.input->Commit();
    session.fill(data, 0x80);
    require(session.call("scePadReadState", handle, data) == 0, "scePadReadState failed");
    const auto raw = session.bytes(data, 0x80);
    require(session.at<std::uint32_t>(data) == (cross | l1 | r2), "buttons differ at offset 0");
    require(raw[4] == 10 && raw[5] == 20 && raw[6] == 230 && raw[7] == 240, "stick bytes differ at offsets 4..7");
    require(raw[8] == 0 && raw[9] == 200, "analog triggers differ at offsets 8..9");
    require(session.at<float>(data + 0x0c) == 0.0f && session.at<float>(data + 0x18) == 1.0f,
            "orientation is not the identity quaternion at 0x0c..0x1b");
    require(session.at<float>(data + 0x20) == 1.0f && session.at<float>(data + 0x1c) == 0.0f,
            "resting acceleration is not 1 g on Y at 0x1c..0x27");
    require(raw[0x34] == 0, "touch count differs at 0x34");
    require(raw[0x4c] == 1, "connected flag differs at 0x4c");
    require(session.at<std::uint64_t>(data + 0x50) != 0, "timestamp missing at 0x50");
    require(raw[0x68] == 1, "connected count differs at 0x68");
    for (std::size_t index = 0x78; index < 0x80; ++index)
        require(raw[index] == 0xa7, "scePadReadState wrote past the 0x78-byte record");
}

// Contract: with no controller the keyboard and mouse drive the virtual pad; a connected
// controller replaces them. Regression: input never reaching the guest without a controller.
void keyboardMouseFallback() {
    Session session;
    const auto handle = session.open();
    auto& input = *session.input;
    input.Key(keyW, true);
    input.Key(keyReturn, true);
    input.Key(keyUp, true);
    input.MouseButton(1, true);
    input.MouseButton(2, true);
    input.Commit();
    auto state = session.read(handle);
    require(state.sticks == std::array<std::uint8_t, 4>{128, 0, 128, 128}, "W does not push the left stick up");
    require(state.buttons == (cross | up | square | r2), "keyboard/mouse buttons map incorrectly");
    require(state.r2 == 255, "a digital R2 does not report a full analog trigger");

    input.Key(keyW, false);
    input.Key(keyD, true);
    input.MouseButton(4, true);
    input.MouseButton(4, false);
    input.MouseMotion(40, -1000);
    input.Commit();
    state = session.read(handle);
    require(state.sticks[0] == 255 && state.sticks[1] == 128, "D does not push the left stick right");
    require(state.sticks[2] > 128 && state.sticks[3] == 0, "mouse look does not drive the right stick");
    input.Commit();
    state = session.read(handle);
    require(state.sticks[2] > 128 && state.sticks[3] == 0, "mouse look did not hold its deflection between pumps");
    std::this_thread::sleep_for(Cpu::PadHostInput::MouseHold + std::chrono::milliseconds(10));
    input.Key(keyUp, false);
    input.MouseWheel(1);
    input.Commit();
    state = session.read(handle);
    require(state.sticks[2] == 128 && state.sticks[3] == 128, "the right stick does not recentre without motion");
    require(state.buttons & up, "the mouse wheel does not press Up");
    std::this_thread::sleep_for(Cpu::PadHostInput::WheelHold + std::chrono::milliseconds(10));
    input.Commit();
    require(!(session.read(handle).buttons & up), "the mouse wheel press was not released");
    input.ResetKeyboardMouse();
    input.MouseMotion(50, 0);
    input.Commit();
    require(session.read(handle).sticks[2] == 128, "focus loss did not leave mouse look");
    input.MouseButton(4, true);
    input.MouseButton(4, false);
    input.Key(keyD, true);

    input.SetController({options, {128, 128, 128, 128}, 0, 0});
    input.Commit();
    state = session.read(handle);
    require(state.buttons == options && state.sticks == std::array<std::uint8_t, 4>{128, 128, 128, 128} && state.r2 == 0,
            "a connected controller does not replace keyboard and mouse input");
    input.DisconnectController();
    input.ResetKeyboardMouse();
    input.Key(keyEscape, true);
    input.Commit();
    state = session.read(handle);
    require(state.buttons == options && state.sticks[0] == 128, "focus reset or disconnect left stale input");
}

// Contract: scePadRead returns buffered samples oldest first, bounded at 64 with the oldest dropped,
// and the current state once nothing new is queued; each handle reads with its own cursor.
// Regression: unbounded or undrained input queues, or one handle consuming another's samples.
void bufferedRead() {
    Session session;
    const auto handle = session.open();
    const auto remote = session.call("scePadOpen", 0xff, 16, 0, 0);
    require(static_cast<std::int32_t>(remote) > 0 && remote != handle, "the system remote port did not get its own handle");
    constexpr int total = 100;
    for (int index = 0; index < total; ++index) {
        session.input->SetController({index % 2 ? 0u : cross, {static_cast<std::uint8_t>(index), 128, 128, 128}, 0, 0});
        session.input->Commit();
    }
    require(session.input->DroppedSamples() == total - 64, "the sample ring does not drop exactly the oldest overflow");
    require(session.call("scePadRead", handle, data, 0) == invalidArg, "scePadRead accepted num 0");
    require(session.call("scePadRead", handle, data, 65) == invalidArg, "scePadRead accepted num above 64");
    session.fill(data, 0x78 * 65);
    require(session.call("scePadRead", handle, data, 64) == 64, "scePadRead did not return all 64 buffered samples");
    std::uint64_t previous = 0;
    for (int index = 0; index < 64; ++index) {
        const auto sample = session.decode(data + 0x78 * index);
        const int expected = total - 64 + index;
        require(sample.sticks[0] == expected && sample.buttons == (expected % 2 ? 0u : cross),
                "scePadRead samples are not the newest 64 in order");
        require(sample.timestamp > previous, "scePadRead timestamps are not strictly increasing");
        previous = sample.timestamp;
    }
    require(session.bytes(data + 0x78 * 64, 8) == std::vector<std::uint8_t>(8, 0xa7), "scePadRead wrote past num records");
    session.fill(data, 0x78 * 2);
    require(session.call("scePadRead", handle, data, 2) == 1, "an empty queue does not report the current state once");
    require(session.decode(data).sticks[0] == total - 1 && session.decode(data).timestamp == previous,
            "the current state differs from the newest sample");
    session.fill(data, 0x78 * 64);
    require(session.call("scePadRead", remote, data, 64) == 64 && session.decode(data).sticks[0] == total - 64,
            "a second handle did not read the samples the first handle already read");
}

// Contract: SCE pad status codes for lifecycle and argument errors; no output on failure.
void errors() {
    Session session;
    require(session.call("scePadOpen", user, 0, 0, 0) == notInitialized, "scePadOpen before scePadInit succeeded");
    const auto handle = session.open();
    require(session.call("scePadOpen", user, 0, 0, 0) == handle, "reopening a port did not return its handle");
    require(session.call("scePadGetHandle", user, 0, 0) == handle, "scePadGetHandle differs from the open handle");
    require(session.call("scePadGetHandle", user, 0, 1) == noHandle, "scePadGetHandle found an unopened port");
    require(session.call("scePadOpen", user, 0, 1, 0) == invalidArg, "scePadOpen accepted a second port index");
    require(session.call("scePadReadState", handle + 7, data) == invalidHandle, "an unknown handle was accepted");
    require(session.call("scePadReadState", handle, 0) == invalidArg, "a null ScePadData was accepted");
    require(session.call("scePadReadState", handle, unmapped) == invalidArg, "an unmapped ScePadData was accepted");
    require(session.call("scePadSetLightBar", handle, 0) == invalidArg, "a null light bar was accepted");
    require(session.call("scePadClose", handle) == 0, "scePadClose failed");
    require(session.call("scePadReadState", handle, data) == invalidHandle, "a closed handle was accepted");
    require(session.call("scePadClose", handle) == invalidHandle, "a closed handle closed twice");
    require(!session.pad->Resolve(import("scePadInit", "libSceMouse")), "a libScePad NID resolved outside libScePad");
    require(!session.pad->Resolve(import("sceUnknownPadFunction")), "an unknown libScePad NID resolved");
}

// Contract: light bar, vibration and motion requests reach the host output the controller applies.
void hostOutput() {
    Session session;
    const auto handle = session.open();
    const auto start = session.input->Output().Sequence;
    const std::array<std::uint8_t, 3> color{1, 2, 3};
    session.machine.Write(data, std::as_bytes(std::span(color)));
    require(session.call("scePadSetLightBar", handle, data) == 0, "scePadSetLightBar failed");
    auto output = session.input->Output();
    require(output.LightBarValid && output.LightBar == color && output.Sequence > start, "light bar not published");
    require(session.call("scePadResetLightBar", handle) == 0 && !session.input->Output().LightBarValid,
            "scePadResetLightBar did not restore the default light bar");
    const std::array<std::uint8_t, 2> motors{200, 50};
    session.machine.Write(data, std::as_bytes(std::span(motors)));
    require(session.call("scePadSetVibration", handle, data) == 0, "scePadSetVibration failed");
    output = session.input->Output();
    require(output.LargeMotor == 200 && output.SmallMotor == 50, "vibration not published");
    require(session.call("scePadSetMotionSensorState", handle, 0) == 0 && !session.input->Output().MotionEnabled,
            "scePadSetMotionSensorState did not disable motion");
    require(session.call("scePadSetVibrationMode", handle, 1) == 0, "scePadSetVibrationMode rejected mode 1");
    require(session.call("scePadSetVibrationMode", handle, 2) == invalidArg, "scePadSetVibrationMode accepted mode 2");
}

// Contract: ScePadControllerInformation is the 0x1c-byte record of a local standard pad.
void controllerInformation() {
    Session session;
    const auto handle = session.open();
    session.fill(data, 0x24);
    require(session.call("scePadGetControllerInformation", handle, data) == 0, "scePadGetControllerInformation failed");
    const auto raw = session.bytes(data, 0x24);
    require(session.at<float>(data) == 44.86f && session.at<std::uint16_t>(data + 4) == 1920 &&
            session.at<std::uint16_t>(data + 6) == 943, "touch pad information differs");
    require(raw[8] == 2 && raw[9] == 2 && raw[10] == 0 && raw[11] == 1 && raw[12] == 1 &&
            session.at<std::int32_t>(data + 16) == 0, "stick/connection information differs");
    for (std::size_t index = 0x1c; index < 0x24; ++index)
        require(raw[index] == 0xa7, "scePadGetControllerInformation wrote past the 0x1c-byte record");
}
}

int main() {
    try {
        dataLayout();
        keyboardMouseFallback();
        bufferedRead();
        errors();
        hostOutput();
        controllerInformation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
