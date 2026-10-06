#include <cpu/SceAudioOut2Imports.hpp>
#include <cpu/NativeAudioOutput.hpp>
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
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
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& operation, const char* expected) {
    try { operation(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing audio boundary rejection: ") + expected);
}
template<class F> void eventually(F&& condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!condition()) {
        require(std::chrono::steady_clock::now() < deadline, "Real native audio callback deadline exceeded");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Primary keeper: actual native output owns acceptance/consumption/lifetime; real qualified x86 gates own
// guest ABI validation and session handles. Credible regressions are premature queue publication,
// partial attribute/output commits and recycled stale handles. Existing synchronous user/kernel/ISA
// fixtures and the separate SDL mixer tests do not traverse this provider or its Core Audio callback.
// No callback injection, fake backend or test-only production accessor is used. Counters prove progress
// and conservation, never PCM/channel identity, acoustic output, or Civilization VII playability.
void nativeQueueBoundary() {
    auto output = Cpu::NativeAudioOutput::Open({1024});
    require(bool(output), "Native audio output did not open");
    eventually([&] {
        const auto s = output->GetSnapshot();
        require(s.running && !s.nativeError, "Real output unit is not running");
        require(s.acceptedFrames == 0 && s.renderedFrames == 0 && s.queuedFrames == 0,
                "Empty callbacks counted silence as accepted or rendered PCM");
        return s.underrunFrames > 0;
    });
    const auto accepted = output->GetSnapshot().acceptedFrames;
    const std::array<float, 3> odd{0, 0, 0};
    std::vector<float> oversized(2050, 0);
    std::vector<float> nonfinite(1024, 0.001f);
    nonfinite.back() = std::numeric_limits<float>::quiet_NaN();
    for (const auto input : {std::span<const float>{}, std::span<const float>(odd),
                             std::span<const float>(oversized), std::span<const float>(nonfinite)}) {
        rejects([&] { (void)output->SubmitInterleavedStereo(input); }, "submission");
        require(output->GetSnapshot().acceptedFrames == accepted, "Rejected host block was partially accepted");
    }
    std::array<float, 1024> pcm{};
    for (std::size_t i = 0; i < pcm.size(); i += 2) { pcm[i] = 0.001f; pcm[i + 1] = -0.002f; }
    std::uint64_t total = 0;
    // Five independent 512-frame blocks cross the 1024-frame ring boundary repeatedly.
    for (unsigned block = 0; block < 5; ++block) {
        eventually([&] {
            const auto status = output->SubmitInterleavedStereo(pcm);
            if (status == Cpu::NativeAudioOutput::SubmitStatus::WouldBlock) {
                require(output->GetSnapshot().acceptedFrames == total, "Backpressure partially accepted a block");
                return false;
            }
            require(status == Cpu::NativeAudioOutput::SubmitStatus::Accepted, "Native output faulted during submission");
            return true;
        });
        total += 512;
        const auto s = output->GetSnapshot();
        require(s.acceptedFrames == total && s.acceptedFrames == s.renderedFrames + s.queuedFrames,
                "Native acceptance/render/queue counters do not conserve independently submitted frames");
        eventually([&] { return output->GetSnapshot().renderedFrames == total; });
    }
    // A bounded real producer burst must actually hit capacity; a conditional assertion would
    // never test backpressure when each earlier block was allowed to drain completely.
    bool blocked = false;
    const auto burstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!blocked && std::chrono::steady_clock::now() < burstDeadline) {
        const auto status = output->SubmitInterleavedStereo(pcm);
        if (status == Cpu::NativeAudioOutput::SubmitStatus::Accepted) total += 512;
        else {
            require(status == Cpu::NativeAudioOutput::SubmitStatus::WouldBlock, "Native burst faulted before backpressure");
            blocked = true;
        }
        const auto pending = output->GetSnapshot();
        require(pending.acceptedFrames == total && pending.queuedFrames <= pending.capacityFrames &&
                pending.acceptedFrames == pending.renderedFrames + pending.queuedFrames,
                "Burst acceptance/backpressure lost pending-frame conservation");
    }
    require(blocked, "Real bounded queue never exercised WouldBlock");
    eventually([&] { return output->GetSnapshot().renderedFrames == total; });
    require(output->SubmitInterleavedStereo(pcm) == Cpu::NativeAudioOutput::SubmitStatus::Accepted,
            "Final lifetime block was rejected");
    total += 512;
    output->Close();
    auto s = output->GetSnapshot();
    require(!s.running && !s.nativeError && s.queuedFrames == 0 && s.acceptedFrames == total &&
            s.acceptedFrames == s.renderedFrames + s.discardedFrames, "Close lost accepted or unconsumed frames");
    require(output->SubmitInterleavedStereo(pcm) == Cpu::NativeAudioOutput::SubmitStatus::Stopped,
            "Closed stream accepted another block");
    require(output->GetSnapshot().acceptedFrames == total, "Stopped submission changed accepted count");
    output->Close();
}

struct Guest {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceAudioOut2Imports> imports = std::make_unique<Cpu::SceAudioOut2Imports>(machine);
    Guest() {
        machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
        machine.Map(0x2000, 6 * 4096, rw);
        machine.Map(0x8000, 4096, rw);
        machine.Map(0x9000, 4096, Permission::Read);
        machine.Map(0x10000, 65536, rw);
        constexpr std::array<std::uint8_t, 17> code{0xff,0x15,0xfa,0x0f,0,0,
            0x48,0x89,0x05,0x03,0x10,0,0,0x48,0xff,0xc3,0x90};
        machine.Write(0x1000, std::as_bytes(std::span(code)));
    }
    template<class T> void put(std::uint64_t address, const T& value) {
        machine.Write(address, std::as_bytes(std::span(&value, 1)));
    }
    template<class T> T get(std::uint64_t address) {
        T value{}; machine.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
    std::uint64_t gate(const char* nid) {
        Cpu::SceImport value; value.Nid = nid; value.LibraryName = "libSceAudioOut2";
        value.ModuleName = "libSceAudioOut"; value.LibraryId = 36; value.ModuleId = 37;
        value.LibraryVersion = 1; value.ModuleMajor = 1; value.ModuleMinor = 1;
        return imports->Resolve(value).value();
    }
    std::uint64_t invoke(std::uint64_t target, std::uint64_t a = 0, std::uint64_t b = 0,
                         std::uint64_t c = 0, std::uint64_t d = 0) {
        put(0x2000, target);
        machine.Set(Register::Rdi, a); machine.Set(Register::Rsi, b);
        machine.Set(Register::Rdx, c); machine.Set(Register::Rcx, d);
        machine.Set(Register::Rsp, 0x4ff0); machine.Set(Register::Rbx, 0x76543210);
        constexpr std::array saved{Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (const auto r : saved) machine.Set(r, 0x123456789abcdef0);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address,
                "Actual x86 GOT caller failed to resume after audio gate");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x76543211,
                "Audio gate corrupted CALL/RET stack or skipped continuation");
        for (const auto r : saved) require(machine.Get(r) == 0x123456789abcdef0, "Audio gate corrupted callee-saved state");
        require(get<std::uint64_t>(0x2010) == machine.Get(Register::Rax), "Guest did not store actual gate status");
        return machine.Get(Register::Rax);
    }
    std::uint64_t call(const char* nid, std::uint64_t a = 0, std::uint64_t b = 0,
                       std::uint64_t c = 0, std::uint64_t d = 0) { return invoke(gate(nid), a, b, c, d); }
    void sentinel(std::uint64_t address, std::size_t length) {
        const std::vector<std::byte> bytes(length, std::byte{0xa7}); machine.Write(address, bytes);
    }
    void untouched(std::uint64_t address, std::size_t length) {
        std::vector<std::byte> bytes(length); machine.Read(address, bytes);
        require(std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{0xa7}; }),
                "Rejected guest output changed its writable prefix");
    }
};
struct Attribute { std::uint32_t id, reserved; std::uint64_t payload, size; };
static_assert(sizeof(Attribute) == 24);

