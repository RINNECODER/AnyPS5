#include "FlipFixtureSupport.hpp"
#include <iostream>

// Authoring gate: the real guest import boundary owns producer ABI admission,
// scalar narrowing and atomic descriptor/command publication. Existing native
// producer tests call a host CommandBuffer; existing CPU AGC tests never resolve
// these producers. A permissive admission, cursor overrun, or wrong word packing
// fails independent bytes/guards. No mock or production test seam is required.
namespace {
using namespace FlipFixture;
struct Snapshot {
    std::array<std::byte, 72> descriptor;
    std::array<std::byte, 128> commands;
    explicit Snapshot(Cpu::Machine& machine) {
        machine.Read(Builder - 8, descriptor); machine.Read(Commands, commands);
    }
    void unchanged(Cpu::Machine& machine) const {
        const Snapshot after(machine);
        require(after.descriptor == descriptor && after.commands == commands,
            "Rejected producer changed descriptor/command guards");
    }
};
void admission(Cpu::Machine& machine, Cpu::SceAgcImports& imports) {
    Cpu::SceAgcImports empty(machine, {}, std::span<const Cpu::AgcAbiAdmission>{}, 0x7ffdf3100000ULL);
    for (const char* nid : {FlipNid, WaitNid}) {
        rejects([&] { empty.Resolve(identity(nid), 2, 0); }, "Unqualified");
        rejects([&] { imports.Resolve(identity(nid), 1, 0); }, "only function");
        rejects([&] { imports.Resolve(identity(nid), 2, 8); }, "only function");
        auto wrong = identity(nid, true);
        rejects([&] { imports.Resolve(wrong, 2, 0); }, "scope/version");
        wrong = identity(nid); wrong.LibraryName = "foreign-library";
        rejects([&] { imports.Resolve(wrong, 2, 0); }, "scope/version");
        wrong = identity(nid); wrong.ModuleName = "foreign-module";
        rejects([&] { imports.Resolve(wrong, 2, 0); }, "scope/version");
        for (unsigned field = 0; field < 3; ++field) {
            wrong = identity(nid);
            if (field == 0) wrong.LibraryVersion = 2;
            if (field == 1) wrong.ModuleMajor = 2;
            if (field == 2) wrong.ModuleMinor = 2;
            rejects([&] { imports.Resolve(wrong, 2, 0); }, "scope/version");
        }
    }
    rejects([&] { imports.Resolve(identity("unknown-flip-service"), 2, 0); }, "Unsupported");
}
void run(const char* const* paths) {
    Cpu::Machine machine; Caller caller(machine, paths);
    Cpu::SceAgcImports imports(machine, {}, Cpu::TargetAgcAdmissions());
    admission(machine, imports);
    const auto flip = imports.Resolve(identity(FlipNid), 2, 0);
    const auto wait = imports.Resolve(identity(WaitNid), 2, 0);
    builder(machine);
    // Compiler narrowing is independent of the host register marshaller.
    const std::array<std::uint64_t, 8> input{Builder, 0x1234567800000011ULL,
        0x8765432100000002ULL, 0xabcdef1200000001ULL, 0, wait};
    require(caller.call(Caller::Sequence, flip, input) == Commands + 16,
        "Wait/flip producer returned wrong numeric guest packet address");
    // Source-pinned AnyPS5 internal bridge oracle, not literal retail PM4 bytes.
    const std::array<std::uint32_t, 10> expected{0xc0021018, 17, 2, 0,
        0xc004105c, 17, 2, 1, 0, 0};
    require(load<std::array<std::uint32_t, 10>>(machine, Commands) == expected &&
        load<std::uint64_t>(machine, Builder + 16) == Commands + 40,
        "Compiled wait/flip internal packet order or cursor differs");
    require(load<std::uint64_t>(machine, Builder - 8) == 0x7b7b7b7b7b7b7b7bULL &&
        load<std::uint64_t>(machine, Builder + 56) == 0x7b7b7b7b7b7b7b7bULL &&
        load<std::uint64_t>(machine, Builder + 32) == 0x9897969594939291ULL &&
        load<std::uint64_t>(machine, Builder + 40) == 0x8182838485868788ULL &&
        load<std::uint32_t>(machine, Builder + 48) == 2 &&
        load<std::uint32_t>(machine, Builder + 52) == 0x7b7b7b7b &&
        load<std::uint64_t>(machine, Commands + 40) == 0xd3d3d3d3d3d3d3d3ULL,
        "Producer changed adjacent descriptor, callbacks, reserved fields or packet guards");

    const auto atomicFailure = [&](Caller::Section section, std::array<std::uint64_t, 8> args,
                                   const char* reason) {
        const Snapshot before(machine);
        rejects([&] { caller.call(section, section == Caller::Wait ? wait : flip, args); }, reason);
        before.unchanged(machine);
    };
    for (const auto& invalid : std::array<std::array<std::uint64_t, 8>, 8>{{
        {Builder, 0, 0, 1, 0}, {Builder, 0x80000000, 0, 1, 0},
        {Builder, 17, 0xffffffff, 1, 0}, {Builder, 17, 3, 1, 0},
        {Builder, 17, 0, 0, 0}, {Builder, 17, 0, 2, 0},
        {Builder, 17, 0, 1, 1}, {Builder, 17, 0, 1, 0x100000000ULL}}}) {
        builder(machine); atomicFailure(Caller::Emit, invalid, "flip arguments outside qualified target use");
    }
    for (const auto& invalid : std::array<std::array<std::uint64_t, 8>, 4>{{
        {Builder, 0, 0}, {Builder, 0x80000000, 0}, {Builder, 17, 3}, {Builder, 17, 0xffffffff}}}) {
        builder(machine); atomicFailure(Caller::Wait, invalid, "rendering wait arguments outside qualified target use");
    }
    for (const auto section : {Caller::Emit, Caller::Wait}) {
        const std::array<std::uint64_t, 8> args{Builder, 17, 0, 1, 0};
        builder(machine, Commands, Commands + (section == Caller::Emit ? 20 : 12), 0);
        atomicFailure(section, args, "command buffer exhausted");
        builder(machine, Commands, Commands + (section == Caller::Emit ? 24 : 16), 1);
        atomicFailure(section, args, "command buffer exhausted");
        builder(machine); auto misaligned = args; misaligned[0] = Builder + 1;
        atomicFailure(section, misaligned, "misaligned");
        machine.Protect(Commands, 4096, Cpu::Permission::Read);
        atomicFailure(section, args, "permission");
        machine.Protect(Commands, 4096, RW);
        builder(machine); machine.Protect(Control, 4096, Cpu::Permission::Read);
        atomicFailure(section, args, "permission");
        machine.Protect(Control, 4096, RW);
        builder(machine);
        store(machine, Builder, Builder + 16); store(machine, Builder + 8, Builder + 256);
        store(machine, Builder + 16, Builder + 16); store(machine, Builder + 24, Builder + 256);
        atomicFailure(section, args, "overlaps command buffer descriptor");
    }

    // Separate public source contract: signed64/signed32 bit packing is broader
    // than the target mode1/argument0/registered-three-slot qualification.
    const std::array sourceContracts{Cpu::AgcAbiContract::DcbSetFlipCommandBuffer56};
    Cpu::SceAgcImports source(machine, {}, sourceContracts, 0x7ffdf3300000ULL);
    builder(machine);
    require(caller.call(Caller::Emit, source.Resolve(identity(FlipNid), 2, 0),
        {Builder, 0xfedcba98, 0xfffffffe, 0x12345678, 0xfedcba9876543211ULL}) == Commands,
        "Public source flip returned wrong guest packet pointer");
    const std::array<std::uint32_t, 6> sourceExpected{0xc004105c, 0xfedcba98, 0xfffffffe,
        0x12345678, 0x76543211, 0xfedcba98};
    require(load<std::array<std::uint32_t, 6>>(machine, Commands) == sourceExpected &&
        load<std::uint64_t>(machine, Builder + 16) == Commands + 24,
        "Public source flip lost full signed argument/index bits");
    std::cout << "PASS compiled x86 qualified wait/flip imported semantics -> literal internal bridge; "
        "admission/policy/permissions/capacity atomic guards; separate public signed-bit packing\n";
}
}
int main(int argc, const char* argv[]) {
    try {
        FlipFixture::require(argc == 5, "Flip contract fixture requires four compiled callers");
        run(argv + 1); return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
