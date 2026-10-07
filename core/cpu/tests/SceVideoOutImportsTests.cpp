#include <cpu/SceVideoOutImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
using Cpu::Register;
using Cpu::Permission;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class TFunction> void rejects(TFunction&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing VideoOut rejection: ") + expected);
}

Cpu::SceImport import(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libSceVideoOut";
    value.LibraryId = 39;
    value.ModuleName = "libSceVideoOut";
    value.ModuleId = 40;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceVideoOutImports> imports;

    explicit Session(Cpu::SceVideoOutBackend backend = {}) {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x5000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> program{
            0xff, 0x15, 0xfa, 0x0f, 0x00, 0x00,
            0x48, 0x89, 0x05, 0x03, 0x10, 0x00, 0x00,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(program)));
        imports = std::make_unique<Cpu::SceVideoOutImports>(machine, std::move(backend));
    }

    void arguments(const std::array<std::uint64_t, 8>& values, std::uint64_t stack = 0x4fc0) {
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        for (std::size_t index = 0; index < registers.size(); ++index) machine.Set(registers[index], values[index]);
        machine.Set(Register::Rsp, stack);
        machine.Set(Register::Rax, 0x7766554433221100);
        machine.Set(Register::Rbx, 0x123456789abcdef0);
        machine.Write(stack, std::as_bytes(std::span(values).last(2)));
    }

    std::uint64_t runGate(std::uint64_t gate) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        const auto stack = machine.Get(Register::Rsp);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "VideoOut host gate did not return to actual x86 GOT caller");
        require(machine.Get(Register::Rsp) == stack && machine.Get(Register::Rbx) == 0x123456789abcdef1,
                "VideoOut gate corrupted x86 stack or continuation");
        std::uint64_t stored = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "Actual x86 caller failed to store VideoOut return register");
        return stored;
    }

    std::uint64_t call(const char* nid, const std::array<std::uint64_t, 8>& values) {
        arguments(values);
        return runGate(imports->ResolvePublicFixture(import(nid)));
    }

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t size) {
        std::vector<std::uint8_t> result(size);
        machine.Read(address, std::as_writable_bytes(std::span(result)));
        return result;
    }
};

std::array<std::uint8_t, 80> attributeBytes() {
    return {0,0,0,0, 1,0,0,0, 0,0,0,0, 3,1,0,0, 137,0,0,0, 0,0,0,0,
            8,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,128, 136,119,102,85,68,51,34,17,
            8,2,0,0, 0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0};
}

