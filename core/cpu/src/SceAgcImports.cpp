#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include "SceShaders.hpp"
#include "prx/libSceAgc/Shader/include/ShaderUtils.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace Cpu {
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
    {"MWiElSNE8j8", Contract::DcbWaitUntilSafeForRenderingCommandBuffer56, false}
};
struct GuestPacket { std::uint64_t Address; std::uint32_t Words; std::uint8_t Flags; std::array<std::uint8_t,3> Reserved; };
struct GuestCommandBuffer {
    std::uint64_t Bottom, Top, Up, Down, Callback, UserData;
    std::uint32_t ReservedWords, Padding;
};
static_assert(sizeof(GuestPacket) == 16 && sizeof(GuestCommandBuffer) == 56);
static_assert(sizeof(Shader) == 96 && offsetof(Shader, header_size) == 64 && offsetof(Shader, type) == 90);
static_assert(std::endian::native == std::endian::little);

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

struct SceAgcImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::uint16_t>;
    Machine& MachineRef;
    SceAgcBackend Backend;
    std::set<Contract> Admitted;
    std::map<Contract, AgcArgumentPolicy> Policies;
    std::uint64_t Base;
    std::size_t Slots = 0;
    std::map<Key, std::uint64_t> Gates;
    Impl(Machine& machine, SceAgcBackend backend, std::span<const Contract> admitted, std::uint64_t base) :
        MachineRef(machine), Backend(std::move(backend)), Admitted(admitted.begin(), admitted.end()), Base(base) {
        require(base && !(base & 4095) && base < 0x7ffffffff000, "gates require aligned low canonical guest page");
        for (auto contract : Admitted)
            require(std::any_of(std::begin(bindings), std::end(bindings), [contract](const auto& row) { return row.Operation == contract; }),
                    "unknown ABI admission contract");
        std::array<std::byte,4096> trap;
        trap.fill(std::byte{0xcc});
        MachineRef.Map(Base, trap.size(), Permission::Read | Permission::Write);
        MachineRef.Write(Base, trap);
        MachineRef.Protect(Base, trap.size(), Permission::Read | Permission::Execute);
    }

    std::uint64_t emit(Machine& guest, std::uint64_t bufferAddress, std::span<const std::uint32_t> words) {
        const auto buffer = read<GuestCommandBuffer>(guest, bufferAddress);
        require(((buffer.Bottom | buffer.Top | buffer.Up | buffer.Down) & 3) == 0 && buffer.Bottom &&
                buffer.Bottom <= buffer.Up && buffer.Up <= buffer.Down && buffer.Down <= buffer.Top,
                "invalid command buffer cursor bounds");
        require(words.size() <= 0x4001 && (buffer.Down - buffer.Up) / 4 >= words.size() + buffer.ReservedWords,
                "command buffer exhausted; guest allocation callbacks are unsupported");
        // Validate both writes before modifying either. Only the cursor changes,
        // so guest callback, user data, reserved words and padding stay intact.
        address(buffer.Up, words.size_bytes(), 4);
        guest.CheckAccess(buffer.Up, words.size_bytes(), Permission::Write);
        guest.CheckAccess(bufferAddress + offsetof(GuestCommandBuffer, Up), 8, Permission::Write);
        require(buffer.Up + words.size_bytes() <= bufferAddress || bufferAddress + sizeof(buffer) <= buffer.Up,
                "command output overlaps command buffer descriptor");
        guest.Write(buffer.Up, std::as_bytes(words));
        write(guest, bufferAddress + offsetof(GuestCommandBuffer, Up), buffer.Up + words.size_bytes());
        return buffer.Up;
    }

    void submit(Machine& guest, std::uint64_t pointer, std::uint32_t queue, Contract operation) {
        required(Backend.Submit);
        const auto packet = read<GuestPacket>(guest, pointer);
        require(!packet.Flags, "nonzero command submission flags are unsupported");
        if (Policies.contains(operation))
            require(packet.Words <= 0xfffff, "target packet count outside qualified bound");
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
        require(count <= 1024, "multi submission count outside supported bound");
        address(addresses, std::size_t(count) * 8, 8);
        address(sizes, std::size_t(count) * 4, 4);
        guest.CheckAccess(addresses, std::size_t(count) * 8, Permission::Read);
        guest.CheckAccess(sizes, std::size_t(count) * 4, Permission::Read);
        std::vector<std::uint64_t> pointers(count);
        std::vector<std::uint32_t> lengths(count);
        guest.Read(addresses, std::as_writable_bytes(std::span(pointers)));
        guest.Read(sizes, std::as_writable_bytes(std::span(lengths)));
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

    void invoke(Machine& guest, Contract operation) {
        const auto a=guest.Get(Register::Rdi), b=guest.Get(Register::Rsi), c=guest.Get(Register::Rdx),
            d=guest.Get(Register::Rcx), e=guest.Get(Register::R8), f=guest.Get(Register::R9);
        switch (operation) {
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
            guest.Set(Register::Rax, emit(guest, a, words)); return;
        }
        case Contract::DcbSetShRegisterDirect: case Contract::DcbSetCxRegisterDirect: case Contract::DcbSetUcRegisterDirect: {
            const auto offset=static_cast<std::uint32_t>(b), value=static_cast<std::uint32_t>(b >> 32);
            require(offset <= 0xffff, "invalid direct register offset");
            const auto opcode = operation == Contract::DcbSetShRegisterDirect ? 0x76u : operation == Contract::DcbSetCxRegisterDirect ? 0x69u : 0x79u;
            const std::array words{0xc0010000u | (opcode << 8), offset, value};
            guest.Set(Register::Rax, emit(guest, a, words)); return;
        }
        case Contract::CbDispatchCommandBuffer56: {
            const auto modifier=static_cast<std::uint32_t>(e);
            require(!(modifier & ~(0xa038u | 0x41u)), "invalid dispatch modifier");
            if (Policies.contains(operation)) require(modifier & 1, "dispatch modifier outside qualified target forwarding path");
            const std::array words{0xc0031500u, static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(c), static_cast<std::uint32_t>(d), modifier | 0x41u};
            guest.Set(Register::Rax, emit(guest, a, words)); return;
        }
        case Contract::DcbSetFlipCommandBuffer56: {
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
            guest.Set(Register::Rax, emit(guest, a, words)); return;
        }
        case Contract::DcbWaitUntilSafeForRenderingCommandBuffer56: {
            const auto handle = static_cast<std::uint32_t>(b);
            const auto index = static_cast<std::uint32_t>(c);
            if (Policies.contains(operation)) {
                require(handle > 0 && handle <= std::numeric_limits<std::int32_t>::max() && index < 3,
                        "rendering wait arguments outside qualified target use");
            }
            const std::array words{AgcDriver::RenderingWaitPacketHeader, handle, index, 0u};
            guest.Set(Register::Rax, emit(guest, a, words)); return;
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
        }
        throw std::runtime_error("SCE AGC: unhandled ABI contract");
    }
};

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

