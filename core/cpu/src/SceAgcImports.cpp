#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include "SceShaders.hpp"
#include "AgcUpstreamBuilders.hpp"
#include "upstream-prx/Marshal.hpp"
#include "prx/libSceAgc/Shader/include/ShaderUtils.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace Cpu {
namespace UpstreamPrx { const Export* AgcDriverLeafExport(std::string_view nid); }
// Guest image of an upstream register defaults table laid out at `base`.
std::vector<std::byte> AgcRegisterDefaultsGuestImage(std::uint32_t version, bool internal, std::uint64_t base);
namespace {
using Contract = AgcAbiContract;
struct Binding { const char* Nid; Contract Operation; bool Driver; };
constexpr Binding bindings[] = {
    {"UglJIZjGssM", Contract::SubmitDcbPacket16, true},
    {"AhGvpITrf4M", Contract::AgrSubmitDcbPacket16, true},
    {"gSRnr79F8tQ", Contract::SubmitAcbPacket16, true},
    {"6UzEidRZwkg", Contract::SubmitMultiDcbs, true},
    {"+T8Xo6LtFJI", Contract::AgrSubmitMultiDcbs, true},
    {"HF3YllT3mXU", Contract::SubmitMultiAcbs, true},
    {"i1jyy49AjXU", Contract::DcbWriteDataCommandBuffer56, false},
    {"eZ4+17OQz4Q", Contract::AcbWriteDataCommandBuffer56, false},
    {"pFLArOT53+w", Contract::DcbSetShRegisterDirect, false},
    {"LHFXRrlTPD8", Contract::DcbSetCxRegisterDirect, false},
    {"w4-d0n60hdo", Contract::DcbSetUcRegisterDirect, false},
    {"k3GhuSNmBLU", Contract::CbDispatchCommandBuffer56, false},
    {"f3dg2CSgRKY", Contract::CreateShaderRelativeHeader96, false},
    {"h9z6+0hEydk", Contract::SuspendPoint, false},
    {"w2rJhmD+dsE", Contract::AddEqEvent, true},
    {"DL2RXaXOy88", Contract::DeleteEqEvent, true},
    {"YUeqkyT7mEQ", Contract::DcbSetFlipCommandBuffer56, false},
    {"MWiElSNE8j8", Contract::DcbWaitUntilSafeForRenderingCommandBuffer56, false},
    {"57labkp+rSQ", Contract::DcbAcquireMem, false},
    {"KT-hTp-Ch14", Contract::AcbAcquireMem, false},
    {"wr23dPKyWc0", Contract::CbReleaseMem, false},
    {"VmW0Tdpy420", Contract::DcbWaitRegMem, false},
    {"htn36gPnBk4", Contract::AcbWaitRegMem, false},
    {"1rZSWUv1IRc", Contract::DcbCopyData, false},
    {"qzMN2XKGA4k", Contract::AcbCopyData, false},
    {"WmAc2MEj6Io", Contract::DcbDmaData, false},
    {"-RnpfpxIhec", Contract::AcbDmaData, false},
    {"1-gUn1PI4Sw", Contract::DcbAtomicMem, false},
    {"XKKuA6VkSRc", Contract::AcbAtomicMem, false},
    {"BIPexNBSGog", Contract::DcbCondExec, false},
    {"qyM2bxYFPAk", Contract::AcbCondExec, false},
    {"n2fD4A+pb+g", Contract::CbSetShRegisterRangeDirect, false},
    {"MDLD5Ly94Xk", Contract::CbSetUcRegisterRangeDirect, false},
    {"ebixW91gpPw", Contract::AcbSetFlipCommandBuffer56, false},
    {"GPbUp9jXQa8", Contract::AcbWaitUntilSafeForRenderingCommandBuffer56, false}
};
struct GuestPacket { std::uint64_t Address; std::uint32_t Words; std::uint8_t Flags; std::array<std::uint8_t,3> Reserved; };
struct GuestCommandBuffer {
    std::uint64_t Bottom, Top, Up, Down, Callback, UserData;
    std::uint32_t ReservedWords, Padding;
};
static_assert(sizeof(GuestPacket) == 16 && sizeof(GuestCommandBuffer) == 56);
static_assert(sizeof(Shader) == 96 && offsetof(Shader, header_size) == 64 && offsetof(Shader, type) == 90);
static_assert(std::endian::native == std::endian::little);
constexpr std::uint32_t MaxSubmissionWords = 0xfffff, MaxMultiSubmissions = 1024;
// SCE_KERNEL_ERROR_EINVAL; the exact AGC driver code for these refusals is unverified.
constexpr std::uint32_t InvalidArgument = 0x80020016u;
// Pm4::Validate accepts a DISPATCH_DIRECT modifier only when
// (modifier & ~0xa024) == 0x41; the builder always ORs in 0x41.
constexpr std::uint32_t DispatchModifierMask = 0xa024u | 0x41u;

void require(bool valid, const char* reason) {
    if (!valid) throw std::runtime_error(std::string("SCE AGC: ") + reason);
}
void address(std::uint64_t value, std::size_t bytes, std::size_t alignment) {
    require(value && value % alignment == 0, "null or misaligned guest address");
    require(bytes && bytes <= std::numeric_limits<std::uint64_t>::max() - value, "guest address range overflow");
}
template<class T> T read(Machine& machine, std::uint64_t value) {
    address(value, sizeof(T), alignof(T));
    machine.CheckAccess(value, sizeof(T), Permission::Read);
    T result{};
    machine.Read(value, std::as_writable_bytes(std::span(&result, 1)));
    return result;
}
template<class T> void write(Machine& machine, std::uint64_t value, const T& data) {
    machine.Write(value, std::as_bytes(std::span(&data, 1)));
}
template<class Callback> void required(const Callback& callback) {
    require(static_cast<bool>(callback), "native backend contract is unavailable");
}
std::int32_t signed32(std::uint64_t value) { return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }
std::uint64_t numeric(const void* pointer) { return reinterpret_cast<std::uintptr_t>(pointer); }
template<class T> void relocate(T*& pointer, std::uint64_t fieldAddress) {
    if (!pointer) return;
    const auto delta = std::bit_cast<std::int64_t>(numeric(pointer));
    if (delta >= 0) require(static_cast<std::uint64_t>(delta) <= UINT64_MAX - fieldAddress, "shader relative pointer overflow");
    else require((std::uint64_t{0} - static_cast<std::uint64_t>(delta)) <= fieldAddress, "shader relative pointer underflow");
    pointer = reinterpret_cast<T*>(fieldAddress + static_cast<std::uint64_t>(delta));
}
void inside(std::uint64_t base, std::size_t bytes, std::uint64_t pointer, std::size_t length, std::size_t alignment) {
    if (!length) return;
    address(pointer, length, alignment);
    require(pointer >= base && pointer - base <= bytes && length <= bytes - (pointer - base),
            "external shader metadata is not supported by the relative-header contract");
}
std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
        " module=" + import.ModuleName + ":" + std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor);
}
}

bool GenericContractKnown(AgcAbiContract contract);

struct SceAgcImports::Impl : std::enable_shared_from_this<SceAgcImports::Impl> {
    // Generic upstream bindings (AgcGenericBindings) run through a handler; the
    // handler travels with a suspended builder so growth resumes the same call.
    using Handler = void (*)(Impl&, Machine&, Contract);
    Handler Active = nullptr;
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::uint16_t>;
    // A builder suspended in its guest growth callback, keyed by the stack slot
    // the callback's return lands on at the resume gate.
    struct Growth { Contract Operation; Handler Run; std::array<std::uint64_t, 6> Arguments; std::uint64_t Stack; };
    Machine& MachineRef;
    SceAgcBackend Backend;
    std::set<Contract> Admitted;
    std::map<Contract, AgcArgumentPolicy> Policies;
    std::optional<SceHostTrampolines> Trampolines;
    std::map<Key, std::uint64_t> Gates;
    std::optional<std::uint64_t> ResumeGate;
    std::map<std::uint64_t, Growth> PendingGrowth;
    bool Resuming = false;
    // Host-built tables (register defaults) the guest reads through returned
    // pointers live in a window above the gate table, mapped on first use.
    static constexpr std::uint64_t PublishedOffset = 0x400000, PublishedLimit = 0x400000;
    std::uint64_t PublishedBase = 0, PublishedUsed = 0;
    std::map<std::pair<std::uint32_t, bool>, std::uint64_t> RegisterDefaults;
    Impl(Machine& machine, SceAgcBackend backend, std::span<const Contract> admitted, std::uint64_t base) :
        MachineRef(machine), Backend(std::move(backend)), Admitted(admitted.begin(), admitted.end()),
        PublishedBase(base + PublishedOffset) {
        require(base && !(base & 4095) && base < 0x7ffffffff000, "gates require aligned low canonical guest page");
        for (auto contract : Admitted)
            require(std::any_of(std::begin(bindings), std::end(bindings), [contract](const auto& row) { return row.Operation == contract; }) ||
                    GenericContractKnown(contract), "unknown ABI admission contract");
        Trampolines.emplace(MachineRef, base, SceHostTrampolines::DefaultCapacity, "SCE Agc import");
    }

