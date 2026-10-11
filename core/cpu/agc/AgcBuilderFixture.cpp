#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include "AgcUpstreamBuilders.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Contract: AGC command builders run from real x86 guest callers. When a
// command buffer is exhausted the builder calls the guest growth callback with
// (buffer, words + reserved, user data) exactly once, re-checks capacity, and
// returns a null packet when the callback refuses or leaves too little room.
// Multi-submit entries carry the single-submit word bound and fail with an SCE
// code before any submission. Regression: the bridge threw on exhaustion,
// submitted unbounded multi entries, and accepted dispatch modifier bits the
// PM4 validator rejects.
namespace {
using Contract = Cpu::AgcAbiContract;
using Cpu::Register;
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto RX = Cpu::Permission::Read | Cpu::Permission::Execute;
constexpr std::uint64_t Got = 0x2000, Result = 0x2010, Stack = 0x3fc0;
constexpr std::uint64_t Descriptor = 0x10000, Record = 0x10100, Small = 0x11000, Grown = 0x12000, Data = 0x13000;
constexpr std::uint64_t GrowCallback = 0x4000, RefuseCallback = 0x4100, StaleCallback = 0x4200;
constexpr std::uint32_t InvalidArgument = 0x80020016u;

void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& function, const char* reason) {
    try { function(); } catch (const std::exception& error) {
        require(std::string(error.what()).find(reason) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error(std::string("missing AGC rejection: ") + reason);
}
Cpu::SceImport identity(const char* nid, bool driver) {
    Cpu::SceImport value;
    value.Nid = nid; value.LibraryName = driver ? "libSceAgcDriver" : "libSceAgc";
    value.ModuleName = value.LibraryName; value.LibraryId = 7; value.ModuleId = 8;
    value.LibraryVersion = 1; value.ModuleMajor = 1; value.ModuleMinor = 1;
    return value;
}
template<class T> void store(Cpu::Machine& guest, std::uint64_t address, T value) {
    guest.Write(address, std::as_bytes(std::span(&value, 1)));
}
template<class T> T load(Cpu::Machine& guest, std::uint64_t address) {
    T value{}; guest.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
}
void imm64(std::vector<std::uint8_t>& code, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) code.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}
// Records rdi/rsi/rdx/rsp and a call count at Record, optionally switches the
// buffer to the chunk [Grown, Grown + 1KiB), clobbers argument registers and
// returns eax.
std::vector<std::uint8_t> callback(bool grow, std::uint32_t result) {
    std::vector<std::uint8_t> code{0x48, 0xb9}; imm64(code, Record);              // movabs rcx, Record
    code.insert(code.end(), {0x48, 0x89, 0x39, 0x48, 0x89, 0x71, 0x08, 0x48, 0x89, 0x51, 0x10,
                             0x48, 0xff, 0x41, 0x18, 0x48, 0x89, 0x61, 0x20});      // stores, ++count, rsp
    if (grow) {
        code.insert(code.end(), {0x48, 0xb8}); imm64(code, Grown);                 // movabs rax, Grown
        code.insert(code.end(), {0x48, 0x89, 0x07, 0x48, 0x89, 0x47, 0x10});       // bottom, up
        code.insert(code.end(), {0x48, 0xb8}); imm64(code, Grown + 1024);
        code.insert(code.end(), {0x48, 0x89, 0x47, 0x08, 0x48, 0x89, 0x47, 0x18}); // top, down
    }
    code.insert(code.end(), {0x31, 0xff, 0x31, 0xf6, 0x31, 0xd2, 0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9, 0xb8});
    for (int index = 0; index < 4; ++index) code.push_back(static_cast<std::uint8_t>(result >> (index * 8)));
    code.push_back(0xc3);
    return code;
}

struct Guest {
    Cpu::Machine Machine;
    std::vector<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>> Submissions;
    std::unique_ptr<Cpu::SceAgcImports> Imports;
    explicit Guest(std::span<const Contract> contracts) {
        map();
        Imports = std::make_unique<Cpu::SceAgcImports>(Machine, backend(), contracts);
    }
    explicit Guest(std::span<const Cpu::AgcAbiAdmission> admissions) {
        map();
        Imports = std::make_unique<Cpu::SceAgcImports>(Machine, backend(), admissions);
    }
    Cpu::SceAgcBackend backend() {
        Cpu::SceAgcBackend value;
        value.Submit = [this](std::uint64_t address, std::uint32_t words, std::uint8_t, std::uint32_t queue) {
            Submissions.emplace_back(address, words, queue);
        };
        return value;
    }
    void map() {
        Machine.Map(0x1000, 4096, RX); Machine.Map(0x2000, 4096, RW); Machine.Map(0x3000, 4096, RW);
        Machine.Map(0x4000, 4096, RX); Machine.Map(0x10000, 0x4000, RW);
        // call [rip+0xffa] -> Got; mov [rip+0x1003], rax -> Result; inc rbx; nop
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0, 0x48, 0xff, 0xc3, 0x90};
        Machine.Write(0x1000, std::as_bytes(std::span(caller)));
        const auto grow = callback(true, 1), refuse = callback(false, 0x100), stale = callback(false, 1);
        Machine.Write(GrowCallback, std::as_bytes(std::span(grow)));
        Machine.Write(RefuseCallback, std::as_bytes(std::span(refuse)));
        Machine.Write(StaleCallback, std::as_bytes(std::span(stale)));
    }
    void buffer(std::uint64_t up, std::uint64_t down, std::uint64_t function, std::uint32_t reserved) {
        const std::array<std::uint64_t, 6> fields{Small, Small + 4096, up, down, function, 0xabcdef12345ULL};
        Machine.Write(Descriptor, std::as_bytes(std::span(fields)));
        store(Machine, Descriptor + 48, reserved);
        store(Machine, Descriptor + 52, std::uint32_t{0x5a5a5a5a});
        const std::array<std::uint64_t, 5> clear{};
        Machine.Write(Record, std::as_bytes(std::span(clear)));
    }
    std::uint64_t call(const char* nid, bool driver, std::span<const std::uint64_t> args) {
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        for (std::size_t index = 0; index < registers.size(); ++index)
            Machine.Set(registers[index], index < args.size() ? args[index] : 0);
        for (std::size_t index = registers.size(); index < args.size(); ++index)
            store(Machine, Stack + (index - registers.size()) * 8, args[index]);
        Machine.Set(Register::Rax, 0xfedcba9876543210ULL);
        Machine.Set(Register::Rbx, 0x123456789abcdef0ULL);
        Machine.Set(Register::R12, 0x0badc0ffee0ddf00ULL);
        Machine.Set(Register::Rsp, Stack);
        store(Machine, Got, Imports->Resolve(identity(nid, driver), 2, 0));
        require(Machine.Run(0x1000, 0x1011, 400) == Cpu::StopReason::Address, "AGC import did not return to its caller");
        require(Machine.Get(Register::Rsp) == Stack && Machine.Get(Register::Rbx) == 0x123456789abcdef1ULL &&
                Machine.Get(Register::R12) == 0x0badc0ffee0ddf00ULL, "AGC import corrupted the caller continuation");
        const auto result = load<std::uint64_t>(Machine, Result);
        require(result == Machine.Get(Register::Rax), "caller did not store the AGC return value");
        return result;
    }
    std::uint64_t up() { return load<std::uint64_t>(Machine, Descriptor + 16); }
    std::uint64_t record(int field) { return load<std::uint64_t>(Machine, Record + field * 8); }
};