void guestAudioBoundary() {
    Guest g;
    require(g.call("g2tViFIohHE") == 0, "Audio session initialization failed");
    g.sentinel(0x8fe0, 32);
    rejects([&] { g.call("t5YrizufpQc", 0x8fe0); }, "permission"); g.untouched(0x8fe0, 32);
    require(g.call("t5YrizufpQc", 0x5000) == 0, "Context parameter reset failed");
    // These bytes come from independently traced conditional actual-title defaults, not SDK labels.
    const std::array<std::uint32_t, 6> defaults{16, 128, 0, 4, 512, 1};
    require(g.get<std::array<std::uint32_t, 6>>(0x5000) == defaults, "Context defaults differ from traced caller profile");
    for (const auto nid : {"pDmme7Bgm6E", "xywYcRB7nbQ"}) {
        g.sentinel(0x8ffc, 4);
        rejects([&] { g.call(nid, std::string(nid) == "pDmme7Bgm6E" ? 0x5000 : 0x10000000, 0x8ffc); }, "permission");
        g.untouched(0x8ffc, 4);
    }
    require(g.call("pDmme7Bgm6E", 0x5000, 0x5100) == 0, "Context memory query failed");
    const auto scratch = g.get<std::uint64_t>(0x5100);
    require(scratch > 0 && scratch <= 65536, "Queried scratch does not fit caller-owned mapped buffer");
    g.sentinel(0x8ffc, 4);
    rejects([&] { g.call("0x6o1VVAYSY", 0x5000, 0x10000, scratch, 0x8ffc); }, "permission"); g.untouched(0x8ffc, 4);
    require(g.call("0x6o1VVAYSY", 0x5000, 0x10000, scratch, 0x5110) == 0, "Native context creation failed");
    const auto ctx = g.get<std::uint64_t>(0x5110);
    require(g.call("xywYcRB7nbQ", 0x10000000, 0x5120) == 0, "Local audio user creation failed");
    const auto user = g.get<std::uint64_t>(0x5120);
    rejects([&] { g.call("on6ZH7Abo10", user); }, "context handle");
    std::array<std::uint32_t, 16> portParams{}; portParams[0] = 0xbeef0000; portParams[1] = 0x200; portParams[2] = 48000;
    g.put(0x5200, portParams); g.put(0x5210, user);
    g.sentinel(0x8ffc, 4);
    rejects([&] { g.call("JK2wamZPzwM", ctx, 0x5200, 0x8ffc); }, "permission"); g.untouched(0x8ffc, 4);
    require(g.call("JK2wamZPzwM", ctx, 0x5200, 0x5250) == 0, "Float stereo main port creation failed");
    const auto port = g.get<std::uint64_t>(0x5250);
    rejects([&] { g.call("gatEUKG+Ea4", ctx, 0x5300); }, "port handle");
    g.sentinel(0x8fe0, 32);
    rejects([&] { g.call("gatEUKG+Ea4", port, 0x8fe0); }, "permission"); g.untouched(0x8fe0, 32);
    require(g.call("gatEUKG+Ea4", port, 0x5300) == 0, "Live native main port state failed");
    require(g.get<std::uint16_t>(0x5300) != 0 && g.get<std::uint8_t>(0x5302) == 2,
            "Virtual native output did not report its running stereo profile");
    g.sentinel(0x5540, 4); g.sentinel(0x8ffe, 2);
    rejects([&] { g.call("R7d0F1g2qsU", ctx, 0x5540, 0x8ffe); }, "permission");
    g.untouched(0x5540, 4); g.untouched(0x8ffe, 2);
    std::array<float, 1024> pcm{}; g.put(0x6000, pcm); g.put(0x5500, std::uint64_t{0x6000});
    g.put(0x5520, std::array<float, 2>{1, 1});
    const std::array<Attribute, 2> attributes{{{0,0xdeadbeef,0x5500,8},{1,0x80001234,0x5520,8}}};
    g.put(0x5400, attributes);
    require(g.call("8XTArSPyWHk", port, 0x5400, 2) == 0, "Owned PCM/gain attributes failed");
    // A later bad gain must not commit an earlier replacement PCM pointer from the same batch.
    g.put(0x5500, std::uint64_t{0xa000});
    g.put(0x5520, std::array<float, 2>{1, std::numeric_limits<float>::quiet_NaN()});
    rejects([&] { g.call("8XTArSPyWHk", port, 0x5400, 2); }, "nonfinite");
    require(g.call("aII9h5nli9U", ctx, 1) == 0, "Rejected batch partially replaced prior valid PCM pointer");
    auto invalidGainSpan = attributes; invalidGainSpan[1].payload = 0x9ffc;
    g.put(0x5400, invalidGainSpan);
    rejects([&] { g.call("8XTArSPyWHk", port, 0x5400, 2); }, "Guest access denied");
    require(g.call("aII9h5nli9U", ctx, 1) == 0, "Unreadable nested gain partially committed earlier PCM attribute");
    auto invalidPcmPayload = attributes; invalidPcmPayload[0].payload = 0x9ffc;
    g.put(0x5400, invalidPcmPayload);
    rejects([&] { g.call("8XTArSPyWHk", port, 0x5400, 2); }, "Guest access denied");
    require(g.call("aII9h5nli9U", ctx, 1) == 0, "Unreadable nested PCM address changed the existing association");
    g.put(0x5400, attributes);
    g.put(0x5520, std::array<float, 2>{1, 1});
    require(g.call("8XTArSPyWHk", port, 0x5400, 2) == 0, "Deferred guest PCM address association failed");
    rejects([&] { g.call("aII9h5nli9U", ctx, 1); }, "Guest access denied");
    g.put(0x5500, std::uint64_t{0x6000}); g.call("8XTArSPyWHk", port, 0x5400, 2);
    pcm.back() = std::numeric_limits<float>::infinity(); g.put(0x6000, pcm);
    rejects([&] { g.call("aII9h5nli9U", ctx, 1); }, "nonfinite");
    pcm.back() = 0; g.put(0x6000, pcm);
    require(g.call("aII9h5nli9U", ctx, 1) == 0, "Valid Push could not recover after invalid PCM rejection");
    for (const auto unsupportedType : {std::uint16_t{6}, std::uint16_t{0x100}}) {
        g.put(0x5200, unsupportedType); g.sentinel(0x5250, 8);
        rejects([&] { g.call("JK2wamZPzwM", ctx, 0x5200, 0x5250); }, "main mono/stereo");
        g.untouched(0x5250, 8);
    }
    g.put(0x5200, std::uint16_t{0});
    for (const auto nid : {"PE2zHMqLSHs", "TViD1EZXkNI", "XHl38ZNknbs"})
        rejects([&] { g.call(nid, ctx); }, "not established");
    rejects([&] { g.call("IaZXJ9M79uo", user); }, "owns ports");
    g.call("cd+Rtw+D1x8", port);
    g.call("JK2wamZPzwM", ctx, 0x5200, 0x5250);
    const auto replacement = g.get<std::uint64_t>(0x5250);
    require(replacement != port, "Destroyed port handle was recycled");
    rejects([&] { g.call("gatEUKG+Ea4", port, 0x5300); }, "stale");
    require(g.call("gatEUKG+Ea4", replacement, 0x5300) == 0, "Stale rejection damaged replacement port");
    g.call("on6ZH7Abo10", ctx);
    rejects([&] { g.call("gatEUKG+Ea4", replacement, 0x5300); }, "stale");
    rejects([&] { g.call("aII9h5nli9U", ctx, 1); }, "stale");
    require(g.call("0x6o1VVAYSY", 0x5000, 0x10000, scratch, 0x5110) == 0, "Replacement native context failed");
    const auto replacementContext = g.get<std::uint64_t>(0x5110);
    require(replacementContext != ctx, "Destroyed context handle was recycled");
    rejects([&] { g.call("on6ZH7Abo10", ctx); }, "stale");
    // Leave this real native context active to exercise provider-owned callback teardown below.
    g.call("IaZXJ9M79uo", user);
    rejects([&] { g.call("IaZXJ9M79uo", user); }, "stale");
    const auto expiredGate = g.gate("g2tViFIohHE"); g.imports.reset();
    rejects([&] { g.invoke(expiredGate); }, "expired");
}
}

int main() {
    try {
        nativeQueueBoundary(); guestAudioBoundary();
        std::cout << "PASS real native audio callback accounting/lifetime and checked x86 AudioOut2 gates; PCM identity/audibility unverified\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