    // Writes one builder packet and returns its guest address in RAX. Exhaustion
    // mirrors upstream Agc::Command::Reserve: call the guest growth callback once
    // with (buffer, words + reserved, user data), then re-check the capacity. A
    // refused, unusable or insufficient growth returns a null packet.
    void emit(Machine& guest, std::uint64_t bufferAddress, std::span<const std::uint32_t> words, Contract operation) {
        const auto buffer = read<GuestCommandBuffer>(guest, bufferAddress);
        require(((buffer.Bottom | buffer.Top | buffer.Up | buffer.Down) & 3) == 0 && buffer.Bottom &&
                buffer.Bottom <= buffer.Up && buffer.Up <= buffer.Down && buffer.Down <= buffer.Top,
                "invalid command buffer cursor bounds");
        require(words.size() <= 0x4001, "command packet exceeds the maximum size");
        const auto needed = words.size() + std::uint64_t{buffer.ReservedWords};
        if ((buffer.Down - buffer.Up) / 4 < needed) {
            if (Resuming || !buffer.Callback || needed > std::numeric_limits<std::uint32_t>::max() || !grow(guest, bufferAddress, buffer, static_cast<std::uint32_t>(needed), operation))
                guest.Set(Register::Rax, 0);
            return;
        }
        // Validate both writes before modifying either. Only the cursor changes,
        // so guest callback, user data, reserved words and padding stay intact.
        address(buffer.Up, words.size_bytes(), 4);
        guest.CheckAccess(buffer.Up, words.size_bytes(), Permission::Write);
        guest.CheckAccess(bufferAddress + offsetof(GuestCommandBuffer, Up), 8, Permission::Write);
        require(buffer.Up + words.size_bytes() <= bufferAddress || bufferAddress + sizeof(buffer) <= buffer.Up,
                "command output overlaps command buffer descriptor");
        guest.Write(buffer.Up, std::as_bytes(words));
        write(guest, bufferAddress + offsetof(GuestCommandBuffer, Up), buffer.Up + words.size_bytes());
        guest.Set(Register::Rax, buffer.Up);
    }

    // Turns this gate's return into callback(buffer, words, user data) whose own
    // return lands on the resume gate. Argument registers are kept host-side;
    // stack arguments stay untouched above the original RSP.
    bool grow(Machine& guest, std::uint64_t bufferAddress, const GuestCommandBuffer& buffer, std::uint32_t words,
              Contract operation) {
        const auto stack = guest.Get(Register::Rsp);
        if (stack < 4096) return false;
        // Below the red zone; [frame] is popped into RIP, so the callback is
        // entered with RSP = frame + 8, the SysV call-entry alignment.
        const auto frame = (stack - 128 - 16) & ~std::uint64_t{15};
        try {
            guest.CheckAccess(buffer.Callback, 1, Permission::Execute);
            guest.CheckAccess(frame, 16, Permission::Read | Permission::Write);
        } catch (const std::exception&) { return false; }
        if (!ResumeGate) ResumeGate = Trampolines->Add([weak=weak_from_this()](Machine& resumed) {
            const auto state = weak.lock();
            require(static_cast<bool>(state), "import runtime expired");
            state->resume(resumed);
        });
        const std::array<std::uint64_t, 2> call{buffer.Callback, *ResumeGate};
        guest.Write(frame, std::as_bytes(std::span(call)));
        PendingGrowth.insert_or_assign(frame + 16, Growth{operation, Active, {guest.Get(Register::Rdi), guest.Get(Register::Rsi),
            guest.Get(Register::Rdx), guest.Get(Register::Rcx), guest.Get(Register::R8), guest.Get(Register::R9)}, stack});
        guest.Set(Register::Rsp, frame);
        guest.Set(Register::Rdi, bufferAddress);
        guest.Set(Register::Rsi, words);
        guest.Set(Register::Rdx, buffer.UserData);
        return true;
    }

    void resume(Machine& guest) {
        const auto found = PendingGrowth.find(guest.Get(Register::Rsp));
        require(found != PendingGrowth.end(), "command buffer growth resumed without a pending callback");
        const auto growth = found->second;
        PendingGrowth.erase(found);
        const bool granted = (guest.Get(Register::Rax) & 0xff) != 0; // C bool: only AL is defined
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        for (std::size_t index = 0; index < registers.size(); ++index) guest.Set(registers[index], growth.Arguments[index]);
        guest.Set(Register::Rsp, growth.Stack);
        if (!granted) { guest.Set(Register::Rax, 0); return; }
        Resuming = true;
        struct Clear { bool& Value; ~Clear() { Value = false; } } clear{Resuming};
        Active = growth.Run;
        if (growth.Run) growth.Run(*this, guest, growth.Operation); else invoke(guest, growth.Operation);
    }

    void submit(Machine& guest, std::uint64_t pointer, std::uint32_t queue, Contract operation) {
        required(Backend.Submit);
        const auto packet = read<GuestPacket>(guest, pointer);
        require(!packet.Flags, "nonzero command submission flags are unsupported");
        if (Policies.contains(operation))
            require(packet.Words <= MaxSubmissionWords, "target packet count outside qualified bound");
        if (packet.Words) {
            address(packet.Address, std::size_t(packet.Words) * 4, 4);
            guest.CheckAccess(packet.Address, std::size_t(packet.Words) * 4, Permission::Read);
        }
        Backend.Submit(packet.Address, packet.Words, packet.Flags, queue);
        guest.Set(Register::Rax, 0);
    }

    void multi(Machine& guest, std::uint64_t addresses, std::uint64_t sizes, std::uint32_t count, std::uint32_t queue) {
        if (!count) { guest.Set(Register::Rax, 0); return; }
        required(Backend.Submit);
        // Count and per-entry word bounds fail with an SCE code before any entry
        // is submitted; the entry bound matches the single-submit packet bound.
        if (count > MaxMultiSubmissions) { guest.Set(Register::Rax, InvalidArgument); return; }
        address(addresses, std::size_t(count) * 8, 8);
        address(sizes, std::size_t(count) * 4, 4);
        guest.CheckAccess(addresses, std::size_t(count) * 8, Permission::Read);
        guest.CheckAccess(sizes, std::size_t(count) * 4, Permission::Read);
        std::vector<std::uint64_t> pointers(count);
        std::vector<std::uint32_t> lengths(count);
        guest.Read(addresses, std::as_writable_bytes(std::span(pointers)));
        guest.Read(sizes, std::as_writable_bytes(std::span(lengths)));
        if (std::any_of(lengths.begin(), lengths.end(), [](auto words) { return words > MaxSubmissionWords; })) {
            guest.Set(Register::Rax, InvalidArgument);
            return;
        }
        for (std::size_t index=0; index<count; ++index) if (lengths[index]) {
            address(pointers[index], std::size_t(lengths[index]) * 4, 4);
            guest.CheckAccess(pointers[index], std::size_t(lengths[index]) * 4, Permission::Read);
        }
        for (std::size_t index=0; index<count; ++index) Backend.Submit(pointers[index], lengths[index], 0, queue);
        guest.Set(Register::Rax, 0);
    }

    void createShader(Machine& guest, std::uint64_t output, std::uint64_t base, std::uint64_t code) {
        required(Backend.RegisterShader);
        address(output, 8, 8);
        guest.CheckAccess(output, 8, Permission::Write);
        // Header bytes may be unaligned; copy into an aligned local value.
        address(base, sizeof(Shader), 1);
        guest.CheckAccess(base, sizeof(Shader), Permission::Read);
        Shader shader{};
        guest.Read(base, std::as_writable_bytes(std::span(&shader, 1)));
        require(shader.file_header == 0x34333231 && shader.version == 0x18 &&
                shader.header_size >= sizeof(Shader) && shader.header_size <= 1024 * 1024 &&
                shader.shader_size && !(shader.shader_size & 3), "invalid relative shader header");
        address(base, shader.header_size, 1);
        address(code, shader.shader_size, 256);
        require(output + 8 <= base || base + shader.header_size <= output, "shader output overlaps header");
        require(code + shader.shader_size <= base || base + shader.header_size <= code, "shader code overlaps header");
        require(output + 8 <= code || code + shader.shader_size <= output, "shader output overlaps code");
        guest.CheckAccess(base, shader.header_size, Permission::Read | Permission::Write);
        guest.CheckAccess(code, shader.shader_size, Permission::Read);
        std::vector<std::byte> staged(shader.header_size);
        guest.Read(base, staged);
        const auto original = staged;
        relocate(shader.user_data, base + offsetof(Shader, user_data));
        relocate(shader.cx_registers, base + offsetof(Shader, cx_registers));
        relocate(shader.sh_registers, base + offsetof(Shader, sh_registers));
        relocate(shader.specials, base + offsetof(Shader, specials));
        relocate(shader.input_semantics, base + offsetof(Shader, input_semantics));
        relocate(shader.output_semantics, base + offsetof(Shader, output_semantics));
        inside(base, staged.size(), numeric(shader.cx_registers), std::size_t(shader.num_cx_registers) * sizeof(ShaderRegister), 4);
        inside(base, staged.size(), numeric(shader.sh_registers), std::size_t(shader.num_sh_registers) * sizeof(ShaderRegister), 4);
        inside(base, staged.size(), numeric(shader.specials), shader.special_sizes_bytes, 4);
        inside(base, staged.size(), numeric(shader.input_semantics), std::size_t(shader.num_input_semantics) * sizeof(ShaderSemantic), 4);
        inside(base, staged.size(), numeric(shader.output_semantics), std::size_t(shader.num_output_semantics) * sizeof(ShaderSemantic), 4);
        if (shader.user_data) {
            const auto userAddress = numeric(shader.user_data);
            inside(base, staged.size(), userAddress, sizeof(ShaderUserData), 8);
            ShaderUserData user{};
            std::memcpy(&user, staged.data() + userAddress - base, sizeof(user));
            relocate(user.direct_resource_offset, userAddress + offsetof(ShaderUserData, direct_resource_offset));
            inside(base, staged.size(), numeric(user.direct_resource_offset), std::size_t(user.direct_resource_count) * 2, 2);
            for (std::size_t index=0; index<4; ++index) {
                relocate(user.sharp_resource_offset[index], userAddress + offsetof(ShaderUserData, sharp_resource_offset) + index * 8);
                inside(base, staged.size(), numeric(user.sharp_resource_offset[index]), std::size_t(user.sharp_resource_count[index]) * sizeof(ShaderSharp), 2);
            }
            std::memcpy(staged.data() + userAddress - base, &user, sizeof(user));
        }
        std::vector<ShaderRegister> registers(shader.num_sh_registers);
        if (!registers.empty()) std::memcpy(registers.data(), staged.data() + numeric(shader.sh_registers) - base, registers.size() * sizeof(ShaderRegister));
        PatchProgramAddressRegister(registers.data(), static_cast<std::uint32_t>(registers.size()), shader.type, code);
        if (!registers.empty()) std::memcpy(staged.data() + numeric(shader.sh_registers) - base, registers.data(), registers.size() * sizeof(ShaderRegister));
        shader.code = reinterpret_cast<const volatile void*>(code);
        std::memcpy(staged.data(), &shader, sizeof(shader));
        const std::array ranges{AgcReadableRange{base, staged.size()}, AgcReadableRange{code, shader.shader_size}};
        bool published = false;
        Backend.RegisterShader(base, ranges, [&] {
            guest.Write(base, staged);
            published = true;
        }, [&] { guest.Write(base, original); });
        require(published, "shader backend did not publish prepared header");
        write(guest, output, base);
        guest.Set(Register::Rax, 0);
    }