void Growth() {
    constexpr std::array contracts{Contract::DcbSetShRegisterDirect, Contract::DcbWriteDataCommandBuffer56};
    Guest guest(contracts);
    const std::array<std::uint64_t, 2> sh{Descriptor, (std::uint64_t{0x11223344} << 32) | 0x207};
    // Two free words, three needed plus one reserved: one growth request for four.
    guest.buffer(Small + 4088, Small + 4096, GrowCallback, 1);
    require(guest.call("pFLArOT53+w", false, sh) == Grown, "grown builder did not return the new chunk packet");
    require(guest.record(0) == Descriptor && guest.record(1) == 4 && guest.record(2) == 0xabcdef12345ULL,
            "growth callback arguments differ from (buffer, words + reserved, user data)");
    require(guest.record(3) == 1 && guest.record(4) % 16 == 8, "growth callback count or entry stack alignment differs");
    require(guest.up() == Grown + 12, "grown cursor did not advance by the packet");
    require(load<std::uint32_t>(guest.Machine, Descriptor + 52) == 0x5a5a5a5a, "growth changed descriptor padding");
    const auto packet = load<std::array<std::uint32_t, 3>>(guest.Machine, Grown);
    require(packet == std::array{0xc0017600u, 0x207u, 0x11223344u}, "grown packet words differ");

    // Stack arguments survive the callback round trip: WriteData reads two.
    const std::array<std::uint32_t, 2> payload{0xcafef00d, 0x8badf00d};
    guest.Machine.Write(Data, std::as_bytes(std::span(payload)));
    guest.buffer(Small + 4092, Small + 4096, GrowCallback, 0);
    const std::array<std::uint64_t, 8> write{Descriptor, 5, 1, 0x7000, Data, 2, 0, 1};
    require(guest.call("i1jyy49AjXU", false, write) == Grown && guest.record(1) == 6 && guest.record(3) == 1,
            "grown write data differs");
    const auto words = load<std::array<std::uint32_t, 6>>(guest.Machine, Grown);
    require(words[0] == 0xc0043700u && (words[1] >> 20 & 1) == 1 && words[2] == 0x7000 && words[4] == payload[0] &&
            words[5] == payload[1], "grown write data lost its stack controls or payload");

    // Refusal (al == 0 even with high eax bits), stale space and no callback
    // all return a null packet and leave the command buffer untouched.
    for (const auto function : {RefuseCallback, StaleCallback, std::uint64_t{0}}) {
        guest.buffer(Small + 4088, Small + 4096, function, 1);
        guest.Machine.Write(Small + 4088, std::as_bytes(std::span(payload)));
        require(guest.call("pFLArOT53+w", false, sh) == 0, "failed growth did not return a null packet");
        require(guest.up() == Small + 4088 && load<std::array<std::uint32_t, 2>>(guest.Machine, Small + 4088) == payload,
                "failed growth modified the command buffer");
        require(guest.record(3) == (function ? 1u : 0u), "failed growth called the guest callback a wrong number of times");
    }
    // A callback that is not executable guest code is a failure, not a jump.
    guest.buffer(Small + 4088, Small + 4096, Data, 1);
    require(guest.call("pFLArOT53+w", false, sh) == 0 && guest.up() == Small + 4088, "non-code callback was entered");
    std::cout << "growth: callback grows once with stack arguments preserved; refusal, stale space and no callback return null\n";
}

