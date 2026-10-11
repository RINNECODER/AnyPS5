#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include "AgcUpstreamBuilders.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// The register defaults data is a header of static tables; its bank layout is
// the oracle for the pointer tables published into guest memory.
namespace Oracle {
struct CompactBank { ShaderRegister* registers; std::uint32_t registerCount; const std::uint16_t* offsets; std::uint32_t pointerCount; };
struct CompactRegisterDefaults { CompactBank banks[4]; std::uint32_t* types; std::uint32_t count; };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#include "prx/libSceAgc/Command/include/RegisterDefaultsData.hpp"
#pragma clang diagnostic pop
}

// Contract: the libSceAgcDriver leaf exports and the remaining libSceAgc rows a
// title imports resolve under the target admissions, read guest memory only
// through bounded copies and write results back to the guest. Driver queries
// report the upstream status codes; ACB flips and rendering waits encode the
// Metal driver's packets; register defaults and prim state are the upstream
// tables and values, published at guest addresses. Regression: every row was an
// unresolved import, so the first call aborted the title.
namespace {
using Cpu::Register;
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto RX = Cpu::Permission::Read | Cpu::Permission::Execute;
constexpr std::uint64_t Got = 0x2000, Result = 0x2010, Stack = 0x3fc0;
constexpr std::uint64_t Descriptor = 0x10000, Small = 0x11000, Data = 0x13000, DataEnd = 0x14000;
constexpr std::uint32_t RegistrationUnavailable = 0x8A6C9018u, DebugUnavailable = 0x8A6C1000u;

void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
Cpu::SceImport identity(const char* nid, bool driver) {
    Cpu::SceImport value;
    value.Nid = nid; value.LibraryName = driver ? "libSceAgcDriver" : "libSceAgc";
    value.ModuleName = value.LibraryName; value.LibraryId = 7; value.ModuleId = 8;
    value.LibraryVersion = 1; value.ModuleMajor = 1; value.ModuleMinor = 1;
    return value;
}
template<class T> void store(Cpu::Machine& guest, std::uint64_t address, const T& value) {
    guest.Write(address, std::as_bytes(std::span(&value, 1)));
}
template<class T> T load(Cpu::Machine& guest, std::uint64_t address) {
    T value{}; guest.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
}

struct Guest {
    Cpu::Machine Machine;
    std::unique_ptr<Cpu::SceAgcImports> Imports;
    Guest() {
        Machine.Map(0x1000, 4096, RX); Machine.Map(0x2000, 4096, RW); Machine.Map(0x3000, 4096, RW);
        Machine.Map(0x10000, 0x4000, RW);
        // call [rip+0xffa] -> Got; mov [rip+0x1003], rax -> Result; inc rbx; nop
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0, 0x48, 0xff, 0xc3, 0x90};
        Machine.Write(0x1000, std::as_bytes(std::span(caller)));
        Cpu::SceAgcBackend backend;
        backend.Submit = [](std::uint64_t, std::uint32_t, std::uint8_t, std::uint32_t) {};
        Imports = std::make_unique<Cpu::SceAgcImports>(Machine, std::move(backend), Cpu::TargetAgcAdmissions());
    }
    void buffer() {
        const std::array<std::uint64_t, 6> fields{Small, Small + 4096, Small, Small + 4096, 0, 0};
        Machine.Write(Descriptor, std::as_bytes(std::span(fields)));
        store(Machine, Descriptor + 48, std::uint64_t{0});
    }
    std::uint64_t call(const char* nid, bool driver, std::vector<std::uint64_t> args) {
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        for (std::size_t index = 0; index < registers.size(); ++index)
            Machine.Set(registers[index], index < args.size() ? args[index] : 0);
        for (std::size_t index = registers.size(); index < args.size(); ++index)
            store(Machine, Stack + (index - registers.size()) * 8, args[index]);
        Machine.Set(Register::Rax, 0xfedcba9876543210ULL);
        Machine.Set(Register::Rbx, 0x123456789abcdef0ULL);
        Machine.Set(Register::Rsp, Stack);
        store(Machine, Got, Imports->Resolve(identity(nid, driver), 2, 0));
        require(Machine.Run(0x1000, 0x1011, 400) == Cpu::StopReason::Address, std::string(nid) + " did not return to its caller");
        require(Machine.Get(Register::Rsp) == Stack && Machine.Get(Register::Rbx) == 0x123456789abcdef1ULL,
                std::string(nid) + " corrupted the caller continuation");
        return load<std::uint64_t>(Machine, Result);
    }
    std::uint32_t status(const char* nid, bool driver, std::vector<std::uint64_t> args) {
        return static_cast<std::uint32_t>(call(nid, driver, std::move(args)));
    }
    std::vector<std::uint32_t> words(std::uint64_t at, std::size_t count) {
        std::vector<std::uint32_t> value(count);
        Machine.Read(at, std::as_writable_bytes(std::span(value)));
        return value;
    }
};