    // Runs an upstream libSceAgc builder against a host scratch buffer, then
    // emits its words to the guest buffer exactly as the hand-written builders do.
    template<class Builder> void build(Machine& guest, std::uint64_t bufferAddress, Contract operation, Builder&& builder) {
        Scratch.resize(0x4001); // one maximum-size packet; only emitted words are read back
        CommandBuffer host{Scratch.data(), Scratch.data() + Scratch.size(), Scratch.data(), Scratch.data() + Scratch.size(),
                           nullptr, nullptr, 0};
        require(builder(&host) == Scratch.data(), "upstream builder did not emit at the command cursor");
        emit(guest, bufferAddress, std::span<const std::uint32_t>(Scratch.data(), host.cursor_up), operation);
    }
    std::vector<std::uint32_t> Scratch;

    // One stable guest copy per (version, internal) table: guests compare and
    // cache the returned pointer like the static table upstream returns.
    std::uint64_t registerDefaults(Machine& guest, std::uint32_t version, bool internal) {
        if (const auto found = RegisterDefaults.find({version, internal}); found != RegisterDefaults.end()) return found->second;
        const auto at = PublishedBase + PublishedUsed;
        const auto image = AgcRegisterDefaultsGuestImage(version, internal, at);
        const auto size = (image.size() + 4095) & ~std::size_t{4095};
        require(size && size <= PublishedLimit - PublishedUsed, "published AGC table window exhausted");
        guest.Map(at, size, Permission::Read | Permission::Write);
        guest.Write(at, image);
        PublishedUsed += size;
        RegisterDefaults.emplace(std::pair{version, internal}, at);
        return at;
    }

    // Register ranges are the one builder input upstream dereferences: snapshot
    // the bounded guest values first. Invalid counts reach upstream's own check.
    void registerRange(Machine& guest, Contract operation, std::uint64_t buffer, std::uint32_t offset, std::uint64_t values,
                       std::uint32_t count) {
        std::vector<std::uint32_t> snapshot(count && count <= 0x3fff ? count : 0);
        // Null values are valid upstream (zero placeholders patched later).
        if (!snapshot.empty() && values) {
            address(values, snapshot.size() * 4, 4);
            guest.CheckAccess(values, snapshot.size() * 4, Permission::Read);
            guest.Read(values, std::as_writable_bytes(std::span(snapshot)));
        }
        const auto* host = snapshot.empty() || !values ? reinterpret_cast<const std::uint32_t*>(values) : snapshot.data();
        build(guest, buffer, operation, [&](CommandBuffer* cb) {
            return operation == Contract::CbSetShRegisterRangeDirect ? sceAgcCbSetShRegisterRangeDirect(cb, offset, host, count)
                                                                     : sceAgcCbSetUcRegisterRangeDirect(cb, offset, host, count);
        });
    }

    void invoke(Machine& guest, Contract operation) {
        const auto a=guest.Get(Register::Rdi), b=guest.Get(Register::Rsi), c=guest.Get(Register::Rdx),
            d=guest.Get(Register::Rcx), e=guest.Get(Register::R8), f=guest.Get(Register::R9);
        // SysV integer argument `index` (from zero); seventh and later are on the caller's stack.
        const auto arg = [&](std::size_t index) {
            const auto stack = guest.Get(Register::Rsp);
            require(stack <= UINT64_MAX - 8 * (index - 5), "stack argument overflow");
            return read<std::uint64_t>(guest, stack + 8 * (index - 5));
        };
        const auto u8 = [](std::uint64_t value) { return static_cast<std::uint8_t>(value); };
        const auto u16 = [](std::uint64_t value) { return static_cast<std::uint16_t>(value); };
        const auto u32 = [](std::uint64_t value) { return static_cast<std::uint32_t>(value); };
        const auto ptr = [](std::uint64_t value) { return reinterpret_cast<const volatile void*>(value); };
        switch (operation) {
        case Contract::DcbAcquireMem:
            build(guest, a, operation, [&](CommandBuffer* cb) { return sceAgcDcbAcquireMem(cb, u8(b), u32(c), u32(d), ptr(e), f, u32(arg(6))); });
            return;
        case Contract::AcbAcquireMem:
            build(guest, a, operation, [&](CommandBuffer* cb) { return sceAgcAcbAcquireMem(cb, u32(b), ptr(c), d, u32(e)); });
            return;
        case Contract::CbReleaseMem:
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return sceAgcCbReleaseMem(cb, u8(b), u16(c), u8(d), u8(e), reinterpret_cast<const volatile Label*>(f),
                    u8(arg(6)), arg(7), u16(arg(8)), u16(arg(9)), u8(arg(10)), u32(arg(11)));
            });
            return;
        case Contract::DcbWaitRegMem:
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return sceAgcDcbWaitRegMem(cb, u8(b), u8(c), u8(d), u8(e), ptr(f), arg(6), arg(7), u32(arg(8)));
            });
            return;
        case Contract::AcbWaitRegMem:
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return sceAgcAcbWaitRegMem(cb, u8(b), u8(c), u8(d), ptr(e), f, arg(6), u32(arg(7)));
            });
            return;
        case Contract::DcbCopyData: case Contract::AcbCopyData: {
            const auto copy = operation == Contract::DcbCopyData ? sceAgcDcbCopyData : sceAgcAcbCopyData;
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return copy(cb, u8(b), u8(c), d, u8(e), u8(f), arg(6), u8(arg(7)), u8(arg(8)));
            });
            return;
        }
        case Contract::DcbDmaData:
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return sceAgcDcbDmaData(cb, u8(b), u8(c), u8(d), e, u8(f), u8(arg(6)), arg(7), u32(arg(8)),
                    u8(arg(9)), u8(arg(10)), u8(arg(11)));
            });
            return;
        case Contract::AcbDmaData:
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return sceAgcAcbDmaData(cb, u8(b), u8(c), d, u8(e), u8(f), arg(6), u32(arg(7)), u8(arg(8)), u8(arg(9)));
            });
            return;
        case Contract::DcbAtomicMem: case Contract::AcbAtomicMem: {
            const auto atomic = operation == Contract::DcbAtomicMem ? sceAgcDcbAtomicMem : sceAgcAcbAtomicMem;
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return atomic(cb, u8(b), u8(c), u8(d), ptr(e), f, arg(6), u16(arg(7)));
            });
            return;
        }
        case Contract::DcbCondExec: case Contract::AcbCondExec: {
            const auto condition = operation == Contract::DcbCondExec ? sceAgcDcbCondExec : sceAgcAcbCondExec;
            build(guest, a, operation, [&](CommandBuffer* cb) {
                return condition(cb, reinterpret_cast<const volatile std::uint32_t*>(b), u32(c));
            });
            return;
        }
        case Contract::CbSetShRegisterRangeDirect: case Contract::CbSetUcRegisterRangeDirect:
            registerRange(guest, operation, a, u32(b), c, u32(d));
            return;
        case Contract::SubmitDcbPacket16: case Contract::AgrSubmitDcbPacket16: submit(guest, a, 0, operation); return;
        case Contract::SubmitAcbPacket16:
            require(static_cast<std::uint32_t>(a) >= 0x20 && static_cast<std::uint32_t>(a) < 0x58, "unsupported compute queue");
            submit(guest, b, static_cast<std::uint32_t>(a), operation); return;
        case Contract::SubmitMultiDcbs: case Contract::AgrSubmitMultiDcbs: multi(guest, a, b, static_cast<std::uint32_t>(c), 0); return;
        case Contract::SubmitMultiAcbs:
            require(static_cast<std::uint32_t>(a) >= 0x20 && static_cast<std::uint32_t>(a) < 0x58, "unsupported compute queue");
            multi(guest, b, c, static_cast<std::uint32_t>(d), static_cast<std::uint32_t>(a)); return;
        case Contract::DcbWriteDataCommandBuffer56: case Contract::AcbWriteDataCommandBuffer56: {
            const bool compute = operation == Contract::AcbWriteDataCommandBuffer56;
            const auto stack = guest.Get(Register::Rsp);
            require(stack <= UINT64_MAX - 8, "stack argument overflow");
            const auto tail = read<std::array<std::uint64_t,2>>(guest, stack + 8);
            const auto dst=static_cast<std::uint8_t>(b), cache=static_cast<std::uint8_t>(c),
                increment=static_cast<std::uint8_t>(tail[0]), confirm=static_cast<std::uint8_t>(tail[1]);
            const auto count = static_cast<std::uint32_t>(f);
            if (Policies.contains(operation)) {
                require((compute ? dst == 2 : dst == 4 || dst == 5) && increment == 0,
                        "write data controls outside qualified target memory use");
            }
            require(count && count <= 0x3ffd && dst <= (compute ? 15 : 31) && cache <= 3 && increment <= 1 && confirm <= 1,
                    "invalid write data controls or payload count");
            require((compute || !(d & 3)) && (dst || !confirm), "invalid write data destination");
            address(e, std::size_t(count) * 4, 1);
            guest.CheckAccess(e, std::size_t(count) * 4, Permission::Read);
            std::vector<std::uint32_t> words(count + 4);
            words[0]=0xc0003700u | ((count + 2) << 16);
            words[1]=(compute ? std::uint32_t(dst) << 8 : ((dst & 1u) << 30) | ((dst & 0x1eu) << 7)) |
                (std::uint32_t(increment) << 16) | (std::uint32_t(confirm) << 20) | (std::uint32_t(cache) << 25);
            words[2]=static_cast<std::uint32_t>(d); words[3]=static_cast<std::uint32_t>(d >> 32);
            guest.Read(e, std::as_writable_bytes(std::span(words).subspan(4)));
            emit(guest, a, words, operation); return;
        }
        case Contract::DcbSetShRegisterDirect: case Contract::DcbSetCxRegisterDirect: case Contract::DcbSetUcRegisterDirect: {
            const auto offset=static_cast<std::uint32_t>(b), value=static_cast<std::uint32_t>(b >> 32);
            require(offset <= 0xffff, "invalid direct register offset");
            const auto opcode = operation == Contract::DcbSetShRegisterDirect ? 0x76u : operation == Contract::DcbSetCxRegisterDirect ? 0x69u : 0x79u;
            const std::array words{0xc0010000u | (opcode << 8), offset, value};
            emit(guest, a, words, operation); return;
        }
        case Contract::CbDispatchCommandBuffer56: {
            const auto modifier=static_cast<std::uint32_t>(e);
            require(!(modifier & ~DispatchModifierMask), "invalid dispatch modifier");
            if (Policies.contains(operation)) require(modifier & 1, "dispatch modifier outside qualified target forwarding path");
            const std::array words{0xc0031500u, static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(d), modifier | 0x41u};
            emit(guest, a, words, operation); return;
        }
        case Contract::DcbSetFlipCommandBuffer56: case Contract::AcbSetFlipCommandBuffer56: {
            const auto handle = static_cast<std::uint32_t>(b), mode = static_cast<std::uint32_t>(d);
            const auto index = signed32(c);
            if (Policies.contains(operation)) {
                // This target registers three slots starting at zero; registry
                // ownership and the selected slot are validated on submission.
                require(handle > 0 && handle <= std::numeric_limits<std::int32_t>::max() && index >= 0 && index < 3 &&
                        mode == 1 && e == 0, "flip arguments outside qualified target use");
            }
            // This is AnyPS5's source-pinned internal driver encoding. The
            // imported producer ABI does not establish proprietary packet bytes.
            const std::array words{AgcDriver::FlipPacketHeader, handle, static_cast<std::uint32_t>(index), mode,
                static_cast<std::uint32_t>(e), static_cast<std::uint32_t>(e >> 32u)};
            emit(guest, a, words, operation); return;
        }
        case Contract::DcbWaitUntilSafeForRenderingCommandBuffer56: case Contract::AcbWaitUntilSafeForRenderingCommandBuffer56: {
            const auto handle = static_cast<std::uint32_t>(b);
            const auto index = static_cast<std::uint32_t>(c);
            if (Policies.contains(operation)) {
                require(handle > 0 && handle <= std::numeric_limits<std::int32_t>::max() && index < 3,
                        "rendering wait arguments outside qualified target use");
            }
            const std::array words{AgcDriver::RenderingWaitPacketHeader, handle, index, 0u};
            emit(guest, a, words, operation); return;
        }
        case Contract::CreateShaderRelativeHeader96: createShader(guest, a, b, c); return;
        case Contract::SuspendPoint: required(Backend.Suspend); Backend.Suspend(); guest.Set(Register::Rax, 0); return;
        case Contract::AddEqEvent:
            if (Policies.contains(operation))
                require(signed32(b) == 0 && c == 0, "graphics event arguments outside qualified target use");
            required(Backend.AddEvent);
            guest.Set(Register::Rax, static_cast<std::uint32_t>(Backend.AddEvent(a, signed32(b), c))); return;
        case Contract::DeleteEqEvent:
            if (Policies.contains(operation))
                require(signed32(b) == 0, "graphics event arguments outside qualified target use");
            required(Backend.DeleteEvent);
            guest.Set(Register::Rax, static_cast<std::uint32_t>(Backend.DeleteEvent(a, signed32(b)))); return;
        default: break;
        }
        throw std::runtime_error("SCE AGC: unhandled ABI contract");
    }
};

