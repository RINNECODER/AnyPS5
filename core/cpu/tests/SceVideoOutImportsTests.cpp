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
    // Invalid shapes are SCE results, not host exceptions.
    auto bad = args;
    bad[7] = 1;
    require(session.call("rKBUtgRrtbk", bad) == 0x8029001aULL, "Register option did not return INVALID_OPTION");
    bad = args;
    bad[6] = 2;
    require(session.call("rKBUtgRrtbk", bad) == 0x8029001dULL, "Register category did not return INVALID_CATEGORY");
    bad = args;
    bad[2] = 15;
    require(session.call("rKBUtgRrtbk", bad) == 0x80290001ULL, "Register slot overrun did not return INVALID_VALUE");
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
    require(session.call("CBiu4mCE1DA",{9,3,0,0,0,0,0,0}) == 0x80290001ULL, "Flip rate 3 did not return INVALID_VALUE");
    require(session.call("N5KDtkIjjJ4",{9,3,0,0,0,0,0,0}) == 0 && unregisters == 1,"VideoOut unregister failed");
    require(session.call("uquVH4-Du78",{9,0,0,0,0,0,0,0}) == 0 && closes == 1,"VideoOut close failed");
    require(rates == 1,"Rejected flip rate mutated native state");
}

// The production admission profile accepts generic shapes: any set, any 1-16
// buffer run, linear (tiling 1) or tiled, compressed, for any title (#113).
void genericTargetShapes() {
    unsigned registrations = 0;
    Cpu::SceVideoOutBackend backend;
    backend.RegisterBuffers = [&](std::int32_t handle, std::int32_t set, std::int32_t start,
                                  std::span<const Cpu::SceVideoOutBuffer> rows,
                                  const Cpu::SceVideoOutAttribute& attribute, std::int32_t category) {
        ++registrations;
        require(handle == 5 && set == 2 && start == 5 && rows.size() == 2 && category == 1 &&
                rows[1].DataAddress == 0x400010000ULL && rows[1].MetadataAddress == 0x500000000ULL &&
                attribute.TilingMode == 1 && attribute.PixelFormat == 0x8000000022000000ULL,
                "Generic registration shape was not forwarded intact");
        return std::int32_t{0};
    };
    backend.UnregisterBuffers = [](std::int32_t, std::int32_t set) { return set == 2 ? 0 : -1; };
    Session session;
    Cpu::SceVideoOutImports target(session.machine, std::move(backend), 0x7ffdfc000000, Cpu::TargetVideoOutAdmissions());
    // Attribute: linear tiling, a format outside the old three-format pin.
    session.arguments({0x3008,0x8000000022000000ULL,1,1280,720,0,0,0});
    session.runGate(target.Resolve(import("PjS5uASwcV8"), 2, 0));
    Cpu::SceVideoOutAttribute attribute;
    session.machine.Read(0x3008, std::as_writable_bytes(std::span(&attribute, 1)));
    require(attribute.TilingMode == 1 && attribute.Width == 1280 && attribute.PixelFormat == 0x8000000022000000ULL,
            "Target attribute setter refused or changed a linear attribute");
    const std::array<std::uint64_t,8> rows{0x400000000ULL,0x500000000ULL,0,0,0x400010000ULL,0x500000000ULL,0,0};
    session.machine.Write(0x3100, std::as_bytes(std::span(rows)));
    session.arguments({5,2,5,0x3100,2,0x3008,1,0});
    require(session.runGate(target.Resolve(import("rKBUtgRrtbk"), 2, 0)) == 0 && registrations == 1,
            "Target 2-buffer linear compressed registration was refused");
    session.arguments({5,2,0,0,0,0,0,0});
    require(session.runGate(target.Resolve(import("N5KDtkIjjJ4"), 2, 0)) == 0, "Target unregister of set 2 was refused");
    for (const auto* nid : {"U46NwOiJpys","HXzjK9yI30k","-Ozn0F1AFRg","Xru92wHJRmg","oNOQn3knW6s","SbU3dwp80lQ",
                            "1FZBKy8HeNU","j6RaAUlaLv0","zgXifHT9ErY","6kPnj51T62Y","w0hLuNarQxY","MTxxrOCeSig"})
        rejects([&] { target.Resolve(import(nid), 2, 0); }, "without native backend");
    // v1 registration needs only the register callback; the decoders need none.
    for (const auto* nid : {"w3BY+tAEiQY","U2JJtSqNKZI","rWUTcKdkUzQ","Mt4QHHkxkOc"})
        require(target.Resolve(import(nid), 2, 0) != 0, "Target event decoder was not admitted");
}