// Every row this port binds: (nid, driver library).
constexpr std::pair<const char*, bool> Ported[] = {
    {"5CdQTZIQPxM", true}, {"Zw7uUVPulbw", true}, {"Xq5WmbwPTnQ", true}, {"SAfhzJPcjuk", true}, {"FOwvmNlFLjM", true},
    {"Ddwk4gLT5j0", true}, {"qspAL8bgcBY", true}, {"+iAOE3jCnkk", true}, {"WHIOMbb+iIU", true}, {"chJWZcNSzjk", true},
    {"ICaGtkEIXTk", true}, {"8F5j7U-Bq6s", true}, {"+TN0oRTBxJQ", true}, {"rJUyMrDdxJg", true}, {"3AyTaWcF-H8", true},
    {"n5ElQVYsU1A", true}, {"SCoAN5fYlUM", true}, {"X-Nm5KLREeg", true}, {"mXn+K9E-wOA", true}, {"LepGrgk77sM", true},
    {"5l3IfCFJxBs", true}, {"ZLJk9r2+2Aw", true}, {"NghWEUXp1qM", true}, {"VOMSpd9+vxU", true}, {"W5z4eZrjEas", true},
    {"rI9lNAXPMIw", true}, {"M9yBzRKkjPc", true}, {"F0ZXt5q0ZTA", true}, {"uJziRsODk1c", true}, {"ls4jfY576lw", true},
    {"emP3ckeS2uo", true}, {"F0Y42t-3e18", true}, {"pWLG7WOpVcw", true}, {"U9ueyEhSkF4", true}, {"AOLcoIkQDgM", true},
    {"2JtWUUiYBXs", false}, {"wRbq6ZjNop4", false}, {"RTpj-tIlvZc", false}, {"SwI6QxqwAC0", false}, {"D9sr1xGUriE", false},
    {"Y3ymLfZ1384", false}, {"BYcSvEsINWU", false}, {"N7M3+N-sBCw", false}, {"8Kly1JrJUlw", false}, {"pxx-GoOSdw4", false},
    {"ms1xVoZ-Vwc", false}, {"P1CugZ99Uzc", false}, {"7Wa3aeJgeVU", false}, {"rP5xLdOf26k", false}, {"W0WEyog0f74", false},
    {"ebixW91gpPw", false}, {"GPbUp9jXQa8", false},
};

void Admission() {
    Guest guest;
    for (const auto& [nid, driver] : Ported) {
        guest.Imports->Resolve(identity(nid, driver), 2, 0);
        bool rejected = false;
        try { guest.Imports->Resolve(identity(nid, !driver), 2, 0); } catch (const std::exception&) { rejected = true; }
        require(rejected, std::string(nid) + " resolved in the other AGC library scope");
    }
}