// Generic binding of upstream libSceAgc entry points by signature. Arguments
// are fetched per the SysV ABI (registers, then the caller's stack) and
// converted to the upstream parameter type. Pointer parameters carry the guest
// numeric value and are never dereferenced; the handlers that need memory
// (strings, register tables, packets under patch) marshal it explicitly.
struct AgcGenericBindings {
    using Impl = SceAgcImports::Impl;
    using Handler = Impl::Handler;
    struct Row { const char* Nid; Contract Operation; Handler Run; };

    template<class R, class... P> static std::tuple<P...> paramsOf(R (*)(P...));
    template<auto Fn> using ParamsOf = decltype(paramsOf(Fn));
    template<class T> static T conv(std::uint64_t value) {
        if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
        else if constexpr (std::is_same_v<T, bool>) return (value & 0xff) != 0;
        else return static_cast<T>(value);
    }
    static std::uint64_t arg(Machine& guest, std::size_t index) {
        switch (index) {
        case 0: return guest.Get(Register::Rdi);
        case 1: return guest.Get(Register::Rsi);
        case 2: return guest.Get(Register::Rdx);
        case 3: return guest.Get(Register::Rcx);
        case 4: return guest.Get(Register::R8);
        case 5: return guest.Get(Register::R9);
        default: break;
        }
        const auto stack = guest.Get(Register::Rsp);
        require(stack <= UINT64_MAX - 8 * (index - 5), "stack argument overflow");
        return read<std::uint64_t>(guest, stack + 8 * (index - 5));
    }
    // Calls Fn with the host values `pre` for its leading parameters and the
    // guest argument of the same index for the rest.
    template<auto Fn, class... Pre> static auto call(Machine& guest, Pre... pre) {
        using Params = ParamsOf<Fn>;
        constexpr std::size_t K = sizeof...(Pre);
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return Fn(pre..., conv<std::tuple_element_t<K + I, Params>>(arg(guest, K + I))...);
        }(std::make_index_sequence<std::tuple_size_v<Params> - K>{});
    }
    template<class R> static void result(Machine& guest, R value) {
        if constexpr (sizeof(R) <= 4) guest.Set(Register::Rax, static_cast<std::uint32_t>(value));
        else guest.Set(Register::Rax, static_cast<std::uint64_t>(value));
    }

    // Size queries and other pure numeric entry points.
    template<auto Fn> static void plain(Impl&, Machine& guest, Contract) { result(guest, call<Fn>(guest)); }
    // Encoders: argument 0 is the guest command buffer.
    template<auto Fn> static void builder(Impl& state, Machine& guest, Contract operation) {
        state.build(guest, arg(guest, 0), operation, [&](CommandBuffer* host) { return call<Fn>(guest, host); });
    }
    // Debug markers carry a guest C string; upstream encodes the text.
    template<auto Fn> static void marker(Impl& state, Machine& guest, Contract operation) {
        std::string text;
        if (const auto pointer = arg(guest, 1)) {
            for (std::uint64_t at = pointer;; ++at) {
                require(text.size() < 0xfffc, "marker text too long or unterminated");
                require(at >= pointer, "guest address range overflow");
                guest.CheckAccess(at, 1, Permission::Read);
                char character = 0;
                guest.Read(at, std::as_writable_bytes(std::span(&character, 1)));
                if (!character) break;
                text.push_back(character);
            }
        }
        state.build(guest, arg(guest, 0), operation, [&](CommandBuffer* host) {
            return Fn(host, text.c_str(), conv<std::uint32_t>(arg(guest, 2)));
        });
    }
    // Direct register tables are read by the encoder: snapshot the bounded guest table.
    template<auto Fn> static void registersDirect(Impl& state, Machine& guest, Contract operation) {
        const auto table = arg(guest, 1);
        const auto count = static_cast<std::uint32_t>(arg(guest, 2));
        require(count && count <= 0x3fff, "invalid direct register count");
        std::vector<ShaderRegister> registers(count);
        address(table, registers.size() * sizeof(ShaderRegister), 4);
        guest.CheckAccess(table, registers.size() * sizeof(ShaderRegister), Permission::Read);
        guest.Read(table, std::as_writable_bytes(std::span(registers)));
        state.build(guest, arg(guest, 0), operation, [&](CommandBuffer* host) { return Fn(host, registers.data(), count); });
    }

    // Packet patchers edit an already encoded packet in place. The packet is
    // copied to the host, patched by upstream, and written back on success.
    static std::vector<std::uint32_t> packet(Machine& guest, std::uint64_t at, bool writable) {
        const auto header = read<std::uint32_t>(guest, at);
        std::size_t words = 1;
        if ((header & 0xc0000000u) == 0xc0000000u && (header & 0x3fffff00u) != 0x3fff1000u) words = ((header >> 16) & 0x3fff) + 2u;
        address(at, words * 4, 4);
        const auto access = writable ? Permission::Read | Permission::Write : Permission::Read;
        guest.CheckAccess(at, words * 4, access);
        // Some patchers validate a short chain of packets (a wait is wrapped by
        // marker packets): include up to three following headers that are
        // readable PM4 packets, within a small fixed window.
        for (int chained = 0; writable && chained < 3 && words < 64; ++chained) {
            try {
                const auto next = read<std::uint32_t>(guest, at + words * 4);
                if ((next & 0xc0000000u) != 0xc0000000u) break;
                const std::size_t size = ((next >> 16) & 0x3fff) + 2u;
                if (words + size > 64) break;
                guest.CheckAccess(at + words * 4, size * 4, access);
                words += size;
            } catch (const std::exception&) { break; }
        }
        std::vector<std::uint32_t> copy(words + 16); // zero padding: upstream validates the header before use
        guest.Read(at, std::as_writable_bytes(std::span(copy).first(words)));
        return copy;
    }
    template<auto Fn> static void patch(Impl&, Machine& guest, Contract) {
        const auto at = arg(guest, 0);
        auto copy = packet(guest, at, true);
        const auto status = call<Fn>(guest, copy.data());
        guest.Write(at, std::as_bytes(std::span(copy).first(copy.size() - 16)));
        result(guest, status);
    }
    // The end of a conditional range is encoded relative to the packet itself:
    // present it at the same distance from the host copy as it has in the guest.
    template<auto Fn> static void patchRelative(Impl&, Machine& guest, Contract) {
        const auto at = arg(guest, 0);
        auto copy = packet(guest, at, true);
        const auto host = reinterpret_cast<std::uintptr_t>(copy.data()) + (arg(guest, 1) - at);
        const auto status = Fn(copy.data(), reinterpret_cast<const volatile std::uint32_t*>(host));
        guest.Write(at, std::as_bytes(std::span(copy).first(copy.size() - 16)));
        result(guest, status);
    }
    template<auto Fn> static void rangePatch(Impl&, Machine& guest, Contract) {
        const auto start = arg(guest, 0), end = arg(guest, 1);
        require(end >= start && !((end - start) & 3) && end - start <= 0x1000000, "invalid packet range");
        std::vector<std::uint32_t> copy((end - start) / 4);
        if (!copy.empty()) {
            address(start, copy.size() * 4, 4);
            guest.CheckAccess(start, copy.size() * 4, Permission::Read | Permission::Write);
            guest.Read(start, std::as_writable_bytes(std::span(copy)));
        }
        const auto status = Fn(copy.data(), copy.data() + copy.size(), conv<std::uint32_t>(arg(guest, 2)));
        if (!copy.empty()) guest.Write(start, std::as_bytes(std::span(copy)));
        result(guest, status);
    }
    template<auto Fn> static void packetSize(Impl&, Machine& guest, Contract) {
        auto copy = packet(guest, arg(guest, 0), false);
        result(guest, Fn(copy.data()));
    }
    // Payload queries return pointers into the packet: translate them back to guest addresses.
    static std::uint64_t translate(const void* host, const std::uint32_t* base, std::uint64_t at) {
        return host ? at + static_cast<std::uint64_t>(static_cast<const std::uint32_t*>(host) - base) * 4 : 0;
    }
    template<auto Fn> static void payloadAddress(Impl&, Machine& guest, Contract) {
        const auto out = arg(guest, 0), at = arg(guest, 1);
        address(out, 8, 8);
        guest.CheckAccess(out, 8, Permission::Write);
        auto copy = packet(guest, at, false);
        std::uint32_t* host = nullptr;
        const auto status = Fn(&host, copy.data(), static_cast<int>(arg(guest, 2)));
        write(guest, out, translate(host, copy.data(), at));
        result(guest, status);
    }
    template<auto Fn> static void payloadRange(Impl&, Machine& guest, Contract) {
        const auto out = arg(guest, 0), at = arg(guest, 1);
        address(out, 16, 8);
        guest.CheckAccess(out, 16, Permission::Write);
        auto copy = packet(guest, at, false);
        SceAgcMemoryRange range{};
        const auto status = Fn(&range, copy.data(), static_cast<int>(arg(guest, 2)));
        write(guest, out, std::array<std::uint64_t, 2>{translate(range.base, copy.data(), at), range.size});
        result(guest, status);
    }
    template<auto Fn> static void trinity(Impl&, Machine& guest, Contract) {
        const auto out = arg(guest, 0);
        address(out, 1, 1);
        guest.CheckAccess(out, 1, Permission::Write);
        bool flag = true;
        const auto status = Fn(&flag);
        write(guest, out, static_cast<std::uint8_t>(flag));
        result(guest, status);
    }

    // Marker spans carry exactly `length` text bytes, not a C string.
    template<auto Fn> static void markerSpan(Impl& state, Machine& guest, Contract operation) {
        const auto pointer = arg(guest, 1);
        const auto length = conv<std::uint32_t>(arg(guest, 2));
        require(length < 0x10000, "marker span text too long");
        std::vector<char> text(length);
        if (length) {
            require(pointer && pointer <= std::numeric_limits<std::uint64_t>::max() - length, "invalid marker span text range");
            guest.CheckAccess(pointer, length, Permission::Read);
            guest.Read(pointer, std::as_writable_bytes(std::span(text)));
        }
        state.build(guest, arg(guest, 0), operation, [&](CommandBuffer* host) {
            return Fn(host, length ? text.data() : nullptr, length, conv<std::uint32_t>(arg(guest, 3)));
        });
    }

    // Upstream validates the version and builds its host table; the guest gets
    // a copy of the same data at a guest address.
    template<bool Internal> static void registerDefaults(Impl& state, Machine& guest, Contract) {
        const auto version = conv<std::uint32_t>(arg(guest, 0));
        if constexpr (Internal) sceAgcGetRegisterDefaults2Internal(version);
        else sceAgcGetRegisterDefaults2(version);
        result(guest, state.registerDefaults(guest, version, Internal));
    }

    // Prim state reads only each stage's type and special registers (the Metal
    // stage ABI resolver is a no-op): copy those, clear every other guest pointer.
    struct HostStage {
        Shader Header{};
        ShaderSpecialRegs Specials{};
        const Shader* Load(Machine& guest, std::uint64_t at) {
            if (!at) return nullptr;
            Header = read<Shader>(guest, at);
            const auto specials = numeric(Header.specials);
            Header.user_data = nullptr; Header.code = nullptr; Header.cx_registers = nullptr; Header.sh_registers = nullptr;
            Header.input_semantics = nullptr; Header.output_semantics = nullptr; Header.specials = nullptr;
            if (specials) { Specials = read<ShaderSpecialRegs>(guest, specials); Header.specials = &Specials; }
            return &Header;
        }
    };
    template<std::size_t N> static std::optional<std::array<ShaderRegister, N>> registers(Machine& guest, std::uint64_t at, bool input) {
        if (!at) return std::nullopt;
        address(at, N * sizeof(ShaderRegister), alignof(ShaderRegister));
        guest.CheckAccess(at, N * sizeof(ShaderRegister), input ? Permission::Read | Permission::Write : Permission::Write);
        std::array<ShaderRegister, N> values{};
        if (input) guest.Read(at, std::as_writable_bytes(std::span(values)));
        return values;
    }
    template<std::size_t N> static void store(Machine& guest, std::uint64_t at, const std::optional<std::array<ShaderRegister, N>>& values) {
        if (values) guest.Write(at, std::as_bytes(std::span(*values)));
    }
    static void createPrimState(Impl&, Machine& guest, Contract) {
        const auto cxAt = arg(guest, 0), ucAt = arg(guest, 1);
        auto cx = registers<2>(guest, cxAt, false);
        auto uc = registers<3>(guest, ucAt, false);
        HostStage hs, gs;
        const auto* hsShader = hs.Load(guest, arg(guest, 2));
        const auto* gsShader = cx || uc ? gs.Load(guest, arg(guest, 3)) : nullptr;
        const auto status = sceAgcCreatePrimState(cx ? cx->data() : nullptr, uc ? uc->data() : nullptr, hsShader, gsShader,
                                                  conv<std::uint32_t>(arg(guest, 4)));
        if (status == 0) { store(guest, cxAt, cx); store(guest, ucAt, uc); }
        result(guest, status);
    }
    static void updatePrimState(Impl&, Machine& guest, Contract) {
        const auto cxAt = arg(guest, 0), ucAt = arg(guest, 1);
        auto cx = registers<2>(guest, cxAt, true);
        auto uc = registers<3>(guest, ucAt, true);
        const auto status = sceAgcUpdatePrimState(cx ? cx->data() : nullptr, uc ? uc->data() : nullptr, conv<std::uint32_t>(arg(guest, 2)));
        if (status == 0) { store(guest, cxAt, cx); store(guest, ucAt, uc); }
        result(guest, status);
    }

    static std::span<const Row> Rows() {
        static const Row rows[] = {
            {"23LRUSvYu1M", Contract::UpstreamPacketInfo, &plain<sceAgcInit_0090>},
            {"BfBDZGbti7A", Contract::UpstreamPacketInfo, &trinity<sceAgcGetIsTrinityMode>},
            {"t7PlZ9nt5Lc", Contract::UpstreamPacketSizes, &plain<sceAgcCbNopGetSize>},
            {"bxGoVxpdSPQ", Contract::UpstreamPacketSizes, &plain<sceAgcCbSetShRegisterRangeDirectGetSize>},
            {"JOWmDrl+j20", Contract::UpstreamPacketSizes, &plain<sceAgcCbSetUcRegisterRangeDirectGetSize>},
            {"yUBESvCCJ4I", Contract::UpstreamPacketSizes, &plain<sceAgcCbSetShRegistersDirectGetSize>},
            {"TGEZzUWLbrc", Contract::UpstreamPacketSizes, &plain<sceAgcCbSetUcRegistersDirectGetSize>},
            {"nNlUtdDDvZ0", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetShRegistersIndirectGetSize>},
            {"GBCh3zCihoU", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetCxRegistersIndirectGetSize>},
            {"UQGTw4xRlcM", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetUcRegistersIndirectGetSize>},
            {"H6vHS5cidSA", Contract::UpstreamPacketSizes, &plain<sceAgcDcbContextStateOpGetSize>},
            {"p9tI+yTvx68", Contract::UpstreamPacketSizes, &plain<sceAgcDcbWriteDataGetSize>},
            {"C4l9fB17t8w", Contract::UpstreamPacketSizes, &plain<sceAgcDcbEventWriteGetSize>},
            {"43WJ08sSugE", Contract::UpstreamPacketSizes, &plain<sceAgcDcbWaitOnAddressGetSize>},
            {"idlaArvdXEs", Contract::UpstreamPacketSizes, &plain<sceAgcAcbWaitOnAddressGetSize>},
            {"Lkf86B98qPc", Contract::UpstreamPacketInfo, &packetSize<sceAgcGetPacketSize>},
            {"V++UgBtQhn0", Contract::UpstreamPacketInfo, &payloadAddress<sceAgcGetDataPacketPayloadAddress_0090>},
            {"s+VGAMDQ0AQ", Contract::UpstreamPacketInfo, &payloadRange<sceAgcGetDataPacketPayloadRange>},
            {"n8vgpaQg6dA", Contract::UpstreamPatches, &rangePatch<sceAgcSetRangePredication>},
            {"Y-5vneiBtzk", Contract::UpstreamPacketSizes, &plain<sceAgcAcbEventWriteGetSize>},
            {"w6Dj1VJt5qY", Contract::UpstreamPatches, &patch<sceAgcSetPacketPredication>},
            {"w1KFAHVqpaU", Contract::UpstreamCb, &builder<sceAgcCbBranch>},
            {"7toV+elXqNM", Contract::UpstreamCb, &builder<sceAgcCbCondWrite>},
            {"UZbQjYAwwXM", Contract::UpstreamRegisters, &registersDirect<sceAgcCbSetShRegistersDirect>},
            {"03RZmELWWzw", Contract::UpstreamRegisters, &registersDirect<sceAgcCbSetUcRegistersDirect>},
            {"qj7QZpgr9Uw", Contract::UpstreamDcb, &builder<sceAgcDcbContextStateOp_0100>},
            {"xSAR0LTcRKM", Contract::UpstreamDcb, &builder<sceAgcDcbJump>},
            {"-HOOCn0JY48", Contract::UpstreamRegisters, &builder<sceAgcDcbSetShRegistersIndirect>},
            {"ZvwO9euwYzc", Contract::UpstreamRegisters, &builder<sceAgcDcbSetCxRegistersIndirect>},
            {"hvUfkUIQcOE", Contract::UpstreamRegisters, &builder<sceAgcDcbSetUcRegistersIndirect>},
            {"CtB+A9-VxO0", Contract::UpstreamDcb, &builder<sceAgcDcbDispatchIndirect>},
            {"Yw0jKSqop+E", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndexAuto>},
            {"q88lQ+GP5Yk", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndex>},
            {"B+aG9DUnTKA", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndexOffset>},
            {"1q1titRBL6o", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndirect>},
            {"kUlvghKs-mA", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndirectMulti>},
            {"ypVBz4uPKcQ", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndexIndirectMulti>},
            {"t1vNu082-jM", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndexIndirect>},
            {"Rlx+bykm0r0", Contract::UpstreamDraw, &builder<sceAgcDcbDrawIndexMultiInstanced>},
            {"aJf+j5yntiU", Contract::UpstreamDcb, &builder<sceAgcDcbEventWrite>},
            {"vuSXe69VILM", Contract::UpstreamDcb, &builder<sceAgcDcbGetLodStats>},
            {"RmaJwLtc8rY", Contract::UpstreamDcb, &builder<sceAgcDcbSetBaseIndirectArgs>},
            {"-KRzWekV120", Contract::UpstreamDraw, &builder<sceAgcDcbSetIndexSize_0600>},
            {"l4fM9K-Lyks", Contract::UpstreamDraw, &builder<sceAgcDcbSetIndexBuffer>},
            {"8N2tmT3jmC8", Contract::UpstreamDraw, &builder<sceAgcDcbSetIndexCount>},
            {"tSBxhAPyytQ", Contract::UpstreamDraw, &builder<sceAgcDcbSetNumInstances>},
            {"u2T2DiA5hRI", Contract::UpstreamDcb, &builder<sceAgcDcbStallCommandBufferParser>},
            {"zfcxg-ewMK8", Contract::UpstreamDcb, &builder<sceAgcDcbRewind>},
            {"jt3pl7EN17o", Contract::UpstreamDcb, &builder<sceAgcDcbPrimeUtcl2>},
            {"TRO721eVt4g", Contract::UpstreamDcb, &builder<sceAgcDcbResetQueue>},
            {"bbFueFP+J4k", Contract::UpstreamDcb, &builder<sceAgcDcbSetPredication>},
            {"QhCbS4X9Rl8", Contract::UpstreamMarkers, &marker<sceAgcDcbSetMarker>},
            {"+kSrjIVxKFE", Contract::UpstreamMarkers, &marker<sceAgcDcbPushMarker>},
            {"H7uZqCoNuWk", Contract::UpstreamDcb, &builder<sceAgcDcbPopMarker>},
            {"G0jrLdvEqDw", Contract::UpstreamDcb, &builder<sceAgcDcbMemSemaphore>},
            {"JrtiDtKeS38", Contract::UpstreamAcb, &builder<sceAgcAcbResetQueue>},
            {"q4VuU-QsLOE", Contract::UpstreamAcb, &builder<sceAgcAcbMemSemaphore>},
            {"j3EtxFkSIhQ", Contract::UpstreamAcb, &builder<sceAgcAcbDispatchIndirect>},
            {"xAeBOa0A3kk", Contract::UpstreamMarkers, &marker<sceAgcAcbSetMarker>},
            {"DwICrVxerkY", Contract::UpstreamAcb, &builder<sceAgcAcbRewind>},
            {"szG7hz2yEhA", Contract::UpstreamAcb, &builder<sceAgcAcbPrimeUtcl2>},
            {"cpCILPya5Zk", Contract::UpstreamMarkers, &marker<sceAgcAcbPushMarker>},
            {"e1DFTg+Sd8U", Contract::UpstreamAcb, &builder<sceAgcAcbJump>},
            {"cFazmnXpJOE", Contract::UpstreamAcb, &builder<sceAgcAcbEventWrite>},
            {"6mFxkVqdmbQ", Contract::UpstreamAcb, &builder<sceAgcAcbPopMarker>},
            {"K2mciNVxUCE", Contract::UpstreamCb, &builder<sceAgcSetNop>},
            {"Abendgtz+3o", Contract::UpstreamPacketSizes, &plain<sceAgcCbDispatchGetSize>},
            {"uZW-mqsxkrM", Contract::UpstreamPacketSizes, &plain<sceAgcCbBranchGetSize>},
            {"FuVbkyKlf+s", Contract::UpstreamPacketSizes, &plain<sceAgcCbCondWriteGetSize>},
            {"hL7C0IRpWZI", Contract::UpstreamPacketSizes, &plain<sceAgcCbQueueEndOfPipeActionGetSize>},
            {"QhPDD513V0w", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetShRegisterDirectGetSize>},
            {"1DeUNpRIDDA", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetCxRegisterDirectGetSize>},
            {"aP1Ki9G3++4", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetUcRegisterDirectGetSize>},
            {"-vnlTPPXPrw", Contract::UpstreamPacketSizes, &plain<sceAgcDcbAcquireMemGetSize>},
            {"1tB0xkLNjcw", Contract::UpstreamPacketSizes, &plain<sceAgcDcbAtomicGdsGetSize>},
            {"oz6zQq1JwCE", Contract::UpstreamPacketSizes, &plain<sceAgcDcbAtomicMemGetSize>},
            {"ou16V5hh5sg", Contract::UpstreamPacketSizes, &plain<sceAgcDcbCondExecGetSize>},
            {"VEGu4dixjUg", Contract::UpstreamPacketSizes, &plain<sceAgcDcbJumpGetSize>},
            {"b5u0Jzm8TF8", Contract::UpstreamPacketSizes, &plain<sceAgcDcbCopyDataGetSize>},
            {"2ccJz9LQI+w", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDmaDataGetSize>},
            {"zg6u-N6Otxs", Contract::UpstreamPacketSizes, &plain<sceAgcDcbQueueEndOfShaderActionGetSize>},
            {"w8HVkEeXPv8", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDispatchIndirectGetSize>},
            {"WrdP9Zxx3lQ", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndexAutoGetSize>},
            {"6ee9Hd3EWXQ", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndexGetSize>},
            {"qMlfB1ZhMDc", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndexOffsetGetSize>},
            {"cxPZ4Wgvdj8", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndirectGetSize>},
            {"pYoKs3lPy88", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndirectMultiGetSize>},
            {"r98I08t+LOg", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndexIndirectMultiGetSize>},
            {"mStuvI0zOtc", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndexIndirectGetSize>},
            {"mR9j7+SfM34", Contract::UpstreamPacketSizes, &plain<sceAgcDcbDrawIndexMultiInstancedGetSize>},
            {"rUuVjyR+Rd4", Contract::UpstreamPacketSizes, &plain<sceAgcDcbGetLodStatsGetSize>},
            {"9S4noWrUI0s", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetBaseDispatchIndirectArgsGetSize>},
            {"MMlmJAL7N5w", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetBaseDrawIndirectArgsGetSize>},
            {"ca4KPvp0qLQ", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetIndexSizeGetSize>},
            {"j4emHHndCPY", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetIndexBufferGetSize>},
            {"mljzuGDZRQ4", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetIndexCountGetSize>},
            {"6DFuRKT4C9w", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetNumInstancesGetSize>},
            {"+u6dKSLWM2o", Contract::UpstreamPacketSizes, &plain<sceAgcDcbStallCommandBufferParserGetSize>},
            {"QIXCsbipds0", Contract::UpstreamPacketSizes, &plain<sceAgcDcbRewindGetSize>},
            {"XN+Iuu7XsM8", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetZPassPredicationEnableGetSize>},
            {"yheJGN-ay+A", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetBoolPredicationEnableGetSize>},
            {"vLrBL8DQiz8", Contract::UpstreamPacketSizes, &plain<sceAgcDcbSetPredicationDisableGetSize>},
            {"KjPeVduz6jU", Contract::UpstreamPacketSizes, &plain<sceAgcDcbPrimeUtcl2GetSize>},
            {"F8NLhWvFemI", Contract::UpstreamPacketSizes, &plain<sceAgcAcbQueueEndOfShaderActionGetSize>},
            {"CbQh3DKMSno", Contract::UpstreamPacketSizes, &plain<sceAgcAcbCopyDataGetSize>},
            {"da1Sm8-QDoU", Contract::UpstreamPacketSizes, &plain<sceAgcAcbAtomicMemGetSize>},
            {"PxKWV2fVAps", Contract::UpstreamPacketSizes, &plain<sceAgcAcbDispatchIndirectGetSize>},
            {"0ZOG0jc9nRg", Contract::UpstreamPacketSizes, &plain<sceAgcAcbRewindGetSize>},
            {"ozKzBP4aki4", Contract::UpstreamPacketSizes, &plain<sceAgcAcbCondExecGetSize>},
            {"eCjKaqeeQ5s", Contract::UpstreamPacketSizes, &plain<sceAgcAcbPrimeUtcl2GetSize>},
            {"M0ttm8h7SKA", Contract::UpstreamPacketSizes, &plain<sceAgcAcbDmaDataGetSize>},
            {"hcIxS8pmXF4", Contract::UpstreamPacketSizes, &plain<sceAgcAcbAtomicGdsGetSize>},
            {"b-oySn+G2tE", Contract::UpstreamPacketSizes, &plain<sceAgcAcbJumpGetSize>},
            {"ewobAQeMo5k", Contract::UpstreamPacketSizes, &plain<sceAgcAcbAcquireMemGetSize>},
            {"whb1RL7K4Ss", Contract::UpstreamPatches, &patch<sceAgcSetCxRegIndirectPatchSetNumRegisters>},
            {"fPSCdQxgpSw", Contract::UpstreamPatches, &patch<sceAgcWriteDataPatchSetAddressOrOffset>},
            {"6lNcCp+fxi4", Contract::UpstreamPatches, &patch<sceAgcSetUcRegIndirectPatchSetAddress>},
            {"eAy8eGNsCuU", Contract::UpstreamPatches, &patch<sceAgcWriteDataPatchSetCachePolicy>},
            {"n485EBnIWmk", Contract::UpstreamPatches, &patch<sceAgcWaitRegMemPatchCompareFunction>},
            {"J8YCgfKAMQs", Contract::UpstreamPatches, &patch<sceAgcQueueEndOfPipeActionPatchGcrCntl>},
            {"0fWWK5uG9rQ", Contract::UpstreamPatches, &patch<sceAgcQueueEndOfPipeActionPatchAddress>},
            {"vcmNN+AAXnY", Contract::UpstreamPatches, &patch<sceAgcSetCxRegIndirectPatchSetAddress>},
            {"y5K5tPktiL8", Contract::UpstreamPatches, &patch<sceAgcAsyncWriteDataPatchSetCachePolicy>},
            {"vRoArM9zaIk", Contract::UpstreamPatches, &patch<sceAgcSetUcRegIndirectPatchAddRegisters>},
            {"GXBlM-ekzrI", Contract::UpstreamPatches, &patch<sceAgcBranchPatchSetCompareAddress>},
            {"Ikfdt-rIqCE", Contract::UpstreamPatches, &patch<sceAgcJumpPatchSetTarget_0300>},
            {"MlEw1feXcjg", Contract::UpstreamPatches, &patch<sceAgcQueueEndOfPipeActionPatchData>},
            {"EJBA4dbmvfg", Contract::UpstreamPatches, &patch<sceAgcAsyncWriteDataPatchSetDst>},
            {"3KDcnM3lrcU", Contract::UpstreamPatches, &patch<sceAgcWaitRegMemPatchAddress>},
            {"tmy-+rBpspY", Contract::UpstreamPatches, &patch<sceAgcWriteDataPatchSetDst>},
            {"d-6uF9sZDIU", Contract::UpstreamPatches, &patch<sceAgcSetCxRegIndirectPatchAddRegisters>},
            {"k-JpyR2dYAM", Contract::UpstreamPatches, &patchRelative<sceAgcAsyncCondExecPatchSetEnd>},
            {"fRG-JOH5+sI", Contract::UpstreamPatches, &patch<sceAgcSetUcRegIndirectPatchSetNumRegisters>},
            {"3ZWa3AoyWZQ", Contract::UpstreamPatches, &patch<sceAgcAsyncCondExecPatchSetCommandAddress>},
            {"YWTKOju587o", Contract::UpstreamPatches, &patch<sceAgcCondExecPatchSetCommandAddress>},
            {"T9fjQIINoeE", Contract::UpstreamPatches, &patch<sceAgcQueueEndOfPipeActionPatchType>},
            {"ORWsxIbk4TE", Contract::UpstreamPatches, &patchRelative<sceAgcCondExecPatchSetEnd>},
            {"cdDRpqcFGbU", Contract::UpstreamPatches, &patch<sceAgcDmaDataPatchSetSrcAddressOrOffsetOrImmediate>},
            {"eWaWyFegzgQ", Contract::UpstreamPatches, &patch<sceAgcAsyncRewindPatchSetRewindState>},
            {"d4NZIlguzv0", Contract::UpstreamPatches, &patch<sceAgcAsyncWriteDataPatchSetAddressOrOffset>},
            {"ziVA3whp3p4", Contract::UpstreamPatches, &patch<sceAgcRewindPatchSetRewindState>},
            {"z2duB-hHQSM", Contract::UpstreamPatches, &patch<sceAgcSetShRegIndirectPatchAddRegisters>},
            {"Qrj4c+61z4A", Contract::UpstreamPatches, &patch<sceAgcSetShRegIndirectPatchSetAddress>},
            {"7nOoijNPvEU", Contract::UpstreamPatches, &patch<sceAgcWaitRegMemPatchReference>},
            {"IxYiarKlXxM", Contract::UpstreamPatches, &patch<sceAgcDmaDataPatchSetDstAddressOrOffset>},
            {"nCUgItdN2ms", Contract::UpstreamPatches, &patch<sceAgcSetShRegIndirectPatchSetNumRegisters>},
            {"W0WEyog0f74", Contract::UpstreamPatches, &patch<sceAgcAcquireMemSetEngine>},
            {"7Wa3aeJgeVU", Contract::UpstreamPatches, &patch<sceAgcBranchPatchSetThenTarget_0300>},
            {"rP5xLdOf26k", Contract::UpstreamPatches, &patch<sceAgcBranchPatchSetElseTarget_0300>},
            {"BYcSvEsINWU", Contract::UpstreamMarkers, &markerSpan<sceAgcDcbSetMarkerSpan>},
            {"N7M3+N-sBCw", Contract::UpstreamMarkers, &markerSpan<sceAgcDcbPushMarkerSpan>},
            {"8Kly1JrJUlw", Contract::UpstreamMarkers, &markerSpan<sceAgcAcbPushMarkerSpan>},
            {"pxx-GoOSdw4", Contract::UpstreamMarkers, &markerSpan<sceAgcAcbSetMarkerSpan>},
            {"ms1xVoZ-Vwc", Contract::UpstreamPacketSizes, &plain<sceAgcDcbBeginOcclusionQueryGetSize>},
            {"P1CugZ99Uzc", Contract::UpstreamPacketSizes, &plain<sceAgcDcbEndOcclusionQueryGetSize>},
            {"RTpj-tIlvZc", Contract::UpstreamPacketInfo, &plain<sceAgcSetShaderInstrumentation>},
            {"SwI6QxqwAC0", Contract::UpstreamPacketInfo, &plain<sceAgcGetShaderInstrumentation>},
            {"2JtWUUiYBXs", Contract::UpstreamShader, &registerDefaults<false>},
            {"wRbq6ZjNop4", Contract::UpstreamShader, &registerDefaults<true>},
            {"D9sr1xGUriE", Contract::UpstreamShader, &createPrimState},
            {"Y3ymLfZ1384", Contract::UpstreamShader, &updatePrimState},
        };
        return rows;
    }
};
bool GenericContractKnown(AgcAbiContract contract) {
    if (contract == Contract::UpstreamDriver) return true;
    const auto rows = AgcGenericBindings::Rows();
    return std::any_of(rows.begin(), rows.end(), [contract](const auto& row) { return row.Operation == contract; });
}

