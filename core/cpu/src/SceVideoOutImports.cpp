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
static_assert(sizeof(SceVideoOutFlipStatus) == 128 && offsetof(SceVideoOutFlipStatus, FlipPendingNum) == 52);
static_assert(offsetof(SceVideoOutFlipStatus, SubmitProcessTimeCounter) == 64);
static_assert(sizeof(SceVideoOutVblankStatus) == 40 && offsetof(SceVideoOutVblankStatus, Flags) == 32);
static_assert(offsetof(SceVideoOutResolutionStatus, RefreshRate) == 16 &&
              offsetof(SceVideoOutResolutionStatus, Flags) == 28 && offsetof(SceVideoOutResolutionStatus, Reserved1) == 32);
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

// sceVideoOutRegisterBuffers (v1) attribute: 32-bit format, no DCC fields.
struct VideoOutAttributeV1 {
    std::uint32_t PixelFormat;
    std::uint32_t TilingMode;
    std::uint32_t AspectRatio;
    std::uint32_t Width;
    std::uint32_t Height;
    std::uint32_t PitchInPixel;
    std::uint32_t Option;
    std::uint32_t Reserved0;
    std::uint64_t Reserved1;
};
static_assert(sizeof(VideoOutAttributeV1) == 40 && offsetof(VideoOutAttributeV1, Reserved1) == 32);