void DriverQueries() {
    Guest guest;
    // Integer outputs are written back; discarded pointers are never touched.
    store(guest.Machine, Data, std::uint64_t{0x5a5a5a5a5a5a5a5aULL});
    require(guest.status("uJziRsODk1c", true, {Data}) == 0 && load<std::uint32_t>(guest.Machine, Data) == 0xfc,
            "max resource name length differs from upstream");
    require(guest.status("AOLcoIkQDgM", true, {Data}) == 0 && load<std::uint64_t>(guest.Machine, Data) == 0x4000,
            "resource registration memory requirement differs from upstream");
    require(guest.status("F0Y42t-3e18", true, {}) == 0, "resource registration init failed");
    const std::array<std::uint8_t, 32> untouched = [] { std::array<std::uint8_t, 32> v{}; v.fill(0xa5); return v; }();
    store(guest.Machine, Data, untouched);
    const char name[] = "owner";
    guest.Machine.Write(Data + 0x100, std::as_bytes(std::span(name)));
    require(guest.status("X-Nm5KLREeg", true, {Data, Data + 0x100}) == RegistrationUnavailable, "owner registration status");
    require(guest.status("F0ZXt5q0ZTA", true, {Data}) == RegistrationUnavailable, "default owner status");
    require(guest.status("U9ueyEhSkF4", true, {Data}) == RegistrationUnavailable, "default owner registration status");
    // Seven arguments: user data is the caller's first stack slot.
    require(guest.status("W5z4eZrjEas", true, {Data, 1, 0x7000000, 0x1000, Data + 0x100, 2, 0x1122334455667788ULL}) ==
            RegistrationUnavailable, "resource registration status");
    require(guest.status("emP3ckeS2uo", true, {Data, 1, 0x40, 0x10, Data + 0x100, 2, 9}) == RegistrationUnavailable,
            "GDS resource registration status");
    for (const auto* nid : {"LepGrgk77sM", "M9yBzRKkjPc", "rI9lNAXPMIw", "ls4jfY576lw", "mXn+K9E-wOA"})
        require(guest.status(nid, true, {1, Data}) == RegistrationUnavailable, std::string(nid) + " query status");
    require(guest.status("NghWEUXp1qM", true, {1, Data, Data + 8}) == RegistrationUnavailable, "resource range status");
    require(guest.status("5l3IfCFJxBs", true, {0x1000, Data}) == RegistrationUnavailable, "find resources status");
    for (const auto* nid : {"SCoAN5fYlUM", "ZLJk9r2+2Aw", "pWLG7WOpVcw"})
        require(guest.status(nid, true, {1}) == RegistrationUnavailable, std::string(nid) + " unregister status");
    require(guest.status("VOMSpd9+vxU", true, {1, 7}) == RegistrationUnavailable, "set user data status");
    require(load<std::array<std::uint8_t, 32>>(guest.Machine, Data) == untouched, "a failed registration wrote guest outputs");
    require(guest.status("3AyTaWcF-H8", true, {4, Data}) == 0 && guest.status("n5ElQVYsU1A", true, {4}) == 0,
            "workload stream registration differs from upstream");
    for (const auto* nid : {"Ddwk4gLT5j0", "+TN0oRTBxJQ", "qspAL8bgcBY"})
        require(guest.status(nid, true, {}) == 0, std::string(nid) + " reports an active debug session");
    require(guest.status("SAfhzJPcjuk", true, {Data + 0x100}) == DebugUnavailable &&
            guest.status("FOwvmNlFLjM", true, {}) == DebugUnavailable && guest.status("Xq5WmbwPTnQ", true, {}) == DebugUnavailable,
            "capture requests must report the debug feature as unavailable");
    require(guest.call("SAfhzJPcjuk", true, {Data + 0x100}) == 0xffffffff8a6c1000ULL, "SCE status is not sign-extended");
    require(guest.status("rJUyMrDdxJg", true, {}) == 1, "shader debugging status differs from upstream");
    require(guest.status("+iAOE3jCnkk", true, {1}) == DebugUnavailable && guest.status("WHIOMbb+iIU", true, {Data}) == DebugUnavailable &&
            guest.status("chJWZcNSzjk", true, {Data}) == DebugUnavailable && guest.status("ICaGtkEIXTk", true, {Data}) == DebugUnavailable &&
            guest.status("8F5j7U-Bq6s", true, {10}) == DebugUnavailable, "submit validation must report the debug feature as unavailable");
    require(load<std::array<std::uint8_t, 32>>(guest.Machine, Data) == untouched, "validation queries wrote guest outputs");
}

