#include "../NpServices.hpp"
#include <cpu/SceElf.hpp>
#include <cpu/SceNpLocalImports.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& e) {
        require(std::string(e.what()).find(expected) != std::string::npos, e.what());
        return;
    }
    throw std::runtime_error(std::string("Missing rejection: ") + expected);
}
Cpu::SceImport qualified() {
    Cpu::SceImport result;
    result.Nid = "XDncXQIJUSk";
    result.LibraryName = result.ModuleName = "libSceNpManager";
    result.LibraryVersion = result.ModuleMajor = result.ModuleMinor = 1;
    result.LibraryId = 7; result.ModuleId = 8;
    return result;
}
std::vector<std::byte> read(Cpu::Machine& machine, std::uint64_t address, std::size_t count) {
    std::vector<std::byte> result(count);
    machine.Read(address, result);
    return result;
}

// Contract owner: compiled x86 caller observes signed-out or invalid-argument,
// untouched identity output, CALL/RET continuation, and provider page cleanup.
// Regression: resolving the wrong scope, inventing an identity/success, writing
// an offline buffer, returning the wrong signed error, or leaking an owned gate.
// Existing SceNpLocalImportsTest rejects this NID and cannot execute this branch.
// No production test seam: Resolve and the real Machine guest execution API only.
void exercise(const char* path, bool baseline) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "Cannot read compiled x86 guest flat text");
    const std::vector<char> source{std::istreambuf_iterator<char>(stream), {}};
    require(!source.empty() && source.size() < 2048, "Invalid guest flat text size");
    Cpu::Machine machine;
    machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
    machine.Write(0x1000, std::as_bytes(std::span(source)));
    machine.Map(0x3000, 4096, rw);
    machine.Map(0x4000, 4096, rw);
    machine.Map(0x6000, 4096, rw);
    const std::vector<std::byte> sentinel(64, std::byte{0xa7});
    machine.Write(0x6000, sentinel);
    if (baseline) {
        Cpu::SceNpLocalImports previous(machine);
        // Must fail at missing old service, before any candidate implementation.
        (void)previous.Resolve(qualified()).value();
        throw std::runtime_error("Baseline unexpectedly resolved missing NP query");
    }
    auto services = std::make_unique<Cpu::Platform::NpServices>(machine);
    const auto original = qualified();
    const auto gate = services->Resolve(original, 2).value();
    auto invoke = [&](std::uint64_t address, std::uint32_t user, std::uint64_t pointer) {
        constexpr std::uint64_t continuation = 0x1800;
        machine.Write(0x4fe8, std::as_bytes(std::span(&continuation, 1)));
        machine.Set(Register::Rsp, 0x4fe8);
        machine.Set(Register::Rdi, address);
        machine.Set(Register::Rsi, user);
        machine.Set(Register::Rdx, pointer);
        machine.Set(Register::Rcx, 0x3000);
        constexpr std::array saved{Register::Rbx, Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (auto reg : saved) machine.Set(reg, 0x1122334455667788);
        require(machine.Run(0x1000, continuation, 1000) == Cpu::StopReason::Address,
                "Guest NP call failed to return to continuation");
        require(machine.Get(Register::Rsp) == 0x4ff0, "Guest NP call corrupted stack");
        for (auto reg : saved) require(machine.Get(reg) == 0x1122334455667788, "Guest NP call corrupted preserved register");
        std::array<std::uint64_t, 2> report{};
        machine.Read(0x3000, std::as_writable_bytes(std::span(report)));
        require(report[1] == 0x4e504f46464c494eULL, "Guest continuation did not store completion marker");
        require(read(machine, 0x6000, sentinel.size()) == sentinel, "Offline query fabricated or changed identity bytes");
        return report[0];
    };
    require(invoke(gate, 0x10000000, 0x6001) == 0xffffffff80550006ULL,
            "Qualified offline user did not receive signed-out error");
    // Offline branch has no identity write and no layout claim. A read-only or
    // non-null unmapped destination must never be dereferenced on this branch.
    machine.Protect(0x6000, 4096, Permission::Read);
    for (auto pointer : std::array<std::uint64_t, 3>{0x6001, 0x9000, std::numeric_limits<std::uint64_t>::max()})
        require(invoke(gate, 0x10000000, pointer) == 0xffffffff80550006ULL,
                "Offline query dereferenced a non-null destination or invented pointer validation");
    for (const auto item : std::array<std::pair<std::uint32_t, std::uint64_t>, 3>{
            std::pair{0x10000000u, 0ULL}, std::pair{0xffffffffu, 0x6001ULL}, std::pair{0x10000001u, 0x6001ULL}})
        require(invoke(gate, item.first, item.second) == 0xffffffff80550003ULL,
                "Null output or unowned user did not receive invalid argument");
    require(services->Resolve(original, 2).value() == gate, "Repeated qualified import changed gate");
    auto alias = original; alias.LibraryId = 30; alias.ModuleId = 31;
    const auto aliasGate = services->Resolve(alias, 2).value();
    require(aliasGate != gate && invoke(aliasGate, 0x10000000, 0x6001) == 0xffffffff80550006ULL,
            "Importer-local IDs were collapsed or changed offline policy");
    machine.CheckAccess(gate, 1, Permission::Execute);
    rejects([&] { machine.CheckAccess(gate, 1, Permission::Write); }, "permission");
    for (unsigned mismatch = 0; mismatch < 6; ++mismatch) {
        auto wrong = original; unsigned char type = 2;
        switch (mismatch) {
        case 0: wrong.LibraryName = "libc"; break;
        case 1: wrong.ModuleName = "libc"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 0; break;
        case 5: type = 1; break;
        }
        rejects([&] { services->Resolve(wrong, type); }, "scope/version/type");
    }
    auto other = original; other.Nid = "eQH7nWPcAgc";
    require(!services->Resolve(other, 2), "Component captured existing GetState service");
    other.LibraryName = other.ModuleName = "libSceNpAuth";
    require(!services->Resolve(other, 2), "Component claimed unsupported NP Auth");
    services.reset();
    rejects([&] { machine.CheckAccess(gate, 1, Permission::Execute); }, "access");
    // Reusing the released owned page must bind the replacement session, never
    // a retained old provider callback or old user's state.
    auto replacement = std::make_unique<Cpu::Platform::NpServices>(machine, 0x10000042);
    const auto replacementGate = replacement->Resolve(original, 2).value();
    require(invoke(replacementGate, 0x10000000, 0x6001) == 0xffffffff80550003ULL &&
            invoke(replacementGate, 0x10000042, 0x6001) == 0xffffffff80550006ULL,
            "Replacement NP provider retained old session ownership");
    replacement.reset();
    rejects([&] { machine.CheckAccess(gate, 1, Permission::Execute); }, "access");
}
}
int main(int argc, char** argv) {
    try {
        require(argc >= 2, "Usage: NpServicesTest <flat-x86-text> [--baseline]");
        exercise(argv[1], argc >= 3 && std::string(argv[2]) == "--baseline");
        std::cout << "PASS compiled x86 offline NP query, untouched output, exact scope/type and owned gate cleanup\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
