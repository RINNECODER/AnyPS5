#include <cpu/SceUserImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstddef>
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
constexpr std::uint64_t notInitialized = 0xffffffff80960002ULL;
constexpr std::uint64_t alreadyInitialized = 0xffffffff80960003ULL;
constexpr std::uint64_t invalidArgument = 0xffffffff80960005ULL;
constexpr std::uint64_t shortBuffer = 0xffffffff8096000aULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing user-service rejection: ") + expected);
}

Cpu::SceImport qualified(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libSceUserService";
    value.ModuleName = "libSceUserService";
    value.LibraryId = 7;
    value.ModuleId = 11;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceUserImports> imports = std::make_unique<Cpu::SceUserImports>(machine);

    Session() {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x6000, 4096, rw);
        // CALL [RIP+0xffa]; MOV [RIP+0x1003],RAX; INC RBX; NOP.
        constexpr std::array<std::uint8_t, 17> program{
            0xff, 0x15, 0xfa, 0x0f, 0, 0,
            0x48, 0x89, 0x05, 0x03, 0x10, 0, 0,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(program)));
    }

    std::uint64_t callGate(std::uint64_t gate, std::uint64_t first = 0,
                           std::uint64_t second = 0, std::uint64_t third = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        machine.Set(Register::Rdi, first);
        machine.Set(Register::Rsi, second);
        machine.Set(Register::Rdx, third);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        const std::array saved{Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (const auto reg : saved) machine.Set(reg, 0x9876543210123456);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address,
                "User-service gate failed to return to the actual x86 GOT caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "User-service gate corrupted CALL/RET stack or skipped guest continuation");
        for (const auto reg : saved)
            require(machine.Get(reg) == 0x9876543210123456, "User-service gate corrupted a callee-saved register");
        std::uint64_t stored = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Actual x86 caller did not store the gate result");
        return stored;
    }

    std::uint64_t call(const char* nid, std::uint64_t first = 0,
                       std::uint64_t second = 0, std::uint64_t third = 0) {
        return callGate(imports->Resolve(qualified(nid)).value(), first, second, third);
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
};

// Contract: initialization is per session, accepts the declared options, and failed reads cannot change state.
// Regression: global initialization or setting state before validating all four option bytes.
// libc/kernel tests have no user-service state; these invoke public NIDs through the real Machine boundary.
void initializationAndState() {
    {
        Session session;
        session.fill(0x3000, 32);
        require(session.call("CdWp0oHWGr0", 0x3000) == notInitialized, "Initial-user getter lost signed not-initialized error");
        require(session.call("fPhymKNvK-A", 0) == notInitialized, "Login-list getter checked null before initialization");
        require(session.call("1xxcMiGu2fo", 0, 0, 0) == notInitialized, "Name getter checked arguments before initialization");
        require(session.bytes(0x3000, 32) == std::vector<std::uint8_t>(32, 0xa7), "Uninitialized getter changed output");
        for (const auto pointer : std::array<std::uint64_t, 3>{0x9000, 0x6ffe, std::numeric_limits<std::uint64_t>::max() - 1}) {
            require(session.call("j3YMu1MVNNo", pointer) == invalidArgument, "Invalid initialization options were not rejected");
            require(session.call("CdWp0oHWGr0", 0x3000) == notInitialized, "Failed option read initialized the session");
        }
        session.machine.Protect(0x3000, 4096, Permission::Write);
        require(session.call("j3YMu1MVNNo", 0x3000) == invalidArgument, "Initialization read ignored guest read permission");
        session.machine.Protect(0x3000, 4096, rw);
        require(session.call("j3YMu1MVNNo", 0) == 0, "Null initialization options failed");
        require(session.call("j3YMu1MVNNo", 0) == alreadyInitialized, "Repeated initialization lost signed error");
    }
    {
        Session independent;
        require(independent.call("CdWp0oHWGr0", 0x3000) == notInitialized, "Initialization leaked into another Machine session");
    }
    for (const std::array<std::uint8_t, 4> options : {
             std::array<std::uint8_t, 4>{0x00, 0x01, 0, 0}, {0xff, 0x02, 0, 0}}) {
        Session accepted;
        accepted.machine.Write(0x3000, std::as_bytes(std::span(options)));
        require(accepted.call("j3YMu1MVNNo", 0x3000) == 0, "Declared initialization priority endpoint was rejected");
        require(accepted.call("j3YMu1MVNNo", 0x3000) == alreadyInitialized, "Valid options bypassed repeated initialization error");
    }
    for (const std::array<std::uint8_t, 4> options : {
             std::array<std::uint8_t, 4>{0xff, 0, 0, 0}, {0x00, 0x03, 0, 0}}) {
        Session rejected;
        rejected.machine.Write(0x3000, std::as_bytes(std::span(options)));
        rejects([&] { rejected.call("j3YMu1MVNNo", 0x3000); }, "Unsupported SCE user service initialization priority");
        require(rejected.call("CdWp0oHWGr0", 0x3000) == notInitialized, "Unsupported priority changed initialization state");
    }
}

// Contract: ID/list bytes are little-endian and each complete output span is checked before any write.
// Regression: native struct layout, truncated lists, or writing the valid prefix before a later-page fault.
// Existing memory tests cover libc lengths rather than these fixed user-service ABI outputs.
void userOutputs() {
    for (const auto [nid, size] : std::array<std::pair<const char*, std::size_t>, 2>{{
             {"CdWp0oHWGr0", 4}, {"fPhymKNvK-A", 16}}}) {
        Session session;
        require(session.call("j3YMu1MVNNo") == 0, "Output session initialization failed");
        session.fill(0x3000, 32);
        require(session.call(nid, 0x3003) == 0, "Unaligned writable user output was rejected");
        std::vector<std::uint8_t> expected(size, 0xff);
        expected[0] = 0; expected[1] = 0; expected[2] = 0; expected[3] = 0x10;
        require(session.bytes(0x3003, size) == expected, "User output ID/list bytes differ from the independent ABI oracle");
        require(session.bytes(0x3002, 1) == std::vector<std::uint8_t>{0xa7} &&
                session.bytes(0x3003 + size, 1) == std::vector<std::uint8_t>{0xa7}, "User output wrote outside its declared span");
        const std::uint64_t boundary = 0x7000 - size / 2;
        session.fill(boundary, size / 2);
        require(session.call(nid, boundary) == invalidArgument, "Output crossed into an unmapped page");
        require(session.bytes(boundary, size / 2) == std::vector<std::uint8_t>(size / 2, 0xa7),
                "Unmapped output changed its valid prefix before rejecting");
        session.machine.Map(0x7000, 4096, Permission::Read);
        session.fill(boundary, size);
        for (const auto destination : {std::uint64_t(0), std::uint64_t(0x7000), std::uint64_t(0x9000),
                                       std::numeric_limits<std::uint64_t>::max() - 1, boundary}) {
            require(session.call(nid, destination) == invalidArgument, "Invalid output span did not return signed invalid-argument error");
            require(session.bytes(boundary, size) == std::vector<std::uint8_t>(size, 0xa7), "Rejected output partially changed guest memory");
        }
        session.machine.Protect(0x7000, 4096, rw);
        require(session.call(nid, boundary) == 0 && session.bytes(boundary, size) == expected,
                "Output rejected a fully writable span crossing adjacent guest mappings");
    }
}

// Contract: Player requires capacity >=17, zero-fills the entire declared capacity, and leaves failures untouched.
// Regression: checking only the six name bytes, skipping zero padding, or changing argument-error precedence.
// Fixed ID/list output coverage cannot protect a caller-supplied name capacity or its distinct policy limits.
void names() {
    Session session;
    require(session.call("j3YMu1MVNNo") == 0, "Name session initialization failed");
    for (const auto capacity : {17u, 32u}) {
        session.fill(0x3000, 64);
        require(session.call("1xxcMiGu2fo", 0x10000000, 0x3001, capacity) == 0, "Supported username capacity failed");
        std::vector<std::uint8_t> expected(capacity, 0);
        constexpr std::array<std::uint8_t, 6> player{'P', 'l', 'a', 'y', 'e', 'r'};
        for (std::size_t index = 0; index < player.size(); ++index) expected[index] = player[index];
        require(session.bytes(0x3001, capacity) == expected, "Username was not Player with full-capacity zero padding");
        require(session.bytes(0x3000, 1) == std::vector<std::uint8_t>{0xa7} &&
                session.bytes(0x3001 + capacity, 1) == std::vector<std::uint8_t>{0xa7}, "Username changed adjacent bytes");
    }
    session.fill(0x3000, 64);
    for (const auto capacity : {0u, 6u, 16u})
        require(session.call("1xxcMiGu2fo", 0x10000000, 0x3000, capacity) == shortBuffer, "Username accepted capacity below 17");
    require(session.call("1xxcMiGu2fo", 0xffffffff, 0x3000, 16) == invalidArgument, "Wrong-user validation did not precede short capacity");
    require(session.call("1xxcMiGu2fo", 0x10000000, 0, 16) == invalidArgument, "Null-name validation did not precede short capacity");
    session.machine.Protect(0x3000, 4096, Permission::Read);
    require(session.call("1xxcMiGu2fo", 0x10000000, 0x3000, 16) == shortBuffer, "Username checked write permission before short capacity");
    require(session.call("1xxcMiGu2fo", 0x10000000, 0x3000, 17) == invalidArgument, "Username ignored read-only output");
    require(session.bytes(0x3000, 64) == std::vector<std::uint8_t>(64, 0xa7), "Failed username validation changed its buffer");
    session.fill(0x6ff9, 7);
    require(session.call("1xxcMiGu2fo", 0x10000000, 0x6ff9, 17) == invalidArgument, "Username checked only written name bytes instead of full capacity");
    require(session.bytes(0x6ff9, 7) == std::vector<std::uint8_t>(7, 0xa7), "Username partially wrote before an unmapped capacity suffix");
    for (const auto pointer : {std::uint64_t(0x9000), std::numeric_limits<std::uint64_t>::max() - 1})
        require(session.call("1xxcMiGu2fo", 0x10000000, pointer, 17) == invalidArgument, "Username accepted unmapped or overflowing span");
    rejects([&] { session.call("1xxcMiGu2fo", 0x10000000, 0x3000, 16 * 1024 * 1024 + 1); }, "16 MiB");
    require(session.bytes(0x3000, 64) == std::vector<std::uint8_t>(64, 0xa7), "Unsupported username capacity changed its buffer");
}

// Contract: complete import identity owns an executable nonwritable gate; expired session callbacks fault safely.
// Regression: NID-only caches, accepting wrong scope/version, or callbacks retaining dangling user-session pointers.
// Other service resolvers cannot protect this separately owned gate page or weak user-service state lifetime.
void scopeAndLifetime() {
    {
        Session session;
        const auto original = qualified("j3YMu1MVNNo");
        const auto gate = session.imports->Resolve(original).value();
        require(gate > 0 && gate < 0x7ffffffff000ULL, "User-service gate is not a low canonical guest address");
        require(session.imports->Resolve(original).value() == gate, "Repeated qualified user import changed gate address");
        session.machine.CheckAccess(gate, 1, Permission::Execute);
        rejects([&] { session.machine.CheckAccess(gate, 1, Permission::Write); }, "permission");
        auto other = original;
        other.LibraryId = 12; other.ModuleId = 29;
        const auto otherGate = session.imports->Resolve(other).value();
        require(otherGate != gate && session.callGate(otherGate) == 0, "Importer-local IDs were discarded or prevented qualified binding");
        require(session.callGate(gate) == alreadyInitialized, "Distinct importer gates did not share their owning session state");
        for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
            auto wrong = original;
            switch (mismatch) {
            case 0: wrong.LibraryName = "libkernel"; break;
            case 1: wrong.ModuleName = "unrelated"; break;
            case 2: wrong.LibraryVersion = 2; break;
            case 3: wrong.ModuleMajor = 2; break;
            case 4: wrong.ModuleMinor = 2; break;
            }
            rejects([&] { session.imports->Resolve(wrong); }, "scope/version");
        }
        auto unrelated = original;
        unrelated.LibraryName = "libc"; unrelated.ModuleName = "libc";
        require(!session.imports->Resolve(unrelated), "User resolver captured an unrelated module/library scope");
        auto unknown = original; unknown.Nid = "AAAAAAAAAAA";
        rejects([&] { session.imports->Resolve(unknown); }, "Unsupported SCE user import service");
        session.imports.reset();
        rejects([&] { session.callGate(gate); }, "runtime has expired");
    }
    for (const auto base : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(0x7ffffffff000), std::uint64_t(0x800000000000)}) {
        Cpu::Machine machine;
        rejects([&] { Cpu::SceUserImports invalid(machine, base); }, "aligned low canonical guest page");
    }
}
}

int main() {
    try {
        initializationAndState();
        userOutputs();
        names();
        scopeAndLifetime();
        std::cout << "PASS native x86 user-service GOT gates, session state, signed errors, checked ABI outputs and lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