void attributeAndRegistration() {
    unsigned calls = 0;
    Cpu::SceVideoOutBackend backend;
    backend.RegisterBuffers = [&](std::int32_t handle, std::int32_t set, std::int32_t start,
                                  std::span<const Cpu::SceVideoOutBuffer> rows,
                                  const Cpu::SceVideoOutAttribute& attribute, std::int32_t category) {
        ++calls;
        require(handle == -7 && set == 2 && start == 13 && rows.size() == 2 && category == 1,
                "VideoOut registration lost signed or stack arguments");
        require(rows[0].DataAddress == 0x12340000 && rows[0].MetadataAddress == 0x76543210 &&
                rows[1].DataAddress == 0x56780000 && rows[1].MetadataAddress == 0xfedcba98,
                "VideoOut registration cast numeric guest pointers to host pointers");
        require(attribute.Width == 259 && attribute.Height == 137 && attribute.TilingMode == 1 &&
                attribute.PixelFormat == 0x8000000000000000ULL && attribute.Option == 8 &&
                attribute.DccControl == 0x208 && attribute.DccClearColor == 0x1122334455667788,
                "VideoOut registration changed copied attribute fields");
        return std::int32_t{-2144796661};
    };
    Session session(std::move(backend));
    std::array<std::uint8_t, 96> sentinel;
    sentinel.fill(0xa5);
    session.machine.Write(0x3000, std::as_bytes(std::span(sentinel)));
    require(session.call("PjS5uASwcV8", {0x3008,0x8000000000000000ULL,0x100000001ULL,0x200000103ULL,
                                         0x300000089ULL,8,0x100000208ULL,0x1122334455667788ULL}) == 0x7766554433221100,
            "Void VideoOut attribute setter fabricated an integer return");
    const auto expected = attributeBytes();
    require(session.bytes(0x3008, 80) == std::vector<std::uint8_t>(expected.begin(), expected.end()),
            "VideoOut setter did not match independent exact 80-byte ABI oracle");
    require(session.bytes(0x3000,8) == std::vector<std::uint8_t>(8,0xa5) &&
            session.bytes(0x3058,8) == std::vector<std::uint8_t>(8,0xa5), "VideoOut setter wrote beyond its 80-byte output");
    const std::array<std::uint64_t,8> rows{0x12340000,0x76543210,0,0,0x56780000,0xfedcba98,0,0};
    session.machine.Write(0x3100, std::as_bytes(std::span(rows)));
    const std::array<std::uint64_t,8> args{0x12345678fffffff9ULL,2,13,0x3100,2,0x3008,1,0};
    require(session.call("rKBUtgRrtbk", args) == 0x8029000bULL && calls == 1,
            "VideoOut registration did not preserve int return ABI or invoke native callback once");
    auto bad = args;
    bad[7] = 1;
    rejects([&] { session.call("rKBUtgRrtbk", bad); }, "register buffer option");
    bad = args;
    bad[6] = 2;
    rejects([&] { session.call("rKBUtgRrtbk", bad); }, "category");
    bad = args;
    bad[2] = 15;
    rejects([&] { session.call("rKBUtgRrtbk", bad); }, "slot or count");
    bad = args;
    bad[3] = 0x5fe0;
    rejects([&] { session.call("rKBUtgRrtbk", bad); }, "Guest access denied");
    require(calls == 1, "Rejected buffer descriptors reached native state publication");
    session.machine.Protect(0x3000,4096,Permission::Read);
    rejects([&] { session.call("PjS5uASwcV8", {0x3008,0,0,0,0,0,0,0}); }, "permission");
    require(session.bytes(0x3008,80) == std::vector<std::uint8_t>(expected.begin(),expected.end()),
            "Rejected attribute setter partially mutated guest bytes");
    session.machine.Protect(0x3000,4096,rw);
    session.arguments({0x3008,0,0,0,0,0,0,0},0x4ff8);
    session.machine.Protect(0x5000,4096,Permission::Write);
    rejects([&] { session.runGate(session.imports->ResolvePublicFixture(import("PjS5uASwcV8"))); }, "permission");
    require(session.bytes(0x3008,80) == std::vector<std::uint8_t>(expected.begin(),expected.end()),
            "Unreadable eighth stack argument did not stop setter before output write");
}

