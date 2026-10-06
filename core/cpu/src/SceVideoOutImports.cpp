#include <cpu/SceVideoOutImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Cpu {
namespace {

enum class Service { Open, Close, OutputStatus, RegisterBuffers, SetAttribute, FlipRate, Unregister };

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

struct SceVideoOutImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    SceVideoOutBackend backend;
    std::uint64_t base;
    std::size_t nextSlot = 0;
    std::map<Key, std::uint64_t> gates;
    const std::map<std::string, Service> services{
        {"Up36PTk687E", Service::Open}, {"uquVH4-Du78", Service::Close},
        {"utPrVdxio-8", Service::OutputStatus}, {"rKBUtgRrtbk", Service::RegisterBuffers},
        {"PjS5uASwcV8", Service::SetAttribute}, {"CBiu4mCE1DA", Service::FlipRate},
        {"N5KDtkIjjJ4", Service::Unregister}};

    Impl(Machine& guest, SceVideoOutBackend callbacks, std::uint64_t gateBase) :
        machine(guest), backend(std::move(callbacks)), base(gateBase) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE VideoOut gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        const auto fourth = guest.Get(Register::Rcx);
        const auto fifth = guest.Get(Register::R8);
        const auto sixth = guest.Get(Register::R9);
        switch (service) {
        case Service::Open: {
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
            if (result.Result >= 0) guest.Write(second, std::as_bytes(std::span(&result.Status, 1)));
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
            alignedAddress(fourth, alignof(SceVideoOutBuffer));
            std::vector<SceVideoOutBuffer> buffers(static_cast<std::size_t>(count));
            guest.CheckAccess(fourth, buffers.size() * sizeof(SceVideoOutBuffer), Permission::Read);
            guest.Read(fourth, std::as_writable_bytes(std::span(buffers)));
            for (const auto& buffer : buffers) {
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
            if (signedInt(second) < 0 || signedInt(second) >= 4)
                throw std::runtime_error("SCE VideoOut invalid buffer set index");
            required(backend.UnregisterBuffers, "sceVideoOutUnregisterBuffers");
            returnInt(guest, backend.UnregisterBuffers(signedInt(first), signedInt(second)));
            break;
        }
    }
};

SceVideoOutImports::SceVideoOutImports(Machine& machine, SceVideoOutBackend backend, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, std::move(backend), gateBase)) {}
SceVideoOutImports::~SceVideoOutImports() = default;

std::uint64_t SceVideoOutImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libSceVideoOut" || import.ModuleName != "libSceVideoOut" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE VideoOut import scope/version: " + identity(import));
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end())
        throw std::runtime_error("Unsupported SCE VideoOut import service: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->nextSlot == 256) throw std::runtime_error("SCE VideoOut import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const auto operation = service->second;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("Unsupported SCE VideoOut service: runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