std::span<const AgcAbiAdmission> QualifiedAgcAdmissionsForImage(std::string_view verifiedSha256) {
    if (verifiedSha256 != "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397") return {};
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
            "target same cursor56/opaque VideoOut handle32/index32 in three registered slots; source internal four-word reuse wait before flip reservation"}
    };
    return admissions;
}
SceAgcImports::~SceAgcImports() = default;

std::uint64_t SceAgcImports::Resolve(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize) {
    require(symbolType == 2 && symbolSize == 0, "only function symbols with size zero are admitted");
    const auto binding = std::find_if(std::begin(bindings), std::end(bindings), [&](const auto& row) { return import.Nid == row.Nid; });
    if (binding == std::end(bindings)) throw std::runtime_error("Unsupported SCE AGC service: " + identity(import));
    const auto name = binding->Driver ? "libSceAgcDriver" : "libSceAgc";
    if (import.LibraryName != name || import.ModuleName != name || import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE AGC scope/version: " + identity(import));
    if (!impl->Admitted.contains(binding->Operation)) throw std::runtime_error("Unqualified SCE AGC target ABI contract: " + identity(import));
    // A qualified ABI cannot manufacture backend capability. In particular,
    // event imports stay unresolved until the genuine CPU equeue owner exists.
    switch (binding->Operation) {
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
    require(impl->Slots < 256, "import gate page exhausted");
    const auto gate = impl->Base + impl->Slots * 16;
    const std::array ret{std::byte{0xc3}};
    impl->MachineRef.Write(gate, ret);
    impl->MachineRef.AddHostCall(gate, [weak=std::weak_ptr<Impl>(impl), operation=binding->Operation](Machine& guest) {
        const auto state=weak.lock();
        require(static_cast<bool>(state), "import runtime expired");
        state->invoke(guest, operation);
    });
    impl->Gates.emplace(key, gate); ++impl->Slots;
    return gate;
}
}
