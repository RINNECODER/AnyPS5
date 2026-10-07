#include <cpu/SceVideoOutImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Cpu {
namespace {

using Service = VideoOutAbiContract;

static_assert(sizeof(SceVideoOutOpenParam) == 24 && offsetof(SceVideoOutOpenParam, Affinity) == 16);
static_assert(sizeof(SceVideoOutAttribute) == 80 && alignof(SceVideoOutAttribute) == 8);
static_assert(offsetof(SceVideoOutAttribute, Option) == 24 && offsetof(SceVideoOutAttribute, PixelFormat) == 32);
static_assert(offsetof(SceVideoOutAttribute, DccClearColor) == 40 && offsetof(SceVideoOutAttribute, DccControl) == 48);
static_assert(offsetof(SceVideoOutAttribute, Reserved1) == 56);
static_assert(sizeof(SceVideoOutBuffer) == 32 && offsetof(SceVideoOutBuffer, Reserved) == 16);
static_assert(sizeof(SceVideoOutStatus) == 48 && offsetof(SceVideoOutStatus, RefreshRate) == 8);
static_assert(offsetof(SceVideoOutStatus, Flags) == 16 && offsetof(SceVideoOutStatus, Reserved) == 24);
static_assert(std::endian::native == std::endian::little);

std::int32_t signedInt(std::uint64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

void alignedAddress(std::uint64_t address, std::size_t alignment) {
    if (!address || address % alignment)
        throw std::runtime_error("SCE VideoOut invalid guest address/alignment");
}

template<class TValue> TValue read(Machine& guest, std::uint64_t address) {
    alignedAddress(address, alignof(TValue));
    guest.CheckAccess(address, sizeof(TValue), Permission::Read);
    TValue value{};
    guest.Read(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}

std::array<std::uint64_t, 2> stackArguments(Machine& guest) {
    const auto stack = guest.Get(Register::Rsp);
    if (stack > std::numeric_limits<std::uint64_t>::max() - 8)
        throw std::runtime_error("SCE VideoOut stack argument address overflow");
    return read<std::array<std::uint64_t, 2>>(guest, stack + 8);
}

template<class TCallback> void required(const TCallback& callback, const char* service) {
    if (!callback)
        throw std::runtime_error(std::string("Unsupported SCE VideoOut service without native backend: ") + service);
}

void returnInt(Machine& guest, std::int32_t result) {
    guest.Set(Register::Rax, static_cast<std::uint32_t>(result));
}

}

struct SceVideoOutImports::Impl : std::enable_shared_from_this<SceVideoOutImports::Impl> {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t, bool>;
    Machine& machine;
    SceVideoOutBackend backend;
    std::uint64_t base;
    std::size_t nextSlot = 0;
    std::map<Key, std::uint64_t> gates;
    std::set<Service> admitted;
    const std::map<std::string, Service> services{
        {"Up36PTk687E", Service::Open}, {"uquVH4-Du78", Service::Close},
        {"utPrVdxio-8", Service::OutputStatus}, {"rKBUtgRrtbk", Service::RegisterBuffers},
        {"PjS5uASwcV8", Service::SetAttribute}, {"CBiu4mCE1DA", Service::FlipRate},
        {"N5KDtkIjjJ4", Service::Unregister}};

    Impl(Machine& guest, SceVideoOutBackend callbacks, std::uint64_t gateBase,
         std::span<const VideoOutAbiAdmission> admissions) :
        machine(guest), backend(std::move(callbacks)), base(gateBase) {
        for (const auto& admission : admissions) {
            if (admission.Evidence.empty() || static_cast<unsigned>(admission.Contract) > static_cast<unsigned>(Service::Unregister))
                throw std::invalid_argument("SCE VideoOut invalid target admission descriptor");
            admitted.insert(admission.Contract);
        }
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE VideoOut gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

    void invoke(Machine& guest, Service service, bool target) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        const auto fourth = guest.Get(Register::Rcx);
        const auto fifth = guest.Get(Register::R8);
        const auto sixth = guest.Get(Register::R9);
        switch (service) {
        case Service::Open: {
            if (target && (signedInt(first) != 255 || signedInt(second) != 0 || signedInt(third) != 0 || fourth))
                throw std::runtime_error("SCE VideoOut open outside qualified target use");
            std::optional<SceVideoOutOpenParam> param;
            if (fourth) {
                guest.CheckAccess(fourth, 16, Permission::Read);
                param.emplace();
                guest.Read(fourth, std::as_writable_bytes(std::span(&*param, 1)).first(16));
                if (param->FirstWord != 16 || param->SetPriority > 1 || param->SetAffinity > 1)
                    throw std::runtime_error("SCE VideoOut invalid open parameters");
                if (param->SetPriority && (param->Priority < 256 || param->Priority > 767))
                    throw std::runtime_error("SCE VideoOut invalid service thread priority");
                if (param->SetAffinity) {
                    if (fourth > std::numeric_limits<std::uint64_t>::max() - 16)
                        throw std::runtime_error("SCE VideoOut open parameter address overflow");
                    guest.CheckAccess(fourth + 16, sizeof(param->Affinity), Permission::Read);
                    guest.Read(fourth + 16, std::as_writable_bytes(std::span(&param->Affinity, 1)));
                    if (!param->Affinity || (param->Affinity & ~std::uint64_t{0x1fff}))
                        throw std::runtime_error("SCE VideoOut invalid service thread affinity");
                }
            }
            if ((signedInt(first) != 255 && signedInt(first) != 0) || signedInt(second) < 0 ||
                signedInt(second) > 2 || signedInt(third) != 0)
                throw std::runtime_error("SCE VideoOut invalid open user, bus or index");
            required(backend.Open, "sceVideoOutOpen");
            returnInt(guest, backend.Open(signedInt(first), signedInt(second), signedInt(third), param));
            break;
        }
        case Service::Close:
            required(backend.Close, "sceVideoOutClose");
            returnInt(guest, backend.Close(signedInt(first)));
            break;
        case Service::OutputStatus: {
            alignedAddress(second, alignof(SceVideoOutStatus));
            guest.CheckAccess(second, sizeof(SceVideoOutStatus), Permission::Write);
            required(backend.GetOutputStatus, "sceVideoOutGetOutputStatus");
            const auto result = backend.GetOutputStatus(signedInt(first));
            if (result.Result == 0) guest.Write(second, std::as_bytes(std::span(&result.Status, 1)));
            returnInt(guest, result.Result);
            break;
        }
        case Service::RegisterBuffers: {
            const auto stack = stackArguments(guest);
            const auto set = signedInt(second);
            const auto start = signedInt(third);
            const auto count = signedInt(fifth);
            const auto category = signedInt(stack[0]);
            if (set < 0 || set >= 4 || start < 0 || start >= 16 || count < 1 || count > 16 || start + count > 16)
                throw std::runtime_error("SCE VideoOut invalid buffer set, slot or count");
            if (category != 0 && category != 1)
                throw std::runtime_error("SCE VideoOut invalid buffer category");
            if (stack[1]) throw std::runtime_error("Unsupported SCE VideoOut register buffer option");
            const auto attribute = read<SceVideoOutAttribute>(guest, sixth);
            if (target && (set != 0 || start != 0 || count != 3 || category != 0 ||
                attribute.TilingMode != 0 || attribute.PitchInPixel != 0 || attribute.Option != 0 ||
                attribute.DccControl != 0 || attribute.DccClearColor != 0 ||
                (attribute.PixelFormat != 0x8000000000000000ULL && attribute.PixelFormat != 0x8000000022000000ULL)))
                throw std::runtime_error("SCE VideoOut registration outside qualified target use (HDR format 0x8100070422000000 unsupported)");
            alignedAddress(fourth, alignof(SceVideoOutBuffer));
            std::vector<SceVideoOutBuffer> buffers(static_cast<std::size_t>(count));
            guest.CheckAccess(fourth, buffers.size() * sizeof(SceVideoOutBuffer), Permission::Read);
            guest.Read(fourth, std::as_writable_bytes(std::span(buffers)));
            for (const auto& buffer : buffers) {
                if (target && buffer.MetadataAddress)
                    throw std::runtime_error("SCE VideoOut target compressed backing is unqualified");
                if (buffer.Reserved[0] || buffer.Reserved[1])
                    throw std::runtime_error("SCE VideoOut reserved buffer pointers are set");
                alignedAddress(buffer.DataAddress, 65536);
            }
            required(backend.RegisterBuffers, "sceVideoOutRegisterBuffers2");
            returnInt(guest, backend.RegisterBuffers(signedInt(first), set, start, buffers, attribute, category));
            break;
        }
        case Service::SetAttribute: {
            const auto stack = stackArguments(guest);
            if (target && (static_cast<std::uint32_t>(third) != 0 ||
                !static_cast<std::uint32_t>(fourth) || static_cast<std::uint32_t>(fourth) > 16384 ||
                !static_cast<std::uint32_t>(fifth) || static_cast<std::uint32_t>(fifth) > 16384 || sixth ||
                static_cast<std::uint32_t>(stack[0]) || stack[1] ||
                (second != 0x8000000000000000ULL && second != 0x8000000022000000ULL)))
                throw std::runtime_error("SCE VideoOut attribute outside qualified target use (HDR format 0x8100070422000000 unsupported)");
            alignedAddress(first, alignof(SceVideoOutAttribute));
            guest.CheckAccess(first, sizeof(SceVideoOutAttribute), Permission::Write);
            SceVideoOutAttribute attribute;
            attribute.TilingMode = static_cast<std::uint32_t>(third);
            attribute.Width = static_cast<std::uint32_t>(fourth);
            attribute.Height = static_cast<std::uint32_t>(fifth);
            attribute.Option = sixth;
            attribute.PixelFormat = second;
            attribute.DccControl = static_cast<std::uint32_t>(stack[0]);
            attribute.DccClearColor = stack[1];
            guest.Write(first, std::as_bytes(std::span(&attribute, 1)));
            break;
        }
        case Service::FlipRate:
            if (signedInt(second) < 0 || signedInt(second) > 2)
                throw std::runtime_error("SCE VideoOut invalid flip rate");
            required(backend.SetFlipRate, "sceVideoOutSetFlipRate");
            returnInt(guest, backend.SetFlipRate(signedInt(first), signedInt(second)));
            break;
        case Service::Unregister:
            if (target && signedInt(second) != 0)
                throw std::runtime_error("SCE VideoOut unregister outside qualified target group");
            if (signedInt(second) < 0 || signedInt(second) >= 4)
                throw std::runtime_error("SCE VideoOut invalid buffer set index");
            required(backend.UnregisterBuffers, "sceVideoOutUnregisterBuffers");
            returnInt(guest, backend.UnregisterBuffers(signedInt(first), signedInt(second)));
            break;
        }
    }

    std::uint64_t resolve(const SceImport& import, bool target) {
        if (import.LibraryName != "libSceVideoOut" || import.ModuleName != "libSceVideoOut" ||
            import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1 ||
            (target && (import.LibraryId != 39 || import.ModuleId != 40)))
            throw std::runtime_error("Unsupported SCE VideoOut import scope/version: " + identity(import));
        const auto service = services.find(import.Nid);
        if (service == services.end())
            throw std::runtime_error("Unsupported SCE VideoOut import service: " + identity(import));
        if (target) {
            if (!admitted.contains(service->second))
                throw std::runtime_error("Unsupported SCE VideoOut target ABI: no qualified admission");
            switch (service->second) {
            case Service::Open: required(backend.Open, "sceVideoOutOpen"); break;
            case Service::Close: required(backend.Close, "sceVideoOutClose"); break;
            case Service::OutputStatus: required(backend.GetOutputStatus, "sceVideoOutGetOutputStatus"); break;
            case Service::RegisterBuffers: required(backend.RegisterBuffers, "sceVideoOutRegisterBuffers2"); break;
            case Service::FlipRate: required(backend.SetFlipRate, "sceVideoOutSetFlipRate"); break;
            case Service::Unregister: required(backend.UnregisterBuffers, "sceVideoOutUnregisterBuffers"); break;
            case Service::SetAttribute: break;
            }
        }
        const Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                      import.LibraryVersion, import.ModuleMajor, import.ModuleMinor, target};
        if (const auto found = gates.find(key); found != gates.end()) return found->second;
        if (nextSlot == 256) throw std::runtime_error("SCE VideoOut import gate page is exhausted");
        const auto gate = base + nextSlot * 16;
        const std::array ret{std::byte{0xc3}};
        machine.Write(gate, ret);
        machine.AddHostCall(gate, [state = weak_from_this(), operation = service->second, target](Machine& guest) {
            const auto context = state.lock();
            if (!context) throw std::runtime_error("Unsupported SCE VideoOut service: runtime has expired");
            context->invoke(guest, operation, target);
        });
        gates.emplace(key, gate);
        ++nextSlot;
        return gate;
    }
};

SceVideoOutImports::SceVideoOutImports(Machine& machine, SceVideoOutBackend backend, std::uint64_t gateBase,
                                     std::span<const VideoOutAbiAdmission> admissions) :
    impl(std::make_shared<Impl>(machine, std::move(backend), gateBase, admissions)) {}
SceVideoOutImports::~SceVideoOutImports() = default;

std::span<const VideoOutAbiAdmission> QualifiedVideoOutAdmissionsForImage(std::string_view verifiedSha256) {
    if (verifiedSha256 != "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397") return {};
    // Bounded target caller/field inference against the pinned public candidate.
    // Both 8-bit public display formats are admitted; the observed HDR branch
    // needs its exact color/scanout semantics and remains explicitly excluded.
    static constexpr VideoOutAbiAdmission admissions[] = {
        {Service::Open, "eboot:6d8ecd,959cb4,95a349,Open255/main/index0/null; signed EAX"},
        {Service::Close, "eboot:6d8f06,959d37,95a3a3,973163,opaque int32 handle; EAX ignored"},
        {Service::OutputStatus, "eboot:6d8ee2,959cc7,95a398,status48; +0/+4 only on EAX==0"},
        {Service::RegisterBuffers, "eboot:972f8e,group0/start0/count3/stride32/category0/null"},
        {Service::SetAttribute, "eboot:972eec,attribute80/u64-format/u32-extent/tiling0/noDCC"},
        {Service::FlipRate, "eboot:972fa7,helper60->0/30->1/20->2; signed EAX"},
        {Service::Unregister, "eboot:973158,opaque int32 handle/group0; EAX ignored"}
    };
    return admissions;
}

std::uint64_t SceVideoOutImports::Resolve(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize) {
    if (symbolType != 2 || symbolSize != 0)
        throw std::runtime_error("Unsupported SCE VideoOut ELF symbol: observed function type 2 and size zero required");
    return impl->resolve(import, true);
}
std::uint64_t SceVideoOutImports::ResolvePublicFixture(const SceImport& import) {
    return impl->resolve(import, false);
}

}