// The 32-byte kernel event record as written by sceKernelWaitEqueue.
struct KernelEventView {
    std::uint64_t Ident;
    std::int16_t Filter;
    std::uint16_t Flags;
    std::uint32_t Fflags;
    std::int64_t Data;
    std::uint64_t Udata;
};
static_assert(sizeof(KernelEventView) == 32 && offsetof(KernelEventView, Data) == 16);
constexpr std::int16_t VideoOutFilter = -13;

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
        {"N5KDtkIjjJ4", Service::Unregister}, {"U46NwOiJpys", Service::SubmitFlip},
        {"HXzjK9yI30k", Service::AddFlipEvent}, {"-Ozn0F1AFRg", Service::DeleteFlipEvent},
        {"Xru92wHJRmg", Service::AddVblankEvent}, {"oNOQn3knW6s", Service::DeleteVblankEvent},
        {"SbU3dwp80lQ", Service::GetFlipStatus}, {"1FZBKy8HeNU", Service::GetVblankStatus},
        {"j6RaAUlaLv0", Service::WaitVblank}, {"zgXifHT9ErY", Service::IsFlipPending},
        {"6kPnj51T62Y", Service::GetResolutionStatus}, {"w0hLuNarQxY", Service::ConfigureOutput},
        {"MTxxrOCeSig", Service::SetWindowModeMargins}, {"w3BY+tAEiQY", Service::RegisterBuffersV1},
        {"U2JJtSqNKZI", Service::GetEventId}, {"rWUTcKdkUzQ", Service::GetEventData},
        {"Mt4QHHkxkOc", Service::GetEventCount}};

    Impl(Machine& guest, SceVideoOutBackend callbacks, std::uint64_t gateBase,
         std::span<const VideoOutAbiAdmission> admissions) :
        machine(guest), backend(std::move(callbacks)), base(gateBase) {
        for (const auto& admission : admissions) {
            if (admission.Evidence.empty() ||
                static_cast<unsigned>(admission.Contract) > static_cast<unsigned>(Service::GetEventCount))
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

    // A status is written only on success, and only its defined bytes.
    template<class TStatus> static void writeStatus(Machine& guest, std::uint64_t address, std::int32_t result,
                                                    const TStatus& status, std::size_t bytes = sizeof(TStatus)) {
        if (result == 0) guest.Write(address, std::as_bytes(std::span(&status, 1)).first(bytes));
        returnInt(guest, result);
    }

    // Null output is an SCE error; a non-null output must be writable before
    // the backend runs, so a fault never follows a state change.
    static bool statusOutput(Machine& guest, std::uint64_t address, std::size_t alignment, std::size_t bytes) {
        if (!address) { returnInt(guest, VideoOutError::InvalidAddress); return false; }
        alignedAddress(address, alignment);
        guest.CheckAccess(address, bytes, Permission::Write);
        return true;
    }

    // Register row contract: reserved pointers clear, 64 KiB aligned data.
    static std::int32_t validRows(std::span<const SceVideoOutBuffer> rows) {
        for (const auto& buffer : rows) {
            if (buffer.Reserved[0] || buffer.Reserved[1]) return VideoOutError::InvalidValue;
            if (!buffer.DataAddress || buffer.DataAddress % 65536) return VideoOutError::InvalidAddress;
        }
        return 0;
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
                if (param->FirstWord != 16 || param->SetPriority > 1 || param->SetAffinity > 1 ||
                    (param->SetPriority && (param->Priority < 256 || param->Priority > 767)))
                    return returnInt(guest, VideoOutError::InvalidValue);
                if (param->SetAffinity) {
                    if (fourth > std::numeric_limits<std::uint64_t>::max() - 16)
                        throw std::runtime_error("SCE VideoOut open parameter address overflow");
                    guest.CheckAccess(fourth + 16, sizeof(param->Affinity), Permission::Read);
                    guest.Read(fourth + 16, std::as_writable_bytes(std::span(&param->Affinity, 1)));
                    if (!param->Affinity || (param->Affinity & ~std::uint64_t{0x1fff}))
                        return returnInt(guest, VideoOutError::InvalidValue);
                }
            }
            if ((signedInt(first) != 255 && signedInt(first) != 0) || signedInt(second) < 0 ||
                signedInt(second) > 2 || signedInt(third) != 0)
                return returnInt(guest, VideoOutError::InvalidValue);
            required(backend.Open, "sceVideoOutOpen");
            returnInt(guest, backend.Open(signedInt(first), signedInt(second), signedInt(third), param));
            break;
        }
        case Service::Close:
            required(backend.Close, "sceVideoOutClose");
            returnInt(guest, backend.Close(signedInt(first)));
            break;
        case Service::OutputStatus: {
            if (!statusOutput(guest, second, alignof(SceVideoOutStatus), sizeof(SceVideoOutStatus))) break;
            required(backend.GetOutputStatus, "sceVideoOutGetOutputStatus");
            const auto result = backend.GetOutputStatus(signedInt(first));
            writeStatus(guest, second, result.Result, result.Status);
            break;
        }
        case Service::RegisterBuffers: {
            const auto stack = stackArguments(guest);
            const auto set = signedInt(second);
            const auto start = signedInt(third);
            const auto count = signedInt(fifth);
            const auto category = signedInt(stack[0]);
            // Generic shapes: any set, any 1-16 buffer run, tiled or linear, any format.
            if (set < 0 || set >= 4 || start < 0 || start >= 16 || count < 1 || count > 16 || start + count > 16)
                return returnInt(guest, VideoOutError::InvalidValue);
            if (category != 0 && category != 1) return returnInt(guest, VideoOutError::InvalidCategory);
            if (stack[1] || !sixth) return returnInt(guest, VideoOutError::InvalidOption);
            if (!fourth) return returnInt(guest, VideoOutError::InvalidAddress);
            const auto attribute = read<SceVideoOutAttribute>(guest, sixth);
            alignedAddress(fourth, alignof(SceVideoOutBuffer));
            std::vector<SceVideoOutBuffer> buffers(static_cast<std::size_t>(count));
            guest.CheckAccess(fourth, buffers.size() * sizeof(SceVideoOutBuffer), Permission::Read);
            guest.Read(fourth, std::as_writable_bytes(std::span(buffers)));
            if (const auto invalid = validRows(buffers)) return returnInt(guest, invalid);
            required(backend.RegisterBuffers, "sceVideoOutRegisterBuffers2");
            returnInt(guest, backend.RegisterBuffers(signedInt(first), set, start, buffers, attribute, category));
            break;
        }
        case Service::RegisterBuffersV1: {
            // (handle, start, const void* const* addresses, count, const SceVideoOutBufferAttribute*)
            const auto start = signedInt(second);
            const auto count = signedInt(fourth);
            if (start < 0 || start >= 16 || count < 1 || count > 16 || start + count > 16)
                return returnInt(guest, VideoOutError::InvalidValue);
            if (!third || !fifth) return returnInt(guest, VideoOutError::InvalidAddress);
            const auto legacy = read<VideoOutAttributeV1>(guest, fifth);
            if (legacy.AspectRatio || legacy.Reserved0 || legacy.Reserved1)
                return returnInt(guest, VideoOutError::InvalidValue);
            // v1 32-bit formats with a v2 64-bit equivalent (A8R8G8B8 / A8B8G8R8 sRGB).
            std::uint64_t format = 0;
            if (legacy.PixelFormat == 0x80000000u) format = 0x8000000000000000ULL;
            else if (legacy.PixelFormat == 0x80002200u) format = 0x8000000022000000ULL;
            else return returnInt(guest, VideoOutError::InvalidPixelFormat);
            alignedAddress(third, alignof(std::uint64_t));
            std::vector<std::uint64_t> addresses(static_cast<std::size_t>(count));
            guest.CheckAccess(third, addresses.size() * sizeof(std::uint64_t), Permission::Read);
            guest.Read(third, std::as_writable_bytes(std::span(addresses)));
            std::vector<SceVideoOutBuffer> buffers;
            for (const auto address : addresses) buffers.push_back({address, 0, {}});
            if (const auto invalid = validRows(buffers)) return returnInt(guest, invalid);
            SceVideoOutAttribute attribute;
            attribute.TilingMode = legacy.TilingMode;
            attribute.Width = legacy.Width;
            attribute.Height = legacy.Height;
            attribute.PitchInPixel = legacy.PitchInPixel;
            attribute.Option = legacy.Option;
            attribute.PixelFormat = format;
            required(backend.RegisterBuffers, "sceVideoOutRegisterBuffers");
            // v1 names no attribute set: take the first free one and return its index.
            for (std::int32_t set = 0; set < 4; ++set) {
                const auto result = backend.RegisterBuffers(signedInt(first), set, start, buffers, attribute, 0);
                if (result == VideoOutError::InvalidIndex) continue;
                return returnInt(guest, result == 0 ? set : result);
            }
            returnInt(guest, VideoOutError::NoEmptySlot);
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
            if (signedInt(second) < 0 || signedInt(second) > 2) return returnInt(guest, VideoOutError::InvalidValue);
            required(backend.SetFlipRate, "sceVideoOutSetFlipRate");
            returnInt(guest, backend.SetFlipRate(signedInt(first), signedInt(second)));
            break;
        case Service::Unregister:
            if (signedInt(second) < 0 || signedInt(second) >= 4) return returnInt(guest, VideoOutError::InvalidIndex);
            required(backend.UnregisterBuffers, "sceVideoOutUnregisterBuffers");
            returnInt(guest, backend.UnregisterBuffers(signedInt(first), signedInt(second)));
            break;
        case Service::SubmitFlip:
            // Every mode presents at the next vsync; HSYNC/window variants only change tearing.
            if (signedInt(third) < 1 || signedInt(third) > 6) return returnInt(guest, VideoOutError::InvalidValue);
            if (signedInt(second) < -2 || signedInt(second) >= 16) return returnInt(guest, VideoOutError::InvalidIndex);
            required(backend.SubmitFlip, "sceVideoOutSubmitFlip");
            returnInt(guest, backend.SubmitFlip(signedInt(first), signedInt(second), signedInt(third),
                                                static_cast<std::int64_t>(fourth)));
            break;
        case Service::AddFlipEvent: case Service::AddVblankEvent:
            if (!first) return returnInt(guest, VideoOutError::InvalidEventQueue);
            required(backend.AddEvent, "sceVideoOutAddEvent");
            returnInt(guest, backend.AddEvent(first, signedInt(second),
                service == Service::AddFlipEvent ? VideoOutEventFlip : VideoOutEventVblank, third));
            break;
        case Service::DeleteFlipEvent: case Service::DeleteVblankEvent:
            if (!first) return returnInt(guest, VideoOutError::InvalidEventQueue);
            required(backend.DeleteEvent, "sceVideoOutDeleteEvent");
            returnInt(guest, backend.DeleteEvent(first, signedInt(second),
                service == Service::DeleteFlipEvent ? VideoOutEventFlip : VideoOutEventVblank));
            break;
        case Service::GetFlipStatus: {
            if (!statusOutput(guest, second, alignof(SceVideoOutFlipStatus), sizeof(SceVideoOutFlipStatus))) break;
            required(backend.GetFlipStatus, "sceVideoOutGetFlipStatus");
            const auto result = backend.GetFlipStatus(signedInt(first));
            writeStatus(guest, second, result.Result, result.Status);
            break;
        }
        case Service::GetVblankStatus: {
            if (!statusOutput(guest, second, alignof(SceVideoOutVblankStatus), sizeof(SceVideoOutVblankStatus))) break;
            required(backend.GetVblankStatus, "sceVideoOutGetVblankStatus");
            const auto result = backend.GetVblankStatus(signedInt(first));
            writeStatus(guest, second, result.Result, result.Status);
            break;
        }
        case Service::GetResolutionStatus: {
            constexpr std::size_t defined = offsetof(SceVideoOutResolutionStatus, Reserved1) + 12;
            if (!statusOutput(guest, second, alignof(SceVideoOutResolutionStatus), defined)) break;
            required(backend.GetResolutionStatus, "sceVideoOutGetResolutionStatus");
            const auto result = backend.GetResolutionStatus(signedInt(first));
            writeStatus(guest, second, result.Result, result.Status, defined);
            break;
        }
        case Service::WaitVblank:
            required(backend.WaitVblank, "sceVideoOutWaitVblank");
            returnInt(guest, backend.WaitVblank(signedInt(first)));
            break;
        case Service::IsFlipPending:
            required(backend.IsFlipPending, "sceVideoOutIsFlipPending");
            returnInt(guest, backend.IsFlipPending(signedInt(first)));
            break;
        case Service::ConfigureOutput: {
            // (handle, mode, const SceVideoOutOutputOptions*, void* reserved, u64 reserved)
            if (fourth || fifth) return returnInt(guest, VideoOutError::InvalidValue);
            if (third) {
                const auto options = read<std::array<std::uint32_t, 16>>(guest, third);
                for (const auto word : options) if (word) return returnInt(guest, VideoOutError::InvalidOption);
            }
            required(backend.ConfigureOutput, "sceVideoOutConfigureOutput");
            returnInt(guest, backend.ConfigureOutput(signedInt(first), second));
            break;
        }
        case Service::SetWindowModeMargins:
            required(backend.SetWindowModeMargins, "sceVideoOutSetWindowModeMargins");
            returnInt(guest, backend.SetWindowModeMargins(signedInt(first), signedInt(second), signedInt(third)));
            break;
        case Service::GetEventId: case Service::GetEventData: case Service::GetEventCount: {
            if (!first || (service == Service::GetEventData && !second))
                return returnInt(guest, VideoOutError::InvalidAddress);
            const auto event = read<KernelEventView>(guest, first);
            if (event.Filter != VideoOutFilter) return returnInt(guest, VideoOutError::InvalidEvent);
            const auto data = static_cast<std::uint64_t>(event.Data);
            if (service == Service::GetEventCount)
                return returnInt(guest, static_cast<std::int32_t>((data >> 12u) & 0xfu));
            if (service == Service::GetEventId) {
                if (event.Ident != 0 && event.Ident != 1 && event.Ident != 2 && event.Ident != 8)
                    return returnInt(guest, VideoOutError::InvalidEvent);
                return returnInt(guest, static_cast<std::int32_t>(event.Ident));
            }
            // Payload is data bits 63:16; a flip argument keeps its sign.
            auto payload = data >> 16u;
            if (event.Ident == static_cast<std::uint64_t>(VideoOutEventFlip) && (data & 0x8000000000000000ULL))
                payload |= 0xffff000000000000ULL;
            alignedAddress(second, alignof(std::int64_t));
            guest.CheckAccess(second, sizeof(std::int64_t), Permission::Write);
            guest.Write(second, std::as_bytes(std::span(&payload, 1)));
            returnInt(guest, 0);
            break;
        }
        }
    }

    bool hasCallback(Service service) const {
        switch (service) {
        case Service::Open: return bool(backend.Open);
        case Service::Close: return bool(backend.Close);
        case Service::OutputStatus: return bool(backend.GetOutputStatus);
        case Service::RegisterBuffers: case Service::RegisterBuffersV1: return bool(backend.RegisterBuffers);
        case Service::FlipRate: return bool(backend.SetFlipRate);
        case Service::Unregister: return bool(backend.UnregisterBuffers);
        case Service::SubmitFlip: return bool(backend.SubmitFlip);
        case Service::AddFlipEvent: case Service::AddVblankEvent: return bool(backend.AddEvent);
        case Service::DeleteFlipEvent: case Service::DeleteVblankEvent: return bool(backend.DeleteEvent);
        case Service::GetFlipStatus: return bool(backend.GetFlipStatus);
        case Service::GetVblankStatus: return bool(backend.GetVblankStatus);
        case Service::WaitVblank: return bool(backend.WaitVblank);
        case Service::IsFlipPending: return bool(backend.IsFlipPending);
        case Service::GetResolutionStatus: return bool(backend.GetResolutionStatus);
        case Service::ConfigureOutput: return bool(backend.ConfigureOutput);
        case Service::SetWindowModeMargins: return bool(backend.SetWindowModeMargins);
        case Service::SetAttribute: case Service::GetEventId: case Service::GetEventData: case Service::GetEventCount:
            return true;
        }
        return false;
    }

    std::uint64_t resolve(const SceImport& import, bool target) {
        if (import.LibraryName != "libSceVideoOut" || import.ModuleName != "libSceVideoOut" ||
            import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
            throw std::runtime_error("Unsupported SCE VideoOut import scope/version: " + identity(import));
        const auto service = services.find(import.Nid);
        if (service == services.end())
            throw std::runtime_error("Unsupported SCE VideoOut import service: " + identity(import));
        if (target) {
            if (!admitted.contains(service->second))
                throw std::runtime_error("Unsupported SCE VideoOut target ABI: no qualified admission");
            if (!hasCallback(service->second))
                throw std::runtime_error("Unsupported SCE VideoOut service without native backend: " + identity(import));
        }
        const Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                      import.LibraryVersion, import.ModuleMajor, import.ModuleMinor, target};
        if (const auto found = gates.find(key); found != gates.end()) return found->second;
        if (nextSlot == 256) throw std::runtime_error("SCE VideoOut import gate page is exhausted");
        const auto gate = base + nextSlot * 16;
        const std::array ret{std::byte{0xc3}};
        machine.Write(gate, ret);
        machine.AddHostCall(gate, [state = weak_from_this(), operation = service->second](Machine& guest) {
            const auto context = state.lock();
            if (!context) throw std::runtime_error("Unsupported SCE VideoOut service: runtime has expired");
            context->invoke(guest, operation);
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

std::span<const VideoOutAbiAdmission> TargetVideoOutAdmissions() {
    // Title-agnostic: every contract validates its public ABI generically for
    // any importing image (any buffer count 1-16, tiled or linear, any set).
    static constexpr VideoOutAbiAdmission admissions[] = {
        {Service::Open, "public ABI: open(user 0/255, bus 0-2, index 0, optional param16/24); signed EAX"},
        {Service::Close, "public ABI: close(handle); signed EAX"},
        {Service::OutputStatus, "public ABI: status48 written only on EAX==0"},
        {Service::RegisterBuffers, "public ABI: register2(set 0-3, start/count within 16, stride32, category 0/1)"},
        {Service::SetAttribute, "public ABI: attribute80, u64 format, u32 extents, any tiling/DCC"},
        {Service::FlipRate, "public ABI: flip rate 0-2; signed EAX"},
        {Service::Unregister, "public ABI: unregister(handle, set 0-3); signed EAX"},
        {Service::SubmitFlip, "public ABI: submitFlip(handle, index -2..15, mode 1-6, i64 arg)"},
        {Service::AddFlipEvent, "public ABI: addFlipEvent(equeue, handle, udata) -> EVFILT_VIDEO_OUT ident 0"},
        {Service::DeleteFlipEvent, "public ABI: deleteFlipEvent(equeue, handle)"},
        {Service::AddVblankEvent, "public ABI: addVblankEvent(equeue, handle, udata) -> EVFILT_VIDEO_OUT ident 1"},
        {Service::DeleteVblankEvent, "public ABI: deleteVblankEvent(equeue, handle)"},
        {Service::GetFlipStatus, "public ABI: flipStatus128 written only on EAX==0"},
        {Service::GetVblankStatus, "public ABI: vblankStatus40 written only on EAX==0"},
        {Service::WaitVblank, "public ABI: waitVblank(handle) blocks until the next vblank"},
        {Service::IsFlipPending, "public ABI: isFlipPending(handle) -> pending flip count"},
        {Service::GetResolutionStatus, "public ABI: resolutionStatus44 written only on EAX==0"},
        {Service::ConfigureOutput, "public ABI: configureOutput(handle, mode, options64, null, 0)"},
        {Service::SetWindowModeMargins, "public ABI: setWindowModeMargins(handle, top, bottom)"},
        {Service::RegisterBuffersV1, "public ABI: register(start, u64 addresses, count, attribute40) -> set index"},
        {Service::GetEventId, "public ABI: getEventId(event32) for EVFILT_VIDEO_OUT"},
        {Service::GetEventData, "public ABI: getEventData(event32, i64*) payload bits 63:16"},
        {Service::GetEventCount, "public ABI: getEventCount(event32) bits 15:12"}
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
