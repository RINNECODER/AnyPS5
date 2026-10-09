#include <cpu/SceImportStubs.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Cpu::SceUnresolvedImport unresolved(std::string nid, std::uint8_t type = 2, bool weak = false, std::uint64_t size = 0) {
    Cpu::SceImport import;
    import.Nid = std::move(nid);
    import.LibraryName = import.ModuleName = "libSceAnyPS5Absent";
    import.LibraryVersion = import.ModuleMajor = import.ModuleMinor = 1;
    return {{"eboot.bin"}, import, type, size, weak, "no provider declares it"};
}

// CALL [RIP+0xffa] (the GOT slot at 0x2000), then stop at the following NOP.
struct Caller {
    Cpu::Machine machine;
    Caller() {
        machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
        machine.Map(0x2000, 4096, Permission::Read | Permission::Write);
        machine.Map(0x4000, 4096, Permission::Read | Permission::Write);
        constexpr std::array<std::uint8_t, 7> code{0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(code)));
    }
    std::uint64_t call(std::uint64_t target) {
        machine.Write(0x2000, std::as_bytes(std::span(&target, 1)));
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rax, 0x1111);
        machine.Set(Register::Rbx, 0x2222);
        require(machine.Run(0x1000, 0x1006, 100) == Cpu::StopReason::Address, "trap stub did not return to its caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x2222, "trap stub broke the caller frame");
        return machine.Get(Register::Rax);
    }
};

void returnsConfiguredValueBeyondEngineGateBudget() {
    Caller caller;
    std::vector<Cpu::SceImportStubEvent> events;
    Cpu::SceImportStubs stubs(caller.machine, {0x80020002, [&](const auto& event) { events.push_back(event); }});
    // The modern TCG engine has 256 host gates in total; stubs must share one.
    std::vector<std::uint64_t> addresses;
    for (int index = 0; index < 600; ++index) addresses.push_back(stubs.Bind(unresolved("stub" + std::to_string(index))).Address);
    require(stubs.Bind(unresolved("stub7")).Address == addresses[7], "the same import bound a second trap stub");
    require(events.size() == 600, "each distinct unresolved import logs one binding");
    events.clear();
    for (const auto index : {599, 0, 599}) require(caller.call(addresses[index]) == 0x80020002, "stub did not return the configured value");
    require(events.size() == 2 && events[0].Import.Nid == "stub599" && events[1].Import.Nid == "stub0" &&
            events[0].Kind == Cpu::SceImportStubEventKind::Called && events[0].ReturnValue == 0x80020002,
            "each stub logs its first call once");
}

void abortsNamingModuleLibraryAndNid() {
    Caller caller;
    Cpu::SceImportStubs stubs(caller.machine, {});
    const auto stub = stubs.Bind(unresolved("Bagshr7OQ6Q")).Address;
    try {
        caller.call(stub);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        for (const auto* part : {"Unsupported SCE unresolved import called: sceNpTrophy2CreateContext", "nid=Bagshr7OQ6Q",
                                 "library=libSceAnyPS5Absent:1", "module=libSceAnyPS5Absent:1.1", "consumer=eboot.bin"})
            require(message.find(part) != std::string::npos, "abort diagnostic lacks " + std::string(part) + ": " + message);
        return;
    }
    throw std::runtime_error("calling an unresolved import did not stop the guest");
}

void bindsWeakToZeroAndObjectsToZeroedStorage() {
    Cpu::Machine machine;
    std::vector<Cpu::SceImportStubEvent> events;
    Cpu::SceImportStubs stubs(machine, {std::nullopt, [&](const auto& event) { events.push_back(event); }});
    const auto weak = stubs.Bind(unresolved("weak", 2, true));
    require(weak.Address == 0 && events.back().Binding == Cpu::SceImportStubBinding::WeakZero, "weak import is not zero");
    const auto object = stubs.Bind(unresolved("object", 1, false, 24));
    require(object.Address && object.Size >= 24 && events.back().Binding == Cpu::SceImportStubBinding::ZeroObject,
            "object import has no zeroed storage");
    std::array<std::byte, 24> bytes{};
    bytes.fill(std::byte{0xff});
    machine.Read(object.Address, bytes);
    for (const auto byte : bytes) require(byte == std::byte{0}, "object import storage is not zeroed");
    machine.CheckAccess(object.Address, 24, Permission::Write);
    require(stubs.Bind(unresolved("object", 1, false, 8)).Address == object.Address, "object import was bound twice");
    require(Cpu::SceNidName("eQH7nWPcAgc") == "sceNpGetState" && !Cpu::SceNidName("notAKnownNid"), "NID name table lookup");
}
}

int main() {
    try {
        returnsConfiguredValueBeyondEngineGateBudget();
        abortsNamingModuleLibraryAndNid();
        bindsWeakToZeroAndObjectsToZeroedStorage();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: 600 trap stubs share one host gate, abort diagnostics name the import, weak/object fallbacks\n";
    return 0;
}