void fullSurface() {
    struct Calls { std::vector<std::string> log; } calls;
    Cpu::SceVideoOutBackend backend;
    backend.SubmitFlip = [&](std::int32_t handle, std::int32_t index, std::int32_t mode, std::int64_t argument) {
        require(handle == -7 && index == -2 && mode == 6 && argument == -5, "SubmitFlip lost signed arguments");
        calls.log.push_back("flip"); return static_cast<std::int32_t>(0x80290012u);
    };
    backend.AddEvent = [&](std::uint64_t queue, std::int32_t handle, std::int32_t kind, std::uint64_t udata) {
        require(queue == 0x0c09000000000001ULL && handle == 9 && udata == 0x1122334455667788ULL, "AddEvent lost arguments");
        calls.log.push_back(kind == 0 ? "add-flip" : "add-vblank"); return std::int32_t{0};
    };
    backend.DeleteEvent = [&](std::uint64_t queue, std::int32_t handle, std::int32_t kind) {
        require(queue == 0x0c09000000000001ULL && handle == 9, "DeleteEvent lost arguments");
        calls.log.push_back(kind == 0 ? "delete-flip" : "delete-vblank"); return std::int32_t{0};
    };
    backend.GetFlipStatus = [&](std::int32_t handle) {
        Cpu::SceVideoOutFlipStatus status;
        status.Count = 3; status.FlipArg = -9; status.FlipPendingNum = 1; status.CurrentBuffer = 2;
        status.ProcessTime = 0x0102030405060708ULL;
        return Cpu::SceVideoOutFlipStatusResult{handle == 9 ? 0 : static_cast<std::int32_t>(0x8029000b), status};
    };
    backend.GetVblankStatus = [&](std::int32_t) {
        Cpu::SceVideoOutVblankStatus status; status.Count = 77; status.Flags = 1;
        return Cpu::SceVideoOutVblankStatusResult{0, status};
    };
    backend.GetResolutionStatus = [&](std::int32_t) {
        Cpu::SceVideoOutResolutionStatus status;
        status.FullWidth = 1920; status.FullHeight = 1080; status.PaneWidth = 1920; status.PaneHeight = 1080;
        status.RefreshRate = 3; status.ScreenSizeInInch = 50.0f;
        return Cpu::SceVideoOutResolutionStatusResult{0, status};
    };
    backend.WaitVblank = [&](std::int32_t handle) { calls.log.push_back("wait"); return handle == 9 ? 0 : -1; };
    backend.IsFlipPending = [&](std::int32_t) { return std::int32_t{2}; };
    backend.ConfigureOutput = [&](std::int32_t handle, std::uint64_t mode) {
        require(handle == 9 && mode == 1, "ConfigureOutput lost mode"); calls.log.push_back("configure"); return 0;
    };
    backend.SetWindowModeMargins = [&](std::int32_t handle, std::int32_t top, std::int32_t bottom) {
        require(handle == 9 && top == -1 && bottom == 32, "Margins lost signed values"); return 0;
    };
    std::vector<std::int32_t> v1Sets;
    backend.RegisterBuffers = [&](std::int32_t handle, std::int32_t set, std::int32_t start,
                                  std::span<const Cpu::SceVideoOutBuffer> rows,
                                  const Cpu::SceVideoOutAttribute& attribute, std::int32_t category) {
        require(handle == 9 && start == 1 && rows.size() == 2 && rows[0].DataAddress == 0x400000000ULL &&
                rows[1].DataAddress == 0x400100000ULL && !rows[0].MetadataAddress && category == 0 &&
                attribute.PixelFormat == 0x8000000022000000ULL && attribute.TilingMode == 1 &&
                attribute.Width == 1920 && attribute.Height == 1080 && attribute.PitchInPixel == 2048,
                "RegisterBuffers v1 was not converted to the v2 contract");
        v1Sets.push_back(set);
        return set == 0 ? static_cast<std::int32_t>(0x8029000a) : 0; // set 0 occupied
    };
    Session session(std::move(backend));
    const auto fail = [](std::uint32_t code) { return std::uint64_t{code}; };

    require(session.call("U46NwOiJpys", {0xfffffff9,0xfffffffe,6,static_cast<std::uint64_t>(-5),0,0,0,0}) ==
            fail(0x80290012) && calls.log.back() == "flip", "SubmitFlip did not marshal native result");
    require(session.call("U46NwOiJpys", {9,0,0,0,0,0,0,0}) == fail(0x80290001) &&
            session.call("U46NwOiJpys", {9,16,1,0,0,0,0,0}) == fail(0x8029000a) && calls.log.size() == 1,
            "Invalid flip mode/index reached the backend or was not an SCE error");

    const std::uint64_t queue = 0x0c09000000000001ULL, user = 0x1122334455667788ULL;
    require(session.call("HXzjK9yI30k", {queue,9,user,0,0,0,0,0}) == 0 &&
            session.call("Xru92wHJRmg", {queue,9,user,0,0,0,0,0}) == 0 &&
            session.call("-Ozn0F1AFRg", {queue,9,0,0,0,0,0,0}) == 0 &&
            session.call("oNOQn3knW6s", {queue,9,0,0,0,0,0,0}) == 0 &&
            session.call("HXzjK9yI30k", {0,9,user,0,0,0,0,0}) == fail(0x8029000c),
            "Flip/vblank equeue registration did not marshal or reject a null queue");
    require((calls.log == std::vector<std::string>{"flip","add-flip","add-vblank","delete-flip","delete-vblank"}),
            "Event registration kinds differ");

    std::array<std::uint8_t,160> sentinel; sentinel.fill(0xa5);
    session.machine.Write(0x3200, std::as_bytes(std::span(sentinel)));
    require(session.call("SbU3dwp80lQ", {9,0x3200,0,0,0,0,0,0}) == 0, "GetFlipStatus failed");
    Cpu::SceVideoOutFlipStatus flip;
    session.machine.Read(0x3200, std::as_writable_bytes(std::span(&flip, 1)));
    require(flip.Count == 3 && flip.FlipArg == -9 && flip.FlipPendingNum == 1 && flip.CurrentBuffer == 2 &&
            session.bytes(0x3200, 8) == std::vector<std::uint8_t>{3,0,0,0,0,0,0,0} &&
            session.bytes(0x3200 + 52, 4) == std::vector<std::uint8_t>{1,0,0,0} &&
            session.bytes(0x3280, 32) == std::vector<std::uint8_t>(32, 0xa5),
            "Flip status differs from the 128-byte guest layout or overran it");
    session.machine.Write(0x3200, std::as_bytes(std::span(sentinel)));
    require(session.call("SbU3dwp80lQ", {8,0x3200,0,0,0,0,0,0}) == fail(0x8029000b) &&
            session.bytes(0x3200, 128) == std::vector<std::uint8_t>(128, 0xa5) &&
            session.call("SbU3dwp80lQ", {9,0,0,0,0,0,0,0}) == fail(0x80290002),
            "Failed flip status wrote output or null output was not INVALID_ADDRESS");
    require(session.call("1FZBKy8HeNU", {9,0x3200,0,0,0,0,0,0}) == 0 &&
            session.bytes(0x3200, 8) == std::vector<std::uint8_t>{77,0,0,0,0,0,0,0} &&
            session.bytes(0x3200 + 32, 1) == std::vector<std::uint8_t>{1} &&
            session.bytes(0x3200 + 40, 8) == std::vector<std::uint8_t>(8, 0xa5),
            "Vblank status differs from the 40-byte guest layout or overran it");
    session.machine.Write(0x3200, std::as_bytes(std::span(sentinel)));
    require(session.call("6kPnj51T62Y", {9,0x3200,0,0,0,0,0,0}) == 0, "GetResolutionStatus failed");
    Cpu::SceVideoOutResolutionStatus resolution;
    session.machine.Read(0x3200, std::as_writable_bytes(std::span(&resolution, 1)));
    require(resolution.FullWidth == 1920 && resolution.PaneHeight == 1080 && resolution.RefreshRate == 3 &&
            resolution.ScreenSizeInInch == 50.0f && session.bytes(0x3200 + 44, 4) == std::vector<std::uint8_t>(4, 0xa5),
            "Resolution status differs or wrote past its 44 defined bytes");

    require(session.call("j6RaAUlaLv0", {9,0,0,0,0,0,0,0}) == 0 && calls.log.back() == "wait" &&
            session.call("zgXifHT9ErY", {9,0,0,0,0,0,0,0}) == 2 &&
            session.call("MTxxrOCeSig", {9,0xffffffff,32,0,0,0,0,0}) == 0,
            "WaitVblank/IsFlipPending/SetWindowModeMargins did not pass through");
    std::array<std::uint32_t,16> options{}; session.machine.Write(0x3300, std::as_bytes(std::span(options)));
    require(session.call("w0hLuNarQxY", {9,1,0x3300,0,0,0,0,0}) == 0 && calls.log.back() == "configure", "ConfigureOutput failed");
    options[3] = 1; session.machine.Write(0x3300, std::as_bytes(std::span(options)));
    require(session.call("w0hLuNarQxY", {9,1,0x3300,0,0,0,0,0}) == fail(0x8029001a) &&
            session.call("w0hLuNarQxY", {9,1,0,0x3300,0,0,0,0}) == fail(0x80290001) &&
            calls.log.back() == "configure" && calls.log.size() == 7,
            "Nonzero output options or reserved arguments were not rejected before the backend");

    // v1 registration: A8B8G8R8 SRGB linear 2 buffers; first free set is returned.
    const std::array<std::uint32_t,10> legacy{0x80002200u,1,0,1920,1080,2048,0,0,0,0};
    session.machine.Write(0x3400, std::as_bytes(std::span(legacy)));
    const std::array<std::uint64_t,2> addresses{0x400000000ULL,0x400100000ULL};
    session.machine.Write(0x3440, std::as_bytes(std::span(addresses)));
    require(session.call("w3BY+tAEiQY", {9,1,0x3440,2,0x3400,0,0,0}) == 1 && (v1Sets == std::vector<std::int32_t>{0,1}),
            "RegisterBuffers v1 did not take the first free set and return its index");
    auto badFormat = legacy; badFormat[0] = 0x12345678u;
    session.machine.Write(0x3400, std::as_bytes(std::span(badFormat)));
    require(session.call("w3BY+tAEiQY", {9,1,0x3440,2,0x3400,0,0,0}) == fail(0x80290003) && v1Sets.size() == 2,
            "Unknown v1 pixel format reached the backend");

    // Event decoders read the 32-byte kernel record written by WaitEqueue.
    const auto data = [](std::uint64_t count, std::int64_t payload) {
        return 0x123ULL | (count << 12) | ((static_cast<std::uint64_t>(payload) & 0xffffffffffffULL) << 16);
    };
    const std::array<std::uint64_t,4> flipRecord{0, 0xfff3ULL | (1ULL << 16) | (2ULL << 32), data(2, -5), user};
    const std::array<std::uint64_t,4> vblankRecord{1, 0xfff3ULL | (1ULL << 16) | (1ULL << 32), data(1, 77), user};
    const std::array<std::uint64_t,4> graphicsRecord{0, 0xfff2ULL | (0x21ULL << 16), 1, user};
    session.machine.Write(0x3500, std::as_bytes(std::span(flipRecord)));
    session.machine.Write(0x3520, std::as_bytes(std::span(vblankRecord)));
    session.machine.Write(0x3540, std::as_bytes(std::span(graphicsRecord)));
    std::int64_t decoded = 0;
    require(session.call("U2JJtSqNKZI", {0x3500,0,0,0,0,0,0,0}) == 0 &&
            session.call("U2JJtSqNKZI", {0x3520,0,0,0,0,0,0,0}) == 1 &&
            session.call("Mt4QHHkxkOc", {0x3500,0,0,0,0,0,0,0}) == 2 &&
            session.call("rWUTcKdkUzQ", {0x3500,0x3600,0,0,0,0,0,0}) == 0,
            "Event id/count/data decoders failed on VideoOut records");
    session.machine.Read(0x3600, std::as_writable_bytes(std::span(&decoded, 1)));
    require(decoded == -5, "Flip event data lost the sign of its argument");
    require(session.call("rWUTcKdkUzQ", {0x3520,0x3600,0,0,0,0,0,0}) == 0, "Vblank event data failed");
    session.machine.Read(0x3600, std::as_writable_bytes(std::span(&decoded, 1)));
    require(decoded == 77, "Vblank event data differs from its count");
    require(session.call("U2JJtSqNKZI", {0x3540,0,0,0,0,0,0,0}) == fail(0x8029000d) &&
            session.call("rWUTcKdkUzQ", {0x3500,0,0,0,0,0,0,0}) == fail(0x80290002) &&
            session.call("U2JJtSqNKZI", {0,0,0,0,0,0,0,0}) == fail(0x80290002),
            "Non-VideoOut or null events were not SCE errors");
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
        genericTargetShapes();
        fullSurface();
        failClosedScopes();
        std::cout << "PASS VideoOut typed guest CALL/RET ABI, bounded struct copies, native callback ordering and fail-closed scopes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
