#include "ContentServices.hpp"
#include <cpu/SceElf.hpp>
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
using Cpu::Platform::ContentServices;
using Cpu::Platform::ContentAbi;
using Cpu::Platform::InstalledContentRecord;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t parameterError = 0xffffffff80d90002ULL;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& action, const char* message) {
    try { action(); }
    catch (const std::exception& e) {
        require(std::string(e.what()).find(message) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error(std::string("Expected rejection: ") + message);
}
Cpu::SceImport identity() {
    return {"99b82IKXpH4", "libSceAppContent", 12, "libSceAppContentUtil", 14, 1, 1, 1};
}
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<const InstalledContentRecord> record = std::make_shared<const InstalledContentRecord>(
        InstalledContentRecord{1, {std::nullopt, -1234567, std::nullopt, 42}, "owned synthetic fixture metadata"});
    std::unique_ptr<ContentServices> services = std::make_unique<ContentServices>(machine, record,
        ContentAbi::PublicOrbisCandidate);
    Session() {
        machine.Map(0x1000, 4096, rx); machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw); machine.Map(0x4000, 4096, rw); machine.Map(0x6000, 4096, rw);
        // CALL [GOT], store RAX, return to an exact instruction boundary.
        constexpr std::array<std::uint8_t, 13> caller{0xff,0x15,0xfa,0x0f,0,0,0x48,0x89,0x05,0x03,0x10,0,0};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }
    std::uint64_t gate() { return services->Resolve(identity(), 2).value(); }
    std::uint64_t call(std::uint64_t address, std::uint64_t id, std::uint64_t output) {
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, id); machine.Set(Register::Rsi, output);
        machine.Set(Register::Rsp, 0x4ff0); machine.Set(Register::Rbx, 0xaabbccddeeff0011);
        require(machine.Run(0x1000, 0x100d, 100) == Cpu::StopReason::Address, "Guest content CALL did not return");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0xaabbccddeeff0011,
                "Content service corrupted guest stack/callee-save register");
        std::uint64_t stored;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Guest output differs from service return register");
        return stored;
    }
    std::vector<std::byte> bytes(std::uint64_t address, unsigned size) {
        std::vector<std::byte> value(size); machine.Read(address, value); return value;
    }
};

// Contract: only explicit installed values become exactly four guest LE bytes; errors never partially write.
// Regression: unconditional full-SKU/zero defaults, host-pointer writes, truncated spans or native-width stores.
// Existing GuestAppContent tests cover mount errors only, and host exports cannot test native guest permissions.
void integerBoundary() {
    Session s;
    const auto gate = s.gate();
    const std::vector<std::byte> sentinel(8, std::byte{0xa7});
    s.machine.Write(0x3000, sentinel);
    require(s.call(gate, 0, 0x3001) == 0, "Trial metadata query failed");
    require(s.bytes(0x3000, 6) == std::vector<std::byte>{std::byte{0xa7},std::byte{1},std::byte{0},
        std::byte{0},std::byte{0},std::byte{0xa7}}, "SKU metadata or exact byte output incorrect");
    require(s.call(gate, 2, 0x3001) == 0, "Signed installed parameter query failed");
    require(s.bytes(0x3001, 4) == std::vector<std::byte>{std::byte{0x79},std::byte{0x29},
        std::byte{0xed},std::byte{0xff}}, "Signed metadata LE representation incorrect");
    const auto before = s.bytes(0x3000, 8);
    s.machine.Write(0x6ffe, std::span(sentinel).first(2));
    for (const auto address : std::array<std::uint64_t, 4>{0,0x9000,0x6ffe,std::numeric_limits<std::uint64_t>::max()-1}) {
        require(s.call(gate, 0, address) == parameterError, "Invalid span did not return signed parameter error");
        require(s.bytes(0x3000, 8) == before, "Rejected pointer changed existing output");
        require(s.bytes(0x6ffe, 2) == std::vector<std::byte>(2,std::byte{0xa7}), "Cross-page rejection partially changed valid prefix");
    }
    require(s.call(gate, 9, 0x3001) == parameterError && s.bytes(0x3000, 8) == before,
        "Unsupported ID changed output or invented success");
    s.machine.Protect(0x3000, 4096, Permission::Read);
    require(s.call(gate, 0, 0x3001) == parameterError && s.bytes(0x3000, 8) == before,
        "Read-only guest output was modified");
    s.machine.Protect(0x3000, 4096, rw);
    rejects([&]{s.call(gate, 1, 0x3001);}, "lacks requested user parameter");
    require(s.bytes(0x3000, 8) == before, "Absent installed field fabricated data");
    require(s.call(gate, 0x100000000ULL, 0x3001) == 0, "32-bit parameter ABI did not truncate register padding");
}

