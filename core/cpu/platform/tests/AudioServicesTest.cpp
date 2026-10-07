#include "../AudioServices.hpp"
#include <cpu/SceElf.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace {
using Cpu::Register;
using Cpu::Permission;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Cpu::SceImport identity(std::size_t op) {
    Cpu::SceImport value;
    value.Nid = Cpu::Platform::AudioServices::Nids[op];
    value.LibraryName = value.ModuleName = "libSceAjm";
    value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
    return value;
}
struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::Platform::AudioServices> imports;
    std::array<std::uint64_t, 3> gates;
    Session() : imports(std::make_unique<Cpu::Platform::AudioServices>(machine)) {
        machine.Map(0x1000, 4096, rw); machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw); machine.Map(0x4000, 4096, rw);
        for (std::size_t i = 0; i < gates.size(); ++i) gates[i] = imports->Resolve(identity(i), 2).value();
    }
    std::uint32_t call(std::size_t op, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
        // MOVABS RAX,gate; CALL RAX; NOP -- actual x86 caller and continuation.
        std::array<std::byte, 13> code{std::byte{0x48}, std::byte{0xb8}};
        for (int i = 0; i < 8; ++i) code[2 + i] = std::byte((gates[op] >> (8 * i)) & 255);
        code[10] = std::byte{0xff}; code[11] = std::byte{0xd0}; code[12] = std::byte{0x90};
        machine.Write(0x1000, code); machine.Protect(0x1000, 4096, rx);
        machine.Set(Register::Rdi, a); machine.Set(Register::Rsi, b); machine.Set(Register::Rdx, c);
        machine.Set(Register::Rsp, 0x4ff0); machine.Set(Register::Rbx, 0xcafef00d12345678);
        require(machine.Run(0x1000, 0x100d, 100) == Cpu::StopReason::Address, "AJM did not return to guest");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0xcafef00d12345678,
                "AJM corrupted the guest stack/callee-saved register");
        return static_cast<std::uint32_t>(machine.Get(Register::Rax));
    }
    std::uint32_t read(std::uint64_t address) {
        std::uint32_t value; machine.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
};
// Contract: exact identity/type admission. Regression: matching only NID admits a foreign ABI.
// Existing AudioOut resolver only covers its own library. No test-only production seam is added.
void scopes() {
    Session s;
    auto valid = identity(0);
    require(s.imports->Resolve(valid, 2) == s.gates[0], "repeat resolve changed gate");
    for (int change = 0; change < 6; ++change) {
        auto candidate = valid;
        if (change == 0) candidate.LibraryName = "libSceAudioOut2";
        if (change == 1) candidate.ModuleName = "foreign";
        if (change == 2) candidate.LibraryVersion = 2;
        if (change == 3) candidate.ModuleMajor = 2;
        if (change == 4) candidate.ModuleMinor = 0;
        if (change == 5) candidate.Nid = "unimplemented";
        require(!s.imports->Resolve(candidate, 2), "foreign AJM identity admitted");
    }
    require(!s.imports->Resolve(valid, 1) && !s.imports->Resolve(valid, 0), "AJM object/untyped symbol admitted");
    require(!s.imports->Resolve(identity(0), 3), "AJM unsupported symbol type admitted");
}
// Contract: invalid output storage never creates an AJM context or modifies adjacent storage.
// Regression: host casts, missing span check or state insertion before validation leaks handles.
// This differs from the compiled lifecycle oracle by testing mapping permissions and provider lifetime.
void pointersAndLifetime() {
    Session s;
    require(s.call(0, 0, 0) == 0x80930005u, "null output error");
    require(s.call(0, 0x100000000ULL, 0x2000) == 0x80930005u, "64-bit reserved field truncated");
    require(s.call(0, 0, 0x2000) == 0, "initial context creation");
    const auto first = s.read(0x2000);
    s.machine.Protect(0x3000, 4096, Permission::Read);
    for (const auto address : std::array<std::uint64_t, 4>{0x3000, 0x2ffe, 0xdead0000, 0xfffffffffffffffe}) {
        bool rejected = false;
        try { (void)s.call(0, 0, address); }
        catch (const std::exception& error) {
            const auto expected = address == 0xfffffffffffffffe ? "context output span overflows" : "Guest access denied";
            require(std::string(error.what()).find(expected) != std::string::npos, "pointer rejected for an unrelated reason");
            rejected = true;
        }
        require(rejected, "AJM accepted unreadable/overflowing output span");
    }
    require(s.call(0, 0, 0x2004) == 0 && s.read(0x2004) == first + 1, "bad output consumed context identity");
    const auto second = s.read(0x2004);
    require(s.call(1, first) == 0 && s.call(1, first) == 0x80930002u, "stale context accepted");
    require(s.call(2, first, 0, 0) == 0x80930002u, "released context still owns codec registration");
    require(s.call(2, second, 0, 0) == 0x80930008u, "codec registration invented decoder support");
    require(s.call(2, 0, 0, 1) == 0x80930005u, "reserved validation did not precede invalid context");
    require(s.call(1, 0xfeedface00000000ULL | second) == 0, "uint32 context argument or cleanup failed");
    s.imports.reset();
    bool expired = false;
    try { s.machine.CheckAccess(s.gates[1], 1, Permission::Execute); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find("Guest access denied") != std::string::npos,
                "expired provider rejected for an unrelated reason");
        expired = true;
    }
    require(expired, "expired provider gate page retained executable storage");
    s.imports = std::make_unique<Cpu::Platform::AudioServices>(s.machine);
    for (std::size_t i = 0; i < s.gates.size(); ++i) s.gates[i] = s.imports->Resolve(identity(i), 2).value();
    require(s.call(0, 0, 0x2008) == 0, "same-base provider replacement failed");
    const auto replacement = s.read(0x2008);
    require(replacement != first && replacement != second, "replacement reused a stale context token");
    require(s.call(1, second) == 0x80930002u && s.call(1, replacement) == 0,
            "replacement gate retained old state or callback");
}
// Contract: tokens belong to exactly one provider, including two providers in one Machine.
// Regression: per-provider counter1 aliases both owners and allows B to destroy A's numeric handle.
// Existing lifecycle tests use only one provider. This adds no test-only interface or second Machine.
void providerOwnership() {
    Session s;
    require(s.call(0, 0, 0x2000) == 0, "provider A initialization failed");
    const auto a = s.read(0x2000);
    Cpu::Platform::AudioServices other(s.machine, 0x7ffdc6010000);
    std::array<std::uint64_t, 3> otherGates;
    for (std::size_t i = 0; i < otherGates.size(); ++i) otherGates[i] = other.Resolve(identity(i), 2).value();
    const auto originalGates = s.gates;
    s.gates = otherGates;
    require(s.call(0, 0, 0x2004) == 0, "provider B initialization failed");
    const auto b = s.read(0x2004);
    require(a != b, "providers produced colliding context tokens");
    require(s.call(1, a) == 0x80930002u, "provider B accepted foreign context token");
    require(s.call(2, b, 0, 0) == 0x80930008u, "foreign teardown destroyed provider B context");
    s.gates = originalGates;
    require(s.call(1, b) == 0x80930002u && s.call(1, a) == 0, "provider A ownership/cleanup failed");
    s.gates = otherGates;
    require(s.call(1, b) == 0, "provider B cleanup failed");
}
void compiledGuest(const char* path) {
    Session s;
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "guest fixture not readable");
    const std::vector<char> chars{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(!chars.empty() && chars.size() < 0x10000, "invalid guest fixture size");
    s.machine.Map(0x10000, 0x10000, rw); s.machine.Write(0x10000, std::as_bytes(std::span(chars)));
    s.machine.Protect(0x10000, 0x10000, rx);
    s.machine.Map(0x40000, 4096, rw); s.machine.Map(0x80000, 4096, rw);
    s.machine.Map(0x90000, 4096, rx);
    s.machine.Write(0x2000, std::as_bytes(std::span(s.gates)));
    const std::uint64_t continuation = 0x90000;
    s.machine.Write(0x80ff8, std::as_bytes(std::span(&continuation, 1)));
    s.machine.Set(Register::Rsp, 0x80ff8); s.machine.Set(Register::Rdi, 0x2000); s.machine.Set(Register::Rsi, 0x40000);
    require(s.machine.Run(0x10000, continuation, 100000) == Cpu::StopReason::Address, "compiled AJM guest did not return");
    require(s.machine.Get(Register::Rax) == 0 && s.read(0x40014) == 0x50415353u, "compiled AJM lifecycle oracle failed");
    require(s.read(0x40000) == 0xa7a7a7a7u && s.read(0x40008) == 0x5e5e5e5eu, "AJM output width corrupted sentinel");
}
}
int main(int argc, char** argv) {
    try {
        scopes(); pointersAndLifetime(); providerOwnership();
        if (argc == 2) compiledGuest(argv[1]);
        else require(argc == 1, "usage: AudioServicesTest [flat .text guest]");
        std::cout << "PASS AJM context pointer/lifetime/error controls" << (argc == 2 ? " and compiled x86 guest" : "") << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
