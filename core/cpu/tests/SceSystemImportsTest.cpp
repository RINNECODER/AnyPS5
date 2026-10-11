#include <cpu/SceSystemImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t parameterError = 0xffffffff80a10003ULL;
constexpr std::uint64_t noEvent = 0xffffffff80a10004ULL;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing system-service rejection: ") + expected);
}
Cpu::SceImport qualified(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid; value.LibraryName = "libSceSystemService"; value.ModuleName = "libSceSystemService";
    value.LibraryId = 28; value.ModuleId = 29; value.LibraryVersion = 1; value.ModuleMajor = 1; value.ModuleMinor = 1;
    return value;
}
struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceSystemImports> imports = std::make_unique<Cpu::SceSystemImports>(machine);
    Session() {
        machine.Map(0x1000, 4096, rx); machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw); machine.Map(0x4000, 4096, rw); machine.Map(0x6000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0, 0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }
    std::uint64_t gate(const char* nid) { return imports->Resolve(qualified(nid)).value(); }
    std::uint64_t callGate(std::uint64_t address, std::uint64_t first = 0, std::uint64_t second = 0, std::uint64_t third = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, first); machine.Set(Register::Rsi, second); machine.Set(Register::Rdx, third);
        machine.Set(Register::Rsp, 0x4ff0); machine.Set(Register::Rbx, 0x123456789abcdeff);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "System gate did not return to real x86 GOT caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "System gate corrupted guest CALL/RET continuation");
        std::uint64_t stored = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Guest did not store actual system-service RAX result");
        return stored;
    }
    std::uint64_t call(const char* nid, std::uint64_t first = 0, std::uint64_t second = 0, std::uint64_t third = 0) {
        return callGate(gate(nid), first, second, third);
    }
    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t count) {
        std::vector<std::uint8_t> result(count);
        machine.Read(address, std::as_writable_bytes(std::span(result))); return result;
    }
    void fill(std::uint64_t address, std::size_t count) {
        const std::vector<std::uint8_t> sentinel(count, 0xa7);
        machine.Write(address, std::as_bytes(std::span(sentinel)));
    }
};

// Contract: the virtual console profile answers every integer ID with four LE bytes and OK, including
// ENTER_BUTTON_ASSIGN (1000 -> Cross) and DATE/TIME_FORMAT/PARENTAL; unknown IDs read as 0 like upstream.
// Regression: a host exception for an ID nearly every title queries at boot, native-width writes, or writing
// before the complete span is checked. This is the public x86 gate boundary without mocks.
void integerParameters() {
    Session session;
    for (const auto [id, expected] : std::array<std::pair<std::uint64_t, std::uint8_t>, 10>{{
             {1, 1}, {2, 1}, {3, 1}, {4, 0}, {5, 0}, {7, 0}, {1000, 1}, {6, 0}, {999, 0}, {0xffffffff00000001ULL, 1}}}) {
        session.fill(0x3000, 8);
        require(session.call("fZo48un7LK4", id, 0x3001) == 0, "Integer profile parameter did not return OK");
        require(session.bytes(0x3000, 6) == std::vector<std::uint8_t>{0xa7, expected, 0, 0, 0, 0xa7},
                "Integer output was not the profile value as four little-endian bytes at an unaligned guest address");
    }
    session.fill(0x6ffe, 2);
    require(session.call("fZo48un7LK4", 1000, 0x6ffe) == parameterError, "Integer output accepted an unmapped suffix");
    require(session.bytes(0x6ffe, 2) == std::vector<std::uint8_t>(2, 0xa7), "Integer output partially wrote before unmapped suffix");
    session.machine.Map(0x7000, 4096, Permission::Read); session.fill(0x6ffe, 4);
    for (const auto address : std::array<std::uint64_t, 5>{0, 0x9000, 0x7000, 0x6ffe, std::numeric_limits<std::uint64_t>::max() - 1}) {
        require(session.call("fZo48un7LK4", 1, address) == parameterError, "Bad integer span lost signed parameter error");
        require(session.bytes(0x6ffe, 4) == std::vector<std::uint8_t>(4, 0xa7), "Rejected integer span changed valid bytes");
    }
    session.machine.Protect(0x7000, 4096, rw);
    require(session.call("fZo48un7LK4", 1000, 0x6ffe) == 0 && session.bytes(0x6ffe, 4) == std::vector<std::uint8_t>{1, 0, 0, 0},
            "Integer output rejected adjacent writable guest mappings");
}