void EqueueQueries() {
    Guest guest;
    // KernelEvent: ident64, filter16, flags16, fflags32, data64, udata64.
    const std::array<std::uint64_t, 4> graphics{0x41, std::uint64_t{0xfff2}, 0, 0xdeadbeef};
    store(guest.Machine, Data, graphics);
    require(guest.status("5CdQTZIQPxM", true, {Data}) == 0x41 && guest.status("Zw7uUVPulbw", true, {Data}) == 0x41,
            "graphics event type and context id differ from upstream");
    const std::array<std::uint64_t, 4> other{0x9, std::uint64_t{0xfff9}, 7, 0};
    store(guest.Machine, Data, other);
    require(guest.status("5CdQTZIQPxM", true, {Data}) == 7, "non-graphics event type is not the event data");
}

std::vector<std::uint32_t> hostEncode(const std::function<std::uint32_t*(CommandBuffer*)>& builder) {
    std::vector<std::uint32_t> scratch(256);
    CommandBuffer cb{scratch.data(), scratch.data() + scratch.size(), scratch.data(), scratch.data() + scratch.size(), nullptr, nullptr, 0};
    builder(&cb);
    return {scratch.data(), cb.cursor_up};
}

void Builders() {
    Guest guest;
    // The span is exactly `length` bytes: the text ends at the end of a mapping.
    const char text[] = "frame";
    const auto at = DataEnd - 5;
    guest.Machine.Write(at, std::as_bytes(std::span(text, 5)));
    const struct { const char* Nid; std::function<std::uint32_t*(CommandBuffer*)> Host; } spans[] = {
        {"BYcSvEsINWU", [&](CommandBuffer* b) { return sceAgcDcbSetMarkerSpan(b, text, 5, 7); }},
        {"N7M3+N-sBCw", [&](CommandBuffer* b) { return sceAgcDcbPushMarkerSpan(b, text, 5, 7); }},
        {"8Kly1JrJUlw", [&](CommandBuffer* b) { return sceAgcAcbPushMarkerSpan(b, text, 5, 7); }},
        {"pxx-GoOSdw4", [&](CommandBuffer* b) { return sceAgcAcbSetMarkerSpan(b, text, 5, 7); }},
    };
    for (const auto& span : spans) {
        guest.buffer();
        const auto expected = hostEncode(span.Host);
        require(guest.call(span.Nid, false, {Descriptor, at, 5, 7}) == Small, std::string(span.Nid) + " did not return its packet");
        require(guest.words(Small, expected.size()) == expected && load<std::uint64_t>(guest.Machine, Descriptor + 16) == Small + expected.size() * 4,
                std::string(span.Nid) + " marker span differs from upstream");
    }
    guest.buffer();
    require(guest.call("N7M3+N-sBCw", false, {Descriptor, 0, 0, 0}) == Small, "empty marker span was refused");

    guest.buffer();
    require(guest.call("ebixW91gpPw", false, {Descriptor, 3, 1, 1, 0x1122334455667788ULL}) == Small, "ACB flip did not return its packet");
    require(guest.words(Small, 6) == std::vector<std::uint32_t>{AgcDriver::FlipPacketHeader, 3, 1, 1, 0x55667788u, 0x11223344u},
            "ACB flip is not the Metal driver flip packet");
    guest.buffer();
    require(guest.call("GPbUp9jXQa8", false, {Descriptor, 3, 2}) == Small, "ACB rendering wait did not return its packet");
    require(guest.words(Small, 4) == std::vector<std::uint32_t>{AgcDriver::RenderingWaitPacketHeader, 3, 2, 0},
            "ACB rendering wait is not the Metal driver wait packet");

    require(guest.status("ms1xVoZ-Vwc", false, {0}) == sceAgcDcbBeginOcclusionQueryGetSize(0) &&
            guest.status("ms1xVoZ-Vwc", false, {1}) == sceAgcDcbBeginOcclusionQueryGetSize(1) &&
            guest.status("P1CugZ99Uzc", false, {}) == sceAgcDcbEndOcclusionQueryGetSize(), "occlusion query sizes differ from upstream");
    require(guest.status("RTpj-tIlvZc", false, {0}) == 0 && guest.status("SwI6QxqwAC0", false, {}) == 0,
            "disabled shader instrumentation differs from upstream");

    // Patchers: the guest packet is patched exactly as upstream patches a host copy.
    const auto patched = [&](const char* nid, std::vector<std::uint64_t> args, std::vector<std::uint32_t> packet,
                             const std::function<int(std::uint32_t*)>& host) {
        guest.Machine.Write(Data, std::as_bytes(std::span(packet)));
        args.insert(args.begin(), Data);
        require(guest.status(nid, false, args) == 0, std::string(nid) + " refused a valid packet");
        auto expected = packet;
        expected.resize(expected.size() + 16);
        require(host(expected.data()) == 0, std::string(nid) + " host oracle refused the packet");
        expected.resize(packet.size());
        require(guest.words(Data, packet.size()) == expected && expected != packet, std::string(nid) + " patched packet differs from upstream");
    };
    std::vector<std::uint32_t> acquire(8, 0); acquire[0] = 0xc0065800u; acquire[1] = 0x00001234u;
    patched("W0WEyog0f74", {1}, acquire, [](std::uint32_t* c) { return sceAgcAcquireMemSetEngine(c, 1); });
    std::vector<std::uint32_t> branch(14, 0); branch[0] = 0xc00c3f00u;
    const auto* target = reinterpret_cast<const volatile std::uint32_t*>(std::uintptr_t{0x7000100});
    patched("7Wa3aeJgeVU", {2, 0x7000100, 0x40}, branch, [&](std::uint32_t* c) { return sceAgcBranchPatchSetThenTarget_0300(c, 2, target, 0x40); });
    patched("rP5xLdOf26k", {1, 0x7000100, 0x41}, branch, [&](std::uint32_t* c) { return sceAgcBranchPatchSetElseTarget_0300(c, 1, target, 0x41); });
}