SceAgcImports::SceAgcImports(Machine& machine, SceAgcBackend backend, std::span<const Contract> admitted, std::uint64_t base) :
    impl(std::make_shared<Impl>(machine, std::move(backend), admitted, base)) {}
SceAgcImports::SceAgcImports(Machine& machine, SceAgcBackend backend,
    std::span<const AgcAbiAdmission> admissions, std::uint64_t base) {
    std::vector<Contract> contracts;
    for (const auto& admission : admissions) {
        const auto policy = admission.Policy;
        const auto operation = admission.Contract;
        const bool valid = policy == AgcArgumentPolicy::SourceContract ||
            (policy == AgcArgumentPolicy::TargetPacket20Bit && (operation == Contract::SubmitDcbPacket16 ||
                operation == Contract::AgrSubmitDcbPacket16 || operation == Contract::SubmitAcbPacket16)) ||
            (policy == AgcArgumentPolicy::TargetMemoryWrite && (operation == Contract::DcbWriteDataCommandBuffer56 ||
                operation == Contract::AcbWriteDataCommandBuffer56)) ||
            (policy == AgcArgumentPolicy::TargetShader && operation == Contract::CreateShaderRelativeHeader96) ||
            (policy == AgcArgumentPolicy::TargetDispatch && operation == Contract::CbDispatchCommandBuffer56) ||
            (policy == AgcArgumentPolicy::TargetSuspend && operation == Contract::SuspendPoint) ||
            (policy == AgcArgumentPolicy::TargetFlip && operation == Contract::DcbSetFlipCommandBuffer56) ||
            (policy == AgcArgumentPolicy::TargetRenderingWait && operation == Contract::DcbWaitUntilSafeForRenderingCommandBuffer56) ||
            (policy == AgcArgumentPolicy::TargetGraphicsEvent &&
                (operation == Contract::AddEqEvent || operation == Contract::DeleteEqEvent));
        require(valid && !admission.Evidence.empty(), "invalid target ABI admission descriptor");
        require(std::find(contracts.begin(), contracts.end(), operation) == contracts.end(), "duplicate target ABI admission descriptor");
        contracts.push_back(operation);
    }
    impl = std::make_shared<Impl>(machine, std::move(backend), contracts, base);
    for (const auto& admission : admissions)
        if (admission.Policy != AgcArgumentPolicy::SourceContract) impl->Policies.emplace(admission.Contract, admission.Policy);
}

