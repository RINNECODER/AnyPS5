#include <cpu/SceCommonDialogImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t gateBase = 0x7ffdf2000000;
constexpr std::uint64_t alreadyInitialized = 0xffffffff80b80002ULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing common-dialog rejection: ") + expected);
}

Cpu::SceImport qualified(const char* nid = "uoUpLGNkygk") {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libSceCommonDialog";
    value.ModuleName = "libSceCommonDialog";
    value.LibraryId = 20;
    value.ModuleId = 27;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceCommonDialogImports> imports =
        std::make_unique<Cpu::SceCommonDialogImports>(machine, gateBase);

    Session() {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        // CALL [RIP+0xffa]; MOV [RIP+0x1003],RAX; INC RBX; NOP.
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0,
            0x48, 0x89, 0x05, 0x03, 0x10, 0, 0,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }

    std::uint64_t callGate(std::uint64_t gate) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        // This no-argument ABI must not interpret leftover registers as pointers.
        machine.Set(Register::Rdi, 0x9000);
        machine.Set(Register::Rsi, gateBase + 0x80);
        machine.Set(Register::Rdx, 0xffffffffffffffffULL);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        constexpr std::array saved{Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (const auto reg : saved) machine.Set(reg, 0x9876543210123456);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address,
                "Common-dialog gate did not return to the translated x86 GOT caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "Common-dialog gate corrupted CALL/RET stack or skipped the guest continuation");
        for (const auto reg : saved)
            require(machine.Get(reg) == 0x9876543210123456, "Common-dialog gate corrupted a callee-saved register");
        std::uint64_t stored = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Guest did not store the actual common-dialog RAX result");
        return stored;
    }

    std::vector<std::byte> gateBytes() {
        std::vector<std::byte> result(4096);
        machine.Read(gateBase, result);
        return result;
    }
};

// Contract: Initialize returns 0 once, then signed int32 0x80b80002; importer gates share their provider state.
// Regression: always-success or zero-extended errors, state copied into each gate, or process-global initialization.
// System/User/NP keepers do not cover CommonDialog. Public Resolve plus actual x86 CALL/RET needs no new seam.
void initializationAndReset() {
    for (unsigned freshSession = 0; freshSession < 2; ++freshSession) {
        Session session;
        const auto gate = session.imports->Resolve(qualified()).value();
        auto other = qualified();
        other.LibraryId = 42; other.ModuleId = 43;
        const auto otherGate = session.imports->Resolve(other).value();
        require(otherGate != gate, "Common-dialog resolver discarded importer-local IDs");
        require(session.imports->Resolve(other).value() == otherGate,
                "Repeated importer-local common-dialog identity changed its gate");
        session.machine.CheckAccess(gateBase, 4096, Permission::Execute);
        rejects([&] { session.machine.CheckAccess(gateBase, 4096, Permission::Write); }, "permission");
        const auto before = session.gateBytes();
        require(session.callGate(gate) == 0, "Fresh common-dialog provider did not initialize with zero");
        for (const auto repeated : std::array<std::uint64_t, 3>{gate, otherGate, gate})
            require(session.callGate(repeated) == alreadyInitialized,
                    "Repeated common-dialog initialization lost its shared state or full signed error");
        require(session.gateBytes() == before, "Common-dialog initialization changed its reserved RX gate page");
    }
}

// Contract: only the qualified Initialize import binds; invalid APIs remain unresolved and expired gates fault safely.
// Regression: NID-only binding, fabricated dialog service success, or retaining a dead provider through its callback.
// Other resolvers/lifetimes are separate owners; SceImport has no symbol-type field, so this asserts no type contract.
void qualificationAndLifetime() {
    Session session;
    const auto original = qualified();
    const auto gate = session.imports->Resolve(original).value();
    require(session.imports->Resolve(original).value() == gate, "Qualified common-dialog gate cache changed its address");
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
    auto unrelated = original;
    unrelated.LibraryName = "libc"; unrelated.ModuleName = "libc";
    require(!session.imports->Resolve(unrelated), "Common-dialog resolver captured an unrelated scope");
    rejects([&] { session.imports->Resolve(qualified("AAAAAAAAAAA")); }, "Unsupported SCE common dialog import service");
    require(session.callGate(gate) == 0, "Resolving rejected imports changed common-dialog initialization state");
    const auto before = session.gateBytes();
    session.imports.reset();
    rejects([&] { session.callGate(gate); }, "SCE common dialog import runtime has expired");
    require(session.gateBytes() == before, "Expired common-dialog provider changed its gate page");
}
}

int main() {
    try {
        initializationAndReset();
        qualificationAndLifetime();
        std::cout << "PASS translated x86 CommonDialog Initialize gates, signed repeat errors and session-owned state\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