void RegisterDefaults() {
    Guest guest;
    std::size_t tables = 0;
    for (const bool internal : {false, true}) {
        for (std::uint32_t version = 0; version < 14; ++version) {
            const auto* compact = (internal ? Oracle::agcInternalRegDefaultsByVersion : Oracle::agcPublicRegDefaultsByVersion)[version];
            const auto* host = static_cast<const std::uint8_t*>(internal ? sceAgcGetRegisterDefaults2Internal(version) : sceAgcGetRegisterDefaults2(version));
            const auto at = guest.call(internal ? "wRbq6ZjNop4" : "2JtWUUiYBXs", false, {version});
            require(at && guest.call(internal ? "wRbq6ZjNop4" : "2JtWUUiYBXs", false, {version}) == at,
                    "register defaults are not one stable guest table per version");
            // RegisterDefaults: tables[4] (pointer to pointer table), registerCounts[4], types, count.
            const auto image = load<std::array<std::uint64_t, 8>>(guest.Machine, at);
            std::array<std::uint64_t, 8> hostImage{};
            std::memcpy(hostImage.data(), host, sizeof(hostImage));
            require(std::memcmp(&image[4], &hostImage[4], 16) == 0 && image[7] == hostImage[7], "register defaults counts differ");
            const auto count = static_cast<std::uint32_t>(image[7]);
            const auto* hostTypes = reinterpret_cast<const std::uint32_t*>(hostImage[6]);
            std::vector<std::uint32_t> types(count);
            if (count) guest.Machine.Read(image[6], std::as_writable_bytes(std::span(types)));
            require(std::equal(types.begin(), types.end(), hostTypes), "register defaults types differ");
            for (std::size_t bank = 0; bank < 4; ++bank) {
                const auto pointers = compact->banks[bank].pointerCount;
                require((pointers == 0) == (image[bank] == 0), "register defaults table presence differs");
                const auto* hostTable = reinterpret_cast<ShaderRegister* const*>(hostImage[bank]);
                for (std::uint32_t index = 0; index < pointers; ++index) {
                    const auto entry = load<std::uint64_t>(guest.Machine, image[bank] + index * 8);
                    const auto value = load<ShaderRegister>(guest.Machine, entry);
                    require(value.offset == hostTable[index]->offset && value.value == hostTable[index]->value,
                            "register defaults entry differs from upstream");
                    ++tables;
                }
            }
        }
    }
    require(tables > 0, "no register defaults entries were compared");
}