// Contract: system name preflights full capacity but writes only AnyPS5 plus NUL, preserving the remaining bytes.
// Regression: validating only seven bytes, zeroing caller tail, or silently accepting unsupported capacities/IDs.
// Integer outputs cannot establish a declared-capacity string contract; all expectations are independent fixed bytes.
void stringParameters() {
    Session session;
    for (const auto capacity : {65u, 96u}) {
        session.fill(0x3000, 100);
        require(session.call("SsC-m-S9JTA", 6, 0x3001, capacity) == 0, "Supported system-name capacity failed");
        auto expected = std::vector<std::uint8_t>(capacity + 2, 0xa7);
        constexpr std::array<std::uint8_t, 7> name{'A', 'n', 'y', 'P', 'S', '5', 0};
        for (std::size_t i = 0; i < name.size(); ++i) expected[i + 1] = name[i];
        require(session.bytes(0x3000, capacity + 2) == expected, "System name changed tail or adjacent bytes");
    }
    session.fill(0x3000, 100);
    require(session.call("SsC-m-S9JTA", 6, 0, 65) == parameterError, "Null system-name pointer lost signed parameter error");
    require(session.call("SsC-m-S9JTA", 6, 0x3000, 0) == parameterError, "Zero system-name capacity lost signed parameter error");
    for (const auto capacity : {1u, 64u, 16u * 1024 * 1024 + 1}) {
        rejects([&] { session.call("SsC-m-S9JTA", 6, 0x3000, capacity); }, "Unsupported SCE system service name buffer capacity");
        require(session.bytes(0x3000, 100) == std::vector<std::uint8_t>(100, 0xa7), "Unsupported capacity changed name output");
    }
    rejects([&] { session.call("SsC-m-S9JTA", 1, 0x3000, 65); }, "Unsupported SCE system service string parameter");
    require(session.bytes(0x3000, 100) == std::vector<std::uint8_t>(100, 0xa7), "Wrong string ID changed output");
    session.fill(0x6ff9, 7);
    require(session.call("SsC-m-S9JTA", 6, 0x6ff9, 65) == parameterError, "System name checked only its seven written bytes");
    require(session.bytes(0x6ff9, 7) == std::vector<std::uint8_t>(7, 0xa7), "Name wrote before rejecting unmapped capacity tail");
    session.machine.Map(0x7000, 4096, Permission::Read); session.fill(0x6ff9, 65);
    for (const auto address : std::array<std::uint64_t, 4>{0x7000, 0x6ff9, 0x9000, std::numeric_limits<std::uint64_t>::max() - 1}) {
        require(session.call("SsC-m-S9JTA", 6, address, 65) == parameterError, "Bad name span lost signed parameter error");
        require(session.bytes(0x6ff9, 65) == std::vector<std::uint8_t>(65, 0xa7), "Rejected name capacity changed mapped bytes");
    }
    session.machine.Protect(0x7000, 4096, rw);
    require(session.call("SsC-m-S9JTA", 6, 0x6ff9, 65) == 0, "System name rejected fully writable adjacent mappings");
    auto crossed = std::vector<std::uint8_t>(65, 0xa7);
    constexpr std::array<std::uint8_t, 7> name{'A', 'n', 'y', 'P', 'S', '5', 0};
    for (std::size_t i = 0; i < name.size(); ++i) crossed[i] = name[i];
    require(session.bytes(0x6ff9, 65) == crossed, "Cross-page system name changed capacity tail");
    constexpr std::uint64_t maximum = 16 * 1024 * 1024;
    session.machine.Map(0x1000000, maximum, rw); session.fill(0x1000000, 8); session.fill(0x1000000 + maximum - 1, 1);
    require(session.call("SsC-m-S9JTA", 6, 0x1000000, maximum) == 0, "Maximum supported name capacity was rejected");
    require(session.bytes(0x1000000, 8) == std::vector<std::uint8_t>{'A', 'n', 'y', 'P', 'S', '5', 0, 0xa7} &&
            session.bytes(0x1000000 + maximum - 1, 1) == std::vector<std::uint8_t>{0xa7}, "Maximum name capacity clobbered caller tail");
}