void nativeBoundary() {
    unsigned opens = 0, statuses = 0, closes = 0, rates = 0, unregisters = 0;
    Cpu::SceVideoOutBackend backend;
    backend.Open = [&](std::int32_t user, std::int32_t bus, std::int32_t index,
                       const std::optional<Cpu::SceVideoOutOpenParam>& param) {
        ++opens;
        require(user == 255 && bus == 2 && index == 0 && param && param->FirstWord == 16 &&
                param->SetPriority == 1 && param->Priority == 511 && param->SetAffinity == 0 && param->Affinity == 0,
                "VideoOut open did not copy bounded optional prefix");
        return std::int32_t{9};
    };
    backend.GetOutputStatus = [&](std::int32_t handle) {
        ++statuses;
        require(handle == 9 || handle == -1, "VideoOut status lost signed handle");
        return Cpu::SceVideoOutStatusResult{handle == 9 ? 0 : -2144796661,
                                           {2,1,13,0x1122334455667788,{0,0,0}}};
    };
    backend.Close = [&](std::int32_t handle) { ++closes; require(handle == 9,"VideoOut close changed handle"); return std::int32_t{0}; };
    backend.SetFlipRate = [&](std::int32_t handle,std::int32_t rate) {
        ++rates; require(handle == 9 && rate == 2,"VideoOut rate lost arguments"); return std::int32_t{0};
    };
    backend.UnregisterBuffers = [&](std::int32_t handle,std::int32_t set) {
        ++unregisters; require(handle == 9 && set == 3,"VideoOut unregister lost set"); return std::int32_t{0};
    };
    Session session(std::move(backend));
    const std::array<std::uint32_t,4> prefix{16,1,511,0};
    session.machine.Write(0x5ff0,std::as_bytes(std::span(prefix)));
    require(session.call("Up36PTk687E",{255,2,0,0x5ff0,0,0,0,0}) == 9 && opens == 1,
            "VideoOut open did not marshal callback handle to guest EAX");
    const std::array<std::uint32_t,4> affinityPrefix{16,1,511,1};
    session.machine.Write(0x5ff0,std::as_bytes(std::span(affinityPrefix)));
    rejects([&] { session.call("Up36PTk687E",{255,2,0,0x5ff0,0,0,0,0}); }, "Guest access denied");
    require(opens == 1,"Unreadable conditional affinity reached native open");
    require(session.call("utPrVdxio-8",{9,0x3208,0,0,0,0,0,0}) == 0 && statuses == 1,
            "VideoOut status callback failed");
    constexpr std::array<std::uint8_t,48> expected{2,0,0,0,1,0,0,0,13,0,0,0,0,0,0,0,
                                                  136,119,102,85,68,51,34,17,0,0,0,0,0,0,0,0,
                                                  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    require(session.bytes(0x3208,48) == std::vector<std::uint8_t>(expected.begin(),expected.end()),
            "VideoOut status was not serialized into exact 48-byte guest ABI");
    require(session.call("utPrVdxio-8",{0xffffffff,0x3208,0,0,0,0,0,0}) == 0x8029000bULL && statuses == 2 &&
            session.bytes(0x3208,48) == std::vector<std::uint8_t>(expected.begin(),expected.end()),
            "Native status error mutated guest output or lost return bits");
    session.machine.Protect(0x3000,4096,Permission::Read);
    rejects([&] { session.call("utPrVdxio-8",{9,0x3208,0,0,0,0,0,0}); }, "permission");
    require(statuses == 2,"Read-only output reached native status query");
    require(session.call("CBiu4mCE1DA",{9,2,0,0,0,0,0,0}) == 0 && rates == 1,"VideoOut flip rate failed");
    rejects([&] { session.call("CBiu4mCE1DA",{9,3,0,0,0,0,0,0}); }, "flip rate");
    require(session.call("N5KDtkIjjJ4",{9,3,0,0,0,0,0,0}) == 0 && unregisters == 1,"VideoOut unregister failed");
    require(session.call("uquVH4-Du78",{9,0,0,0,0,0,0,0}) == 0 && closes == 1,"VideoOut close failed");
    require(rates == 1,"Rejected flip rate mutated native state");
}

void failClosedScopes() {
    Session session;
    rejects([&] { session.call("Up36PTk687E",{255,0,0,0,0,0,0,0}); }, "without native backend");
    const auto original = import("uquVH4-Du78");
    const auto gate = session.imports->ResolvePublicFixture(original);
    require(session.imports->ResolvePublicFixture(original) == gate,"Full VideoOut identity changed gate");
    auto local = original;
    local.LibraryId = 7;
    local.ModuleId = 11;
    require(session.imports->ResolvePublicFixture(local) != gate,"VideoOut gate discarded importer-local scope IDs");
    for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
        auto wrong = original;
        switch (mismatch) {
        case 0: wrong.LibraryName = "other"; break;
        case 1: wrong.ModuleName = "other"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 2; break;
        }
        rejects([&] { session.imports->ResolvePublicFixture(wrong); },"scope/version");
    }
    auto unknown = original;
    unknown.Nid = "AAAAAAAAAAA";
    rejects([&] { session.imports->ResolvePublicFixture(unknown); },"import service");
    session.machine.CheckAccess(gate,1,Permission::Execute);
    rejects([&] { session.machine.CheckAccess(gate,1,Permission::Write); },"permission");
    session.imports.reset();
    session.arguments({9,0,0,0,0,0,0,0});
    rejects([&] { session.runGate(gate); },"runtime has expired");
}

}

int main() {
    try {
        attributeAndRegistration();
        nativeBoundary();
        failClosedScopes();
        std::cout << "PASS VideoOut typed guest CALL/RET ABI, bounded struct copies, native callback ordering and fail-closed scopes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