void PrimState() {
    Guest guest;
    // Synthetic GS: offsets and stage bits from the upstream register constants.
    ShaderSpecialRegs specials{};
    specials.ge_cntl = {0x25b, 0x00000080};
    specials.vgt_shader_stages_en = {0x2d5, 0x00000000};
    specials.vgt_gs_out_prim_type = {0x29b, 0x2};
    specials.ge_user_vgpr_en = {0x262, 0x1};
    Shader gs{};
    gs.type = 2; // ShaderBinaryType::Gs
    gs.specials = &specials;
    std::array<ShaderRegister, 2> cx{}; std::array<ShaderRegister, 3> uc{};
    require(sceAgcCreatePrimState(cx.data(), uc.data(), nullptr, &gs, 4) == 0, "host oracle refused the synthetic GS");

    constexpr std::uint64_t ShaderAt = Data, SpecialsAt = Data + 0x100, CxAt = Data + 0x200, UcAt = Data + 0x240;
    auto guestShader = gs;
    guestShader.specials = reinterpret_cast<ShaderSpecialRegs*>(std::uintptr_t{SpecialsAt});
    guestShader.code = reinterpret_cast<const volatile void*>(std::uintptr_t{0x7000000});
    store(guest.Machine, ShaderAt, guestShader);
    store(guest.Machine, SpecialsAt, specials);
    require(guest.status("D9sr1xGUriE", false, {CxAt, UcAt, 0, ShaderAt, 4}) == 0, "CreatePrimState refused the synthetic GS");
    const auto guestCx = load<std::array<ShaderRegister, 2>>(guest.Machine, CxAt);
    const auto guestUc = load<std::array<ShaderRegister, 3>>(guest.Machine, UcAt);
    require(std::memcmp(&guestCx, &cx, sizeof(cx)) == 0 && std::memcmp(&guestUc, &uc, sizeof(uc)) == 0,
            "CreatePrimState registers differ from upstream");
    require(sceAgcUpdatePrimState(cx.data(), uc.data(), 6) == 0, "host oracle refused the update");
    require(guest.status("Y3ymLfZ1384", false, {CxAt, UcAt, 6}) == 0, "UpdatePrimState refused the registers");
    require(load<std::array<ShaderRegister, 2>>(guest.Machine, CxAt)[1].value == cx[1].value &&
            load<std::array<ShaderRegister, 3>>(guest.Machine, UcAt)[2].value == uc[2].value,
            "UpdatePrimState registers differ from upstream");
    require(guest.status("D9sr1xGUriE", false, {0, 0, 0, 0, 4}) == 0, "CreatePrimState without outputs must succeed");
}
}

int main() {
    try {
        Admission();
        DriverQueries();
        EqueueQueries();
        Builders();
        RegisterDefaults();
        PrimState();
    } catch (const std::exception& error) {
        std::cerr << "AGC driver leaf fixture failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "agc driver leaf: " << std::size(Ported) << " rows resolve; driver queries, events, builders, patchers, "
                 "register defaults and prim state match upstream\n";
    return 0;
}