void MultiBound() {
    constexpr std::array contracts{Contract::SubmitMultiDcbs, Contract::SubmitMultiAcbs};
    Guest guest(contracts);
    const std::array<std::uint64_t, 2> pointers{Data, Data + 256};
    guest.Machine.Write(Descriptor, std::as_bytes(std::span(pointers)));
    store(guest.Machine, Descriptor + 16, std::array<std::uint32_t, 2>{4, 0x100000});
    require(guest.call("6UzEidRZwkg", true, std::array<std::uint64_t, 3>{Descriptor, Descriptor + 16, 2}) == InvalidArgument,
            "oversized multi DCB entry did not return an SCE error");
    require(guest.call("HF3YllT3mXU", true, std::array<std::uint64_t, 4>{0x20, Descriptor, Descriptor + 16, 2}) == InvalidArgument,
            "oversized multi ACB entry did not return an SCE error");
    require(guest.call("6UzEidRZwkg", true, std::array<std::uint64_t, 3>{Descriptor, Descriptor + 16, 1025}) == InvalidArgument,
            "oversized multi count did not return an SCE error");
    require(guest.Submissions.empty(), "rejected multi submission reached the backend");
    store(guest.Machine, Descriptor + 20, std::uint32_t{8});
    require(guest.call("6UzEidRZwkg", true, std::array<std::uint64_t, 3>{Descriptor, Descriptor + 16, 2}) == 0 &&
            guest.Submissions.size() == 2 && guest.Submissions[1] == std::tuple{Data + 256, 8u, 0u},
            "bounded multi submission differs");
    std::cout << "multi: per-entry 0xfffff word bound and count bound return SCE errors before submission\n";
}