// Contract: scope/type/version matching and opt-in protect title admission, and neither provider nor record outlives its owner.
// Regression: matching by NID alone, default binding, retained gate/callbacks or metadata leaking to a replacement owner.
// No existing test exercises this new content provider's ownership boundary; no test-only production hooks are added.
void ownershipAndBinding() {
    {
        Session s;
        auto original = identity();
        const auto gate = s.gate();
        require(s.services->Resolve(original, 2).value() == gate, "Content resolver is not idempotent");
        auto local = original; local.LibraryId = 4; local.ModuleId = 5;
        require(s.call(s.services->Resolve(local, 2).value(), 4, 0x3000) == 0, "Importer-local IDs prevented matching contract");
        for (unsigned i = 0; i < 6; ++i) {
            auto bad = original;
            switch(i) {
            case 0: bad.LibraryName="other"; break; case 1: bad.ModuleName="other"; break;
            case 2: bad.LibraryVersion=2; break; case 3: bad.ModuleMajor=2; break;
            case 4: bad.ModuleMinor=2; break; default: break;
            }
            rejects([&]{s.services->Resolve(bad, i == 5 ? 1 : 2);}, "scope/version/type");
        }
        auto other = original; other.LibraryName="libc"; other.ModuleName="libc";
        require(!s.services->Resolve(other, 2), "Content provider captured an unrelated family");
        other = original; other.Nid="R9lA82OraNs";
        require(!s.services->Resolve(other, 2), "Unimplemented content initialization was admitted");
        const auto mappingCount = s.machine.Mappings().size();
        ContentServices defaults(s.machine);
        require(!defaults.Resolve(original, 2) && s.machine.Mappings().size() == mappingCount,
            "Default candidate was title-admitted or allocated guest mappings");
        s.record.reset();
        rejects([&]{s.call(gate,0,0x3000);}, "installed-record owner has expired");
        s.services.reset();
        rejects([&]{s.machine.CheckAccess(gate,1,Permission::Execute);}, "Guest access denied");
        s.record = std::make_shared<const InstalledContentRecord>(InstalledContentRecord{
            3, {111,-55,0,8}, "separately owned replacement fixture metadata"});
        s.services = std::make_unique<ContentServices>(s.machine,s.record,ContentAbi::PublicOrbisCandidate);
        require(s.gate() == gate, "Released content gate page could not be reused at the same base");
        require(s.call(gate,0,0x3000) == 0 && s.bytes(0x3000,4) ==
            std::vector<std::byte>{std::byte{3},std::byte{0},std::byte{0},std::byte{0}},
            "Replacement gate retained stale trial metadata or a stale callback");
        require(s.call(gate,2,0x3000) == 0 && s.bytes(0x3000,4) ==
            std::vector<std::byte>{std::byte{0xc9},std::byte{0xff},std::byte{0xff},std::byte{0xff}},
            "Replacement gate retained the previous owner's signed user parameter");
        s.services.reset();
        rejects([&]{s.machine.CheckAccess(gate,1,Permission::Execute);}, "Guest access denied");
    }
    Session independent;
    require(independent.call(independent.gate(),0,0x3000) == 0, "Expired session contaminated a separate title owner");
}

// Contract: compiled freestanding x86-64 caller consumes the real query bytes/return codes through its function pointer.
// Regression: ABI register/sign mistakes, invalid-pointer success, guest continuation failure or hard-coded full SKU.
// Handwritten gate calls above cannot establish compiled C caller interoperability and its multi-call output dependency.
void compiledCaller(const char* path) {
    Session s;
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "Cannot open compiled content guest");
    const std::vector<char> code((std::istreambuf_iterator<char>(input)), {});
    require(!code.empty() && code.size() <= 4096, "Invalid flat guest text size");
    s.machine.Write(0x1000, std::as_bytes(std::span(code)));
    struct Context { std::uint64_t query, output, badOutput; std::array<std::int32_t,6> result; std::array<std::int32_t,2> value; };
    static_assert(sizeof(Context) == 56);
    Context context{s.gate(), 0x3001, 0x9000, {}, {}};
    s.machine.Write(0x2100, std::as_bytes(std::span(&context,1)));
    const std::uint64_t returnAddress = 0x1ff0;
    s.machine.Write(0x4fe8, std::as_bytes(std::span(&returnAddress,1)));
    s.machine.Set(Register::Rdi,0x2100); s.machine.Set(Register::Rsp,0x4fe8);
    require(s.machine.Run(0x1000,returnAddress,10000) == Cpu::StopReason::Address, "Compiled content guest failed continuation");
    s.machine.Read(0x2100,std::as_writable_bytes(std::span(&context,1)));
    require(context.result == std::array<std::int32_t,6>{0,0,std::bit_cast<std::int32_t>(0x80d90002u),
        std::bit_cast<std::int32_t>(0x80d90002u),std::bit_cast<std::int32_t>(0x80d90002u),-1234567},
        "Compiled guest did not observe exact output preservation and parameter errors");
    require(context.value == std::array<std::int32_t,2>{1,-1234567}, "Compiled guest consumed fabricated metadata");
    require(s.machine.Get(Register::Rsp) == 0x4ff0, "Compiled guest did not unwind its own stack");
}
}
int main(int argc, char** argv) {
    try {
        integerBoundary(); ownershipAndBinding();
        if (argc == 2) compiledCaller(argv[1]);
        else throw std::runtime_error("Compiled ContentServicesGuest flat text path is required");
        std::cout << "PASS content candidate: installed metadata, compiled x86 caller, pointer errors, scope and owner cleanup\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