std::span<const AgcAbiAdmission> TargetAgcAdmissions() {
    // Title-agnostic: the same contracts apply to every importing image.
    // Bounded static target inspection, corroborated by the existing public
    // builders/backend and independent TCG/Metal fixtures. These descriptions
    // contain engineering conclusions, never private instruction/binary bytes.
    static constexpr AgcAbiAdmission admissions[] = {
        {Contract::SubmitDcbPacket16, AgcArgumentPolicy::TargetPacket20Bit,
            "target stack packet: pointer64, words32<=0xfffff, flags8=0; native snapshots before return"},
        {Contract::AgrSubmitDcbPacket16, AgcArgumentPolicy::TargetPacket20Bit,
            "same target packet branch/return boundary; queue-zero native snapshot route only"},
        {Contract::SubmitAcbPacket16, AgcArgumentPolicy::TargetPacket20Bit,
            "target queue32 and stack packet16; native bounded compute queues and snapshot ownership"},
        {Contract::DcbWriteDataCommandBuffer56, AgcArgumentPolicy::TargetMemoryWrite,
            "target cursor56 and stack controls; dst4/5 memory, advance0, confirmation bit, capacity checked before writes"},
        {Contract::AcbWriteDataCommandBuffer56, AgcArgumentPolicy::TargetMemoryWrite,
            "target cursor56, dst2 memory, advance0, confirmation bit; scalar widths and stack controls corroborated"},
        {Contract::CreateShaderRelativeHeader96, AgcArgumentPolicy::TargetShader,
            "target three pointer64 arguments, code256 alignment, field-relative64 metadata; bounded atomic publication"},
        {Contract::CbDispatchCommandBuffer56, AgcArgumentPolicy::TargetDispatch,
            "target descriptor64/dimensions32/modifier32; bit0 forwarding path, sufficient-capacity cursor56"},
        {Contract::SuspendPoint, AgcArgumentPolicy::TargetSuspend,
            "target no-argument tail call; source/native queue-zero suspend acceptance boundary only"},
        {Contract::AddEqEvent, AgcArgumentPolicy::TargetGraphicsEvent,
            "target numeric handle64 from CreateEqueue, selector32=0, opaque udata64=0; source graphics filter -14/EV_ADD|EV_CLEAR, genuine owner mailbox required"},
        {Contract::DeleteEqEvent, AgcArgumentPolicy::TargetGraphicsEvent,
            "target same numeric handle64/selector32=0 before DeleteEqueue; source owner withdraws subscription and pending receipts, invalid ownership remains fail-closed"},
        {Contract::DcbSetFlipCommandBuffer56, AgcArgumentPolicy::TargetFlip,
            "target cursor56/opaque VideoOut handle32/index32 in three registered slots/mode1/arg64=0; source internal six-word encoding and cursor24"},
        {Contract::DcbWaitUntilSafeForRenderingCommandBuffer56, AgcArgumentPolicy::TargetRenderingWait,
            "target same cursor56/opaque VideoOut handle32/index32 in three registered slots; source internal four-word reuse wait before flip reservation"},
        // Source-contract admissions: arguments are validated by the builder or
        // submit path itself and failures return SCE codes or null packets.
        {Contract::SubmitMultiDcbs, AgcArgumentPolicy::SourceContract,
            "pointer64[] and words32[] arrays, count<=1024, each entry words<=0xfffff, SCE error before any submission"},
        {Contract::AgrSubmitMultiDcbs, AgcArgumentPolicy::SourceContract, "same multi-submit arrays and bounds; queue zero"},
        {Contract::SubmitMultiAcbs, AgcArgumentPolicy::SourceContract, "queue32 in 0x20..0x57 plus the multi-submit arrays and bounds"},
        {Contract::DcbSetShRegisterDirect, AgcArgumentPolicy::SourceContract, "cursor56 and packed offset16/value32; three-word packet"},
        {Contract::DcbSetCxRegisterDirect, AgcArgumentPolicy::SourceContract, "cursor56 and packed offset16/value32; three-word packet"},
        {Contract::DcbSetUcRegisterDirect, AgcArgumentPolicy::SourceContract, "cursor56 and packed offset16/value32; three-word packet"},
        {Contract::DcbAcquireMem, AgcArgumentPolicy::SourceContract, "upstream builder; engine/cbDbOp/gcr bit checks, poll on stack"},
        {Contract::AcbAcquireMem, AgcArgumentPolicy::SourceContract, "upstream builder; compute engine acquire"},
        {Contract::CbReleaseMem, AgcArgumentPolicy::SourceContract, "upstream builder; six stack arguments, selector and label checks"},
        {Contract::DcbWaitRegMem, AgcArgumentPolicy::SourceContract, "upstream builder; three stack arguments, compare/size checks"},
        {Contract::AcbWaitRegMem, AgcArgumentPolicy::SourceContract, "upstream builder; two stack arguments, compare/size checks"},
        {Contract::DcbCopyData, AgcArgumentPolicy::SourceContract, "upstream builder; three stack arguments"},
        {Contract::AcbCopyData, AgcArgumentPolicy::SourceContract, "upstream builder; three stack arguments"},
        {Contract::DcbDmaData, AgcArgumentPolicy::SourceContract, "upstream builder; six stack arguments"},
        {Contract::AcbDmaData, AgcArgumentPolicy::SourceContract, "upstream builder; four stack arguments"},
        {Contract::DcbAtomicMem, AgcArgumentPolicy::SourceContract, "upstream builder; two stack arguments, address alignment by op"},
        {Contract::AcbAtomicMem, AgcArgumentPolicy::SourceContract, "upstream builder; two stack arguments, address alignment by op"},
        {Contract::DcbCondExec, AgcArgumentPolicy::SourceContract, "upstream builder; aligned address, dwords<=0x3fff"},
        {Contract::AcbCondExec, AgcArgumentPolicy::SourceContract, "upstream builder; aligned address, dwords<=0x3fff"},
        {Contract::CbSetShRegisterRangeDirect, AgcArgumentPolicy::SourceContract, "upstream builder; bounded guest value snapshot"},
        {Contract::CbSetUcRegisterRangeDirect, AgcArgumentPolicy::SourceContract, "upstream builder; bounded guest value snapshot"},
        {Contract::UpstreamPacketSizes, AgcArgumentPolicy::SourceContract, "upstream size queries; numeric arguments only"},
        {Contract::UpstreamPatches, AgcArgumentPolicy::SourceContract, "upstream patchers; packet copied from guest, patched on the host, written back"},
        {Contract::UpstreamMarkers, AgcArgumentPolicy::SourceContract, "upstream marker encoders; bounded guest C string snapshot"},
        {Contract::UpstreamRegisters, AgcArgumentPolicy::SourceContract, "upstream register encoders; bounded guest table snapshot or numeric indirect address"},
        {Contract::UpstreamDraw, AgcArgumentPolicy::SourceContract, "upstream draw and index encoders; numeric addresses are encoded, never dereferenced"},
        {Contract::UpstreamAcb, AgcArgumentPolicy::SourceContract, "upstream compute queue encoders; numeric addresses only"},
        {Contract::UpstreamCb, AgcArgumentPolicy::SourceContract, "upstream common encoders (branch, conditional write, nop); numeric addresses only"},
        {Contract::UpstreamDcb, AgcArgumentPolicy::SourceContract, "upstream graphics queue encoders (jump, flow, state, events); numeric addresses only"},
        {Contract::UpstreamPacketInfo, AgcArgumentPolicy::SourceContract, "upstream packet inspection; packet copied from guest, results translated back"},
        {Contract::UpstreamShader, AgcArgumentPolicy::SourceContract,
            "upstream register defaults published as guest tables; prim state on host copies of stage type and special registers"},
        {Contract::UpstreamDriver, AgcArgumentPolicy::SourceContract,
            "upstream driver leaf exports via prx bridge descriptors; integer outputs written back, discarded pointers never read"},
        {Contract::AcbSetFlipCommandBuffer56, AgcArgumentPolicy::SourceContract,
            "compute cursor56/VideoOut handle32/index32/mode32/arg64; Metal driver flip packet, registry checked on submission"},
        {Contract::AcbWaitUntilSafeForRenderingCommandBuffer56, AgcArgumentPolicy::SourceContract,
            "compute cursor56/VideoOut handle32/index32; Metal driver rendering wait packet, registry checked on submission"}
    };
    return admissions;
}
SceAgcImports::~SceAgcImports() = default;