void DispatchMask() {
    constexpr std::array contracts{Contract::CbDispatchCommandBuffer56};
    Guest guest(contracts);
    guest.buffer(Small, Small + 4096, 0, 0);
    // Pm4::Validate accepts (modifier & ~0xa024) == 0x41 for opcode 0x15.
    rejects([&] { guest.call("k3GhuSNmBLU", false, std::array<std::uint64_t, 5>{Descriptor, 1, 1, 1, 0x8}); }, "invalid dispatch modifier");
    rejects([&] { guest.call("k3GhuSNmBLU", false, std::array<std::uint64_t, 5>{Descriptor, 1, 1, 1, 0x10}); }, "invalid dispatch modifier");
    require(guest.call("k3GhuSNmBLU", false, std::array<std::uint64_t, 5>{Descriptor, 2, 3, 4, 0xa024}) == Small,
            "validator-accepted dispatch modifier was rejected");
    require(load<std::array<std::uint32_t, 5>>(guest.Machine, Small) == std::array{0xc0031500u, 2u, 3u, 4u, 0xa065u},
            "dispatch packet differs");
    std::cout << "dispatch: builder modifier mask matches the PM4 validator\n";
}

const volatile void* pointer(std::uint64_t value) { return reinterpret_cast<const volatile void*>(value); }
// Each binding is called by a real guest caller (stack arguments from index 6,
// garbage above narrow register/stack arguments) and must emit exactly the
// words the upstream builder produces for the truncated values.
void Builders() {
    Guest guest(Cpu::TargetAgcAdmissions());
    for (const auto* nid : {"6UzEidRZwkg", "+T8Xo6LtFJI", "HF3YllT3mXU"})
        require(guest.Imports->Resolve(identity(nid, true), 2, 0) != 0, "target admissions omit a multi-submit contract");
    for (const auto* nid : {"pFLArOT53+w", "LHFXRrlTPD8", "w4-d0n60hdo"})
        require(guest.Imports->Resolve(identity(nid, false), 2, 0) != 0, "target admissions omit a direct register contract");
    const std::array<std::uint32_t, 3> values{0x01020304, 0x05060708, 0x090a0b0c};
    guest.Machine.Write(Data, std::as_bytes(std::span(values)));
    struct Case { const char* Nid; std::vector<std::uint64_t> Args; std::uint32_t* (*Oracle)(CommandBuffer*, const std::uint32_t*); };
    const std::vector<Case> cases{
        {"57labkp+rSQ", {0xabcdef01, 0x12345, 0x1234, 0x7000040, 0x1000, 0xffff000000000190ULL},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcDcbAcquireMem(b, 1, 0x12345, 0x1234, pointer(0x7000040), 0x1000, 0x190); }},
        {"KT-hTp-Ch14", {0x4321, 0x8000000, 0x300, 80},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcAcbAcquireMem(b, 0x4321, pointer(0x8000000), 0x300, 80); }},
        {"wr23dPKyWc0", {0x28, 0x123, 1, 2, 0x9000, 0x1100000002ULL, 0x1122334455667788ULL, 0, 1, 0x7700000002ULL, 0x1234},
            [](CommandBuffer* b, const std::uint32_t*) {
                return sceAgcCbReleaseMem(b, 0x28, 0x123, 1, 2, reinterpret_cast<const volatile Label*>(std::uintptr_t{0x9000}),
                                          2, 0x1122334455667788ULL, 0, 1, 2, 0x1234); }},
        {"VmW0Tdpy420", {1, 3, 5, 1, 0x9100, 0x55, 0xff, 0x100},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcDcbWaitRegMem(b, 1, 3, 5, 1, pointer(0x9100), 0x55, 0xff, 0x100); }},
        {"htn36gPnBk4", {0, 6, 2, 0x9204, 7, 0xffffffff, 0},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcAcbWaitRegMem(b, 0, 6, 2, pointer(0x9204), 7, 0xffffffff, 0); }},
        {"1rZSWUv1IRc", {0xa, 1, 0x9300, 5, 2, 0x9400, 1, 1},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcDcbCopyData(b, 0xa, 1, 0x9300, 5, 2, 0x9400, 1, 1); }},
        {"qzMN2XKGA4k", {5, 2, 0x9500, 5, 0, 0xdeadbeef, 0, 0},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcAcbCopyData(b, 5, 2, 0x9500, 5, 0, 0xdeadbeef, 0, 0); }},
        {"WmAc2MEj6Io", {1, 3, 1, 0xa000, 0x14, 2, 0xb000, 0x400, 1, 1, 1},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcDcbDmaData(b, 1, 3, 1, 0xa000, 0x14, 2, 0xb000, 0x400, 1, 1, 1); }},
        {"-RnpfpxIhec", {0xc, 3, 0xa100, 9, 1, 0xb100, 64, 0, 1},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcAcbDmaData(b, 0xc, 3, 0xa100, 9, 1, 0xb100, 64, 0, 1); }},
        {"1-gUn1PI4Sw", {0x61, 3, 1, 0x9600, 0x1111222233334444ULL, 0x5555, 0x100},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcDcbAtomicMem(b, 0x61, 3, 1, pointer(0x9600), 0x1111222233334444ULL, 0x5555, 0x100); }},
        {"XKKuA6VkSRc", {2, 0, 2, 0x9704, 9, 0, 0x1fff},
            [](CommandBuffer* b, const std::uint32_t*) { return sceAgcAcbAtomicMem(b, 2, 0, 2, pointer(0x9704), 9, 0, 0x1fff); }},
        {"BIPexNBSGog", {0x9804, 0x20},
            [](CommandBuffer* b, const std::uint32_t*) {
                return sceAgcDcbCondExec(b, reinterpret_cast<const volatile std::uint32_t*>(std::uintptr_t{0x9804}), 0x20); }},
        {"qyM2bxYFPAk", {0x9808, 0x3fff},
            [](CommandBuffer* b, const std::uint32_t*) {
                return sceAgcAcbCondExec(b, reinterpret_cast<const volatile std::uint32_t*>(std::uintptr_t{0x9808}), 0x3fff); }},
        {"n2fD4A+pb+g", {0x40, Data, 3},
            [](CommandBuffer* b, const std::uint32_t* v) { return sceAgcCbSetShRegisterRangeDirect(b, 0x40, v, 3); }},
        {"MDLD5Ly94Xk", {0x1000, Data + 4, 2},
            [](CommandBuffer* b, const std::uint32_t* v) { return sceAgcCbSetUcRegisterRangeDirect(b, 0x1000, v + 1, 2); }},
    };
    for (const auto& item : cases) {
        std::vector<std::uint64_t> args{Descriptor};
        args.insert(args.end(), item.Args.begin(), item.Args.end());
        guest.buffer(Small, Small + 4096, 0, 0);
        require(guest.call(item.Nid, false, args) == Small, std::string(item.Nid) + " did not return its packet address");
        std::array<std::uint32_t, 64> scratch{};
        CommandBuffer host{scratch.data(), scratch.data() + scratch.size(), scratch.data(), scratch.data() + scratch.size(),
                           nullptr, nullptr, 0};
        require(item.Oracle(&host, values.data()) == scratch.data(), "upstream oracle did not emit at the cursor");
        const auto words = static_cast<std::size_t>(host.cursor_up - scratch.data());
        require(guest.up() == Small + words * 4, std::string(item.Nid) + " cursor advance differs from upstream");
        std::vector<std::uint32_t> emitted(words);
        guest.Machine.Read(Small, std::as_writable_bytes(std::span(emitted)));
        require(std::equal(emitted.begin(), emitted.end(), scratch.begin()), std::string(item.Nid) + " packet differs from upstream");
    }
    // Upstream argument validation still refuses: bad wait comparison.
    guest.buffer(Small, Small + 4096, 0, 0);
    rejects([&] { guest.call("VmW0Tdpy420", false, std::array<std::uint64_t, 9>{Descriptor, 1, 7, 0, 0, 0x9100, 0, 0, 0}); },
            "invalid wait comparison");
    require(guest.up() == Small, "refused builder advanced the cursor");
    std::cout << "builders: " << cases.size() << " upstream builders marshal register/stack arguments and match upstream packets\n";
}
}

int main() {
    try {
        Growth();
        MultiBound();
        DispatchMask();
        Builders();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