// Contract: GetStatus zeroes the full 136-byte status and returns OK; ReceiveEvent returns the signed NO_EVENT
// code without touching its buffer; HDR reports SDR reference white (100/100/0 nits as LE floats); the player
// dialog initializer and launcher accept a non-null parameter. Every null pointer returns the signed parameter
// error, and unwritable outputs are rejected before any byte is written.
// Regression: per-frame GetStatus == OK checks taking error paths, ReceiveEvent reporting a hard failure instead
// of "no event", or the dialog initializer aborting the host process.
void statusEventsAndDialogs() {
    Session session;
    const auto splash = session.gate("Vo5V8KAwCmk");
    require(session.callGate(splash) == 0 && session.callGate(splash) == 0, "Hide splash was not idempotent success");
    struct Output { const char* nid; std::vector<std::uint8_t> expected; };
    const std::vector<Output> outputs{
        {"rPo6tV8D9bM", std::vector<std::uint8_t>(136, 0)},
        {"mPpPxv5CZt4", {0, 0, 0xc8, 0x42, 0, 0, 0xc8, 0x42, 0, 0, 0, 0}}};
    for (const auto& output : outputs) {
        const auto size = output.expected.size();
        session.fill(0x3000, 256);
        require(session.call(output.nid, 0x3003) == 0, "Status/HDR query did not return OK");
        require(session.bytes(0x3003, size) == output.expected, "Status/HDR output differs from the independent ABI oracle");
        require(session.bytes(0x3002, 1) == std::vector<std::uint8_t>{0xa7} &&
                session.bytes(0x3003 + size, 1) == std::vector<std::uint8_t>{0xa7}, "Status/HDR wrote outside its declared span");
        const std::uint64_t boundary = 0x7000 - size / 2;
        session.fill(boundary, size / 2);
        for (const auto address : std::array<std::uint64_t, 4>{0, boundary, 0x9000, std::numeric_limits<std::uint64_t>::max() - 1})
            require(session.call(output.nid, address) == parameterError, "Bad status/HDR span lost signed parameter error");
        require(session.bytes(boundary, size / 2) == std::vector<std::uint8_t>(size / 2, 0xa7),
                "Status/HDR partially wrote before rejecting an unmapped suffix");
    }
    // Native x86 consumers branch on the sign of EAX: NO_EVENT is negative and must reach the "no event" path.
    session.fill(0x3000, 256);
    require(session.call("656LMQSrg6U", 0x3000) == noEvent, "ReceiveEvent did not return signed NO_EVENT");
    require(session.bytes(0x3000, 256) == std::vector<std::uint8_t>(256, 0xa7), "ReceiveEvent changed its event buffer");
    const std::array<std::uint8_t, 16> branch{
        0x85, 0xc0, 0x78, 0x07, 0xbb, 0x44, 0, 0, 0, 0xeb, 0x05, 0xbb, 0x66, 0, 0, 0};
    session.machine.Write(0x1040, std::as_bytes(std::span(branch)));
    require(session.machine.Run(0x1040, 0x1050, 100) == Cpu::StopReason::Address && session.machine.Get(Register::Rbx) == 0x66,
            "Guest EAX sign branch did not take the no-event continuation");
    require(session.call("656LMQSrg6U", 0) == parameterError, "Null event buffer lost signed parameter error");
    for (const auto nid : {"m5CYKX20wfg", "uaieF+glFPs"}) {
        session.fill(0x3000, 256);
        require(session.call(nid, 0x3000) == 0, "Player dialog parameter call did not return OK");
        require(session.bytes(0x3000, 256) == std::vector<std::uint8_t>(256, 0xa7), "Player dialog call changed its parameter");
        require(session.call(nid, 0) == parameterError, "Null player dialog parameter lost signed parameter error");
    }
    // Distinct resolver ownership: preserve qualified local-ID cache and leave unrelated namespaces to other resolvers.
    const auto original = qualified("Vo5V8KAwCmk");
    require(session.imports->Resolve(original).value() == splash, "Qualified system gate cache changed function address");
    auto other = original; other.LibraryId = 3; other.ModuleId = 4;
    const auto local = session.imports->Resolve(other).value();
    require(local != splash && session.callGate(local) == 0, "System resolver discarded local IDs or rejected matching public scope");
    for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
        auto wrong = original;
        switch (mismatch) {
        case 0: wrong.LibraryName = "libkernel"; break;
        case 1: wrong.ModuleName = "other"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 2; break;
        }
        rejects([&] { session.imports->Resolve(wrong); }, "scope/version");
    }
    auto unrelated = original; unrelated.LibraryName = "libc"; unrelated.ModuleName = "libc";
    require(!session.imports->Resolve(unrelated), "System resolver captured unrelated scope");
    auto unknown = original; unknown.Nid = "AAAAAAAAAAA";
    rejects([&] { session.imports->Resolve(unknown); }, "Unsupported SCE system import service");
    session.imports.reset();
    rejects([&] { session.callGate(splash); }, "runtime has expired");
}
}
int main() {
    try {
        integerParameters(); stringParameters(); statusEventsAndDialogs();
        std::cout << "PASS actual x86 system-service gates, console profile defaults, status/event/HDR ABI, capacity safety and scope\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
