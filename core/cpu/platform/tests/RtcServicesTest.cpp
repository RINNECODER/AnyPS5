#include "../RtcServices.hpp"
#include <cpu/SceElf.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using Cpu::Permission; using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t pointerError = 0xffffffff80b50002ULL;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& function, const char* expected) {
    try { function(); } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error("Expected RTC rejection missing");
}
Cpu::SceImport identity(const char* nid) {
    Cpu::SceImport value; value.Nid = nid;
    value.LibraryName = "libSceRtc"; value.ModuleName = "libSceRtc";
    value.LibraryId = 1; value.ModuleId = 2; value.LibraryVersion = 1; value.ModuleMajor = 1; value.ModuleMinor = 1;
    return value;
}
struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::Platform::RtcServices> service;
    Session() : service(std::make_unique<Cpu::Platform::RtcServices>(machine)) {
        machine.Map(0x1000, 4096, rx); machine.Map(0x2000, 4096, rw);
        machine.Map(0x4000, 4096, rw); machine.Map(0x6000, 4096, rw);
        constexpr std::array<unsigned char, 6> code{0xff, 0x15, 0xfa, 0x0f, 0, 0};
        machine.Write(0x1000, std::as_bytes(std::span(code)));
    }
    std::uint64_t gate(bool format) { return service->Resolve(identity(format ? "WJ3rqFwymew" : "99bMGglFW3I"), 2).value(); }
    std::uint64_t callGate(std::uint64_t address, std::uint64_t out, std::uint64_t in, int offset = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, out); machine.Set(Register::Rsi, in); machine.Set(Register::Rdx, static_cast<std::uint32_t>(offset));
        machine.Set(Register::Rsp, 0x4ff0);
        require(machine.Run(0x1000, 0x1006, 100) == Cpu::StopReason::Address, "RTC did not return to x86 caller");
        require(machine.Get(Register::Rsp) == 0x4ff0, "RTC CALL/RET changed stack");
        return machine.Get(Register::Rax);
    }
    std::uint64_t call(bool format, std::uint64_t out, std::uint64_t in, int offset = 0) { return callGate(gate(format), out, in, offset); }
    void text(const char* value) { machine.Write(0x2100, std::as_bytes(std::span(value, std::strlen(value) + 1))); }
    void tick(std::uint64_t value) { machine.Write(0x2200, std::as_bytes(std::span(&value, 1))); }
    std::uint64_t outputTick() { std::uint64_t value; machine.Read(0x2301, std::as_writable_bytes(std::span(&value, 1))); return value; }
    std::vector<std::byte> bytes(std::uint64_t address, std::size_t length) {
        std::vector<std::byte> value(length); machine.Read(address, value); return value;
    }
    void fill(std::uint64_t address, std::size_t length) { machine.Write(address, std::vector<std::byte>(length, std::byte{0xa7})); }
};
// Contract: independent known dates map to microseconds from Gregorian year 1;
// parsed offsets subtract local-minus-UTC, including century leap boundaries.
// Regression: wrong epoch, fractional scale, leap rule, offset sign or validation;
// host RTC tests do not exercise CPU import memory or guest error continuation.
void calendar() {
    Session session;
    struct Case { const char* input; std::uint64_t expected; };
    const std::array cases{
        Case{"0001-01-01T00:00:00Z", 0},
        Case{"1970-01-01T00:00:00Z", 62135596800000000ULL},
        Case{"2000-01-01T00:00:00Z", 63082281600000000ULL},
        Case{"2024-02-29T14:04:56.789+01:30", 63844806896789000ULL},
        Case{"2024-02-29t07:34:56.789123456-05:00", 63844806896789123ULL},
        Case{"9999-12-31T23:59:59.999999Z", 315537897599999999ULL}};
    for (const auto& entry : cases) {
        session.text(entry.input); session.fill(0x2300, 10);
        require(session.call(false, 0x2301, 0x2100) == 0 && session.outputTick() == entry.expected, "RTC independent date oracle failed");
        require(session.bytes(0x2300, 1)[0] == std::byte{0xa7} && session.bytes(0x2309, 1)[0] == std::byte{0xa7}, "RTC tick wrote outside eight bytes");
    }
    struct Invalid { const char* text; std::uint32_t error; };
    for (const auto& entry : std::array{
        Invalid{"1900-02-29T00:00:00Z", 0x80b5000a}, Invalid{"2000-13-01T00:00:00Z", 0x80b50009},
        Invalid{"2000-01-01T24:00:00Z", 0x80b5000b}, Invalid{"2000-01-01T00:60:00Z", 0x80b5000c},
        Invalid{"2000-01-01T00:00:61Z", 0x80b5000d}, Invalid{"2000-01-01T00:00:60Z", 0x80b50005},
        Invalid{"2000-01-01T00:00:00-00:00", 0x80b50005}, Invalid{"2000-01-01T00:00:00+01:99", 0x80b50007},
        Invalid{"2000-01-01T00:00:00Zjunk", 0x80b50007}, Invalid{"2000-01-01T00:00:00.Z", 0x80b50007},
        Invalid{"0001-01-01T00:00:00+00:01", 0x80b50003}, Invalid{"0000-01-01T00:00:00Z", 0x80b50008}}) {
        session.text(entry.text); session.fill(0x2301, 8);
        const auto status = session.call(false, 0x2301, 0x2100);
        require(static_cast<std::uint32_t>(status) == entry.error && status >> 32 == 0xffffffffULL, "RTC invalid calendar lost exact signed error");
        require(session.bytes(0x2301, 8) == std::vector<std::byte>(8, std::byte{0xa7}), "RTC failed parse changed tick");
    }
    session.tick(63844806896789000ULL); session.fill(0x2400, 40);
    require(session.call(true, 0x2401, 0x2200, -300) == 0, "RTC formatter rejected valid offset");
    constexpr char expected[] = "2024-02-29T07:34:56.78-05:00";
    require(session.bytes(0x2401, sizeof expected) == std::vector<std::byte>(std::as_bytes(std::span(expected)).begin(), std::as_bytes(std::span(expected)).end()), "RTC formatter independent text oracle failed");
    require(session.bytes(0x2400, 1)[0] == std::byte{0xa7} && session.bytes(0x241e, 1)[0] == std::byte{0xa7}, "RTC formatter overran exact output");
    session.fill(0x2400, 40);
    require(session.call(true, 0x2401, 0x2200, -30) == 0, "RTC formatter rejected negative sub-hour offset");
    constexpr char subhour[] = "2024-02-29T12:04:56.78-00:30";
    require(session.bytes(0x2401, sizeof subhour) == std::vector<std::byte>(std::as_bytes(std::span(subhour)).begin(), std::as_bytes(std::span(subhour)).end()), "RTC negative sub-hour lost offset sign or calendar adjustment");
    require(session.bytes(0x2400, 1)[0] == std::byte{0xa7} && session.bytes(0x241e, 1)[0] == std::byte{0xa7}, "RTC sub-hour formatting overran exact output");
    session.tick(315537897599999999ULL); session.fill(0x2400, 32);
    require(session.call(true, 0x2400, 0x2200, 1) == 0xffffffff80b50003ULL && session.bytes(0x2400, 32) == std::vector<std::byte>(32, std::byte{0xa7}), "RTC max-tick timezone overflow changed output");
    session.tick(0);
    require(session.call(true, 0x2400, 0x2200) == 0, "RTC formatter rejected first calendar day");
    constexpr char first[] = "0001-01-01T00:00:00.00Z";
    require(session.bytes(0x2400, sizeof first) == std::vector<std::byte>(std::as_bytes(std::span(first)).begin(), std::as_bytes(std::span(first)).end()), "RTC formatter year-one oracle failed");
    session.tick(63844806896789000ULL);
    for (int offset : {1440, -1440, std::numeric_limits<int>::min()}) {
        session.fill(0x2401, 32);
        require(session.call(true, 0x2401, 0x2200, offset) == 0xffffffff80b50003ULL && session.bytes(0x2401, 32) == std::vector<std::byte>(32, std::byte{0xa7}), "RTC invalid offset wrote output");
    }
}
// Contract: permission/range checks are atomic, source is read before overlapping
// output writes, resolver scope/type/version is exact and destruction releases
// owned gate memory/callbacks so a replacement can execute on the same Machine.
// Regression: host pointer dereference, partial write or retained gate ownership;
// only this new platform provider owns these gates and their lifetime.
void safety() {
    Session session;
    session.text("1970-01-01T00:00:00Z"); session.tick(62135596800000000ULL);
    session.machine.Map(0x7000, 4096, Permission::Read); session.fill(0x6ffb, 12);
    for (auto pointer : std::array<std::uint64_t, 5>{0, 0xdead0000, 0x7000, 0x6ffb, ~std::uint64_t{0} - 2}) {
        require(session.call(false, pointer, 0x2100) == pointerError, "RTC accepted invalid tick output");
        require(session.bytes(0x6ffb, 12) == std::vector<std::byte>(12, std::byte{0xa7}), "RTC partially wrote protected output");
    }
    require(session.call(true, 0x6ffb, 0x2200) == pointerError && session.bytes(0x6ffb, 12) == std::vector<std::byte>(12, std::byte{0xa7}), "RTC partially wrote protected formatted string");
    session.machine.Protect(0x7000, 4096, rw);
    require(session.call(false, 0x6ffb, 0x2100) == 0, "RTC rejected adjacent writable mappings");
    session.machine.Protect(0x7000, 4096, Permission::Write);
    require(session.call(true, 0x2400, 0x6ffb) == pointerError, "RTC accepted a tick crossing unreadable memory");
    session.fill(0x6fff, 1);
    require(session.call(false, 0x2301, 0x6fff) == pointerError, "RTC scanned unreadable string suffix");
    session.text("1970-01-01T00:00:00Z");
    require(session.call(false, 0x2100, 0x2100) == 0, "RTC parse failed valid aliasing");
    require(session.bytes(0x2100, 8) == session.bytes(0x2200, 8), "RTC overlap wrote before reading text");
    const auto original = identity("WJ3rqFwymew"); const auto gate = session.service->Resolve(original, 2).value();
    require(session.service->Resolve(original, 2).value() == gate, "RTC qualified cache changed address");
    auto different = original; different.ModuleId = 99;
    require(session.service->Resolve(different, 2).value() != gate, "RTC resolver discarded local import identity");
    for (unsigned index = 0; index < 5; ++index) {
        auto wrong = original;
        switch (index) { case 0: wrong.LibraryName = "libc"; break; case 1: wrong.ModuleName = "libc"; break; case 2: wrong.LibraryVersion = 2; break; case 3: wrong.ModuleMajor = 2; break; case 4: wrong.ModuleMinor = 2; break; }
        rejects([&]{ session.service->Resolve(wrong, 2); }, "scope/type/version");
    }
    for (unsigned char type : {0, 1, 6}) rejects([&]{ session.service->Resolve(original, type); }, "scope/type/version");
    auto unrelated = original; unrelated.LibraryName = "libc"; unrelated.ModuleName = "libc";
    require(!session.service->Resolve(unrelated, 2), "RTC captured unrelated namespace");
    auto unknown = original; unknown.Nid = "AAAAAAAAAAA"; rejects([&]{ session.service->Resolve(unknown, 2); }, "NID");
    const auto parseGate = session.gate(false);
    session.service.reset();
    rejects([&]{ session.machine.CheckAccess(gate, 1, Permission::Execute); }, "Guest access denied");
    session.service = std::make_unique<Cpu::Platform::RtcServices>(session.machine);
    require(session.gate(false) == parseGate, "Replacement RTC runtime failed to reuse the released gate base");
    session.text("1970-01-01T00:00:00Z"); session.fill(0x2301, 8);
    require(session.call(false, 0x2301, 0x2100) == 0 && session.outputTick() == 62135596800000000ULL,
            "Replacement RTC runtime retained an expired callback or failed its independent parse oracle");
}
struct Context {
    std::uint64_t parse, format; char input[128], output[32];
    std::uint64_t parsed, source, mask; int status[8];
};
void compiledGuest(const char* path) {
    std::ifstream file(path, std::ios::binary); require(bool(file), "RTC guest .text missing");
    const std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
    require(!code.empty() && code.size() < 65536, "RTC guest .text size invalid");
    Session session;
    session.machine.Map(0x100000, 65536, rx); session.machine.Map(0x20000, 4096, rw);
    session.machine.Write(0x100000, std::as_bytes(std::span(code)));
    Context context{}; context.parse = session.gate(false); context.format = session.gate(true);
    std::strcpy(context.input, "2024-02-29T14:04:56.789+01:30"); std::memset(context.output, 0xa7, sizeof context.output);
    session.machine.Write(0x20000, std::as_bytes(std::span(&context, 1)));
    constexpr std::uint64_t stop = 0x1000;
    session.machine.Write(0x4fe8, std::as_bytes(std::span(&stop, 1)));
    session.machine.Set(Register::Rsp, 0x4fe8); session.machine.Set(Register::Rdi, 0x20000);
    require(session.machine.Run(0x100000, stop, 100000) == Cpu::StopReason::Address, "Compiled RTC x86 guest did not return");
    session.machine.Read(0x20000, std::as_writable_bytes(std::span(&context, 1)));
    require(context.mask == 255 && session.machine.Get(Register::Rax) == 255 && context.parsed == 63844806896780000ULL, "Compiled RTC guest independent status/tick oracle failed");
    require(std::strcmp(context.output, "2024-02-29T14:04:56.78+01:30") == 0 && static_cast<unsigned char>(context.output[29]) == 0xa7, "Compiled RTC guest output or redzone changed");
    require(session.machine.Get(Register::Rsp) == 0x4ff0, "Compiled RTC guest stack cleanup failed");
}
}
int main(int argc, char** argv) {
    try { calendar(); safety(); if (argc > 1) compiledGuest(argv[1]);
        std::cout << "PASS RTC guest calendar, RFC3339, atomic memory, strict resolver and lifetime" << (argc > 1 ? "; compiled x86 fixture" : "; compiled fixture NOT RUN") << '\n'; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
