#include <cpu/SceNpLocalImports.hpp>
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
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t localUser = 0x10000000;
constexpr std::uint64_t invalidArgument = 0xffffffff80550003ULL;
constexpr const char* getStateNid = "eQH7nWPcAgc";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing local NP rejection: ") + expected);
}

Cpu::SceImport qualified(const char* nid = getStateNid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libSceNpManager";
    value.ModuleName = "libSceNpManager";
    value.LibraryId = 7;
    value.ModuleId = 8;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceNpLocalImports> imports;

    explicit Session(std::uint32_t user = localUser) :
        imports(std::make_unique<Cpu::SceNpLocalImports>(machine, user)) {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x6000, 4096, rw);
        // CALL [RIP+0xffa]; MOV [RIP+0x1003],RAX; INC RBX; NOP.
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0,
            0x48, 0x89, 0x05, 0x03, 0x10, 0, 0,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }

    std::uint64_t gate() { return imports->Resolve(qualified()).value(); }

    std::uint64_t callGate(std::uint64_t address, std::uint64_t user, std::uint64_t output) {
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, user);
        machine.Set(Register::Rsi, output);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        constexpr std::array saved{Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (const auto reg : saved) machine.Set(reg, 0x9876543210123456);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address,
                "Local NP gate did not return to the translated x86 GOT caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "Local NP gate corrupted CALL/RET stack or skipped the guest continuation");
        for (const auto reg : saved)
            require(machine.Get(reg) == 0x9876543210123456, "Local NP gate corrupted a callee-saved register");
        std::uint64_t stored = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Guest did not store the actual local NP RAX result");
        return stored;
    }

    std::uint64_t call(std::uint64_t user, std::uint64_t output) {
        return callGate(gate(), user, output);
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

// Contract: the configured local user gets exactly four LE state bytes (1), never online/account success.
// Regression: wrong output width/state, wrong argument register, global user policy, or 64-bit interpretation of EDI.
// Existing System/User keepers have no NP state query. This uses the public provider and actual x86 CALL/RET only.
void localStateAndUser() {
    for (const auto user : std::array<std::uint32_t, 2>{0x10000000, 0x10000017}) {
        Session session(user);
        for (const auto argument : std::array<std::uint64_t, 2>{user, 0xfeedbeef00000000ULL | user}) {
            session.fill(0x3000, 8);
            require(session.call(argument, 0x3001) == 0, "Configured local user state query failed");
            require(session.bytes(0x3000, 8) == std::vector<std::uint8_t>{0xa7, 1, 0, 0, 0, 0xa7, 0xa7, 0xa7},
                    "Local NP state differs from independent LE output bytes or changed adjacent bytes");
        }
        for (const auto wrong : std::array<std::uint64_t, 3>{0, 0xffffffff, std::uint64_t(user) + 1}) {
            session.fill(0x3000, 8);
            require(session.call(wrong, 0x3001) == invalidArgument,
                    "Unqualified user did not return the full signed invalid-argument error");
            require(session.bytes(0x3000, 8) == std::vector<std::uint8_t>(8, 0xa7),
                    "Rejected user changed NP output bytes");
        }
    }
}

// Contract: all four bytes are writable before any output write, including adjacent mappings and reserved RX gates.
// Regression: prefix write before a suffix fault, pointer overflow, or host Write bypassing guest permissions.
// Generic memory tests cannot protect this provider's preflight. No additional production seam is needed.
void outputAtomicity() {
    Session session;
    session.fill(0x6ffc, 4);
    require(session.call(localUser, 0x6ffe) == invalidArgument, "NP output crossed an unmapped suffix");
    require(session.bytes(0x6ffc, 4) == std::vector<std::uint8_t>(4, 0xa7),
            "NP output wrote its valid prefix before an unmapped suffix rejection");
    session.machine.Map(0x7000, 4096, rw);
    session.fill(0x6ffc, 8);
    session.machine.Protect(0x7000, 4096, Permission::Read);
    const auto gate = session.gate();
    const auto reservedBefore = session.bytes(gate + 0x80, 8);
    for (const auto output : std::array<std::uint64_t, 6>{
             0, 0x7000, 0x9000, std::numeric_limits<std::uint64_t>::max() - 1, 0x6ffe, gate + 0x80}) {
        require(session.call(localUser, output) == invalidArgument, "NP output accepted an invalid or nonwritable span");
        require(session.bytes(0x6ffc, 8) == std::vector<std::uint8_t>(8, 0xa7),
                "Rejected NP output partially changed a mapped prefix or suffix");
        require(session.bytes(gate + 0x80, 8) == reservedBefore, "NP output changed its reserved executable gate page");
    }
    session.machine.Protect(0x7000, 4096, rw);
    require(session.call(localUser, 0x6ffe) == 0, "NP output rejected adjacent writable mappings");
    require(session.bytes(0x6ffc, 8) == std::vector<std::uint8_t>{0xa7, 0xa7, 1, 0, 0, 0, 0xa7, 0xa7},
            "Cross-mapping NP output changed neighboring bytes or serialized the wrong state");
}

// Contract: qualified import identity owns RX gates; unknown APIs stay unresolved; dead providers cannot write output.
// Regression: NID-only binding/cache, fabricated callback success, or a retained dangling provider callback.
// Other providers cannot protect this NP resolver/lifetime; SceImport exposes no symbol-type field to test here.
void scopeAndLifetime() {
    Session session;
    const auto original = qualified();
    const auto gate = session.gate();
    require(session.imports->Resolve(original).value() == gate, "Repeated qualified NP import changed its gate");
    session.machine.CheckAccess(gate, 1, Permission::Execute);
    rejects([&] { session.machine.CheckAccess(gate, 1, Permission::Write); }, "permission");
    auto other = original;
    other.LibraryId = 42; other.ModuleId = 43;
    const auto otherGate = session.imports->Resolve(other).value();
    session.fill(0x3000, 8);
    require(otherGate != gate && session.callGate(otherGate, localUser, 0x3001) == 0,
            "NP resolver discarded importer-local IDs or refused their qualified state query");
    require(session.bytes(0x3000, 8) == std::vector<std::uint8_t>{0xa7, 1, 0, 0, 0, 0xa7, 0xa7, 0xa7},
            "Alternate importer-local NP gate changed output semantics");
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
    require(!session.imports->Resolve(unrelated), "NP resolver captured unrelated module/library scope");
    // Exact title NP imports remain unsupported: CheckCallback, RegisterStateCallbackA,
    // GetOnlineId, and RegisterNpReachabilityStateCallback, plus an unknown NID.
    for (const auto nid : {"3Zl8BePTh9Y", "qQJfO8HAiaY", "XDncXQIJUSk", "hw5KNqAAels", "AAAAAAAAAAA"})
        rejects([&] { session.imports->Resolve(qualified(nid)); }, "Unsupported SCE local NP import service");
    session.fill(0x3000, 8);
    session.imports.reset();
    rejects([&] { session.callGate(gate, localUser, 0x3001); }, "runtime has expired");
    require(session.bytes(0x3000, 8) == std::vector<std::uint8_t>(8, 0xa7), "Expired NP provider changed output");
}
}

int main() {
    try {
        localStateAndUser();
        outputAtomicity();
        scopeAndLifetime();
        std::cout << "PASS translated x86 local NP gates, four-byte offline policy, user qualification and atomic outputs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