std::uint64_t SceAgcImports::Resolve(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize) {
    require(symbolType == 2 && symbolSize == 0, "only function symbols with size zero are admitted");
    Contract operation{};
    bool driver = false;
    Impl::Handler handler = nullptr;
    const UpstreamPrx::Export* leaf = nullptr;
    if (const auto found = std::find_if(std::begin(bindings), std::end(bindings), [&](const auto& row) { return import.Nid == row.Nid; });
        found != std::end(bindings)) {
        operation = found->Operation; driver = found->Driver;
    } else {
        const auto rows = AgcGenericBindings::Rows();
        const auto generic = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return import.Nid == row.Nid; });
        if (generic != rows.end()) {
            operation = generic->Operation; handler = generic->Run;
        } else if ((leaf = UpstreamPrx::AgcDriverLeafExport(import.Nid))) {
            operation = Contract::UpstreamDriver; driver = true;
        } else {
            throw std::runtime_error("Unsupported SCE AGC service: " + identity(import));
        }
    }
    const auto name = driver ? "libSceAgcDriver" : "libSceAgc";
    if (import.LibraryName != name || import.ModuleName != name || import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE AGC scope/version: " + identity(import));
    if (!impl->Admitted.contains(operation)) throw std::runtime_error("Unqualified SCE AGC target ABI contract: " + identity(import));
    // A qualified ABI cannot manufacture backend capability. In particular,
    // event imports stay unresolved until the genuine CPU equeue owner exists.
    switch (operation) {
    case Contract::SubmitDcbPacket16: case Contract::AgrSubmitDcbPacket16: case Contract::SubmitAcbPacket16:
    case Contract::SubmitMultiDcbs: case Contract::AgrSubmitMultiDcbs: case Contract::SubmitMultiAcbs:
        required(impl->Backend.Submit); break;
    case Contract::CreateShaderRelativeHeader96: required(impl->Backend.RegisterShader); break;
    case Contract::SuspendPoint: required(impl->Backend.Suspend); break;
    case Contract::AddEqEvent: required(impl->Backend.AddEvent); break;
    case Contract::DeleteEqEvent: required(impl->Backend.DeleteEvent); break;
    default: break;
    }
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleId};
    if (const auto found=impl->Gates.find(key); found != impl->Gates.end()) return found->second;
    const auto gate = impl->Trampolines->Add([weak=std::weak_ptr<Impl>(impl), operation, handler, leaf](Machine& guest) {
        const auto state=weak.lock();
        require(static_cast<bool>(state), "import runtime expired");
        if (leaf) { state->Active = nullptr; leaf->Call(guest, leaf->Name); return; }
        state->Active = handler;
        if (handler) handler(*state, guest, operation); else state->invoke(guest, operation);
    });
    impl->Gates.emplace(key, gate);
    return gate;
}
}
