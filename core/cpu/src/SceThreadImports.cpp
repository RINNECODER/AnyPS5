#include <cpu/SceThreadImports.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <array>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service { Dtors, AtexitCount, AtexitReport, Create, Yield, Join, Self, Equal, Error, Tls, Exit, AttrInit, AttrDestroy, AttrSetPriority, AttrGetPriority, AttrSetInherit, AttrSetPolicy,
    AttrSetStackSize, AttrGetStackSize, AttrSetStackAddress, AttrGetStackAddress, AttrSetGuardSize, AttrGetGuardSize,
    AttrSetDetachState, AttrGetDetachState, AttrSetAffinity, AttrGetAffinity, AttrGetPolicy, AttrGetInherit,
    AttrSetStack, AttrGetStack, AttrGet, Detach, GetName, Rename, SetPriority, GetPriority, SetAffinity, GetAffinity,
    Cancel, TestCancel, SetCancelState, SetCancelType, ThreadId };
constexpr std::uint32_t Invalid = 0x80020016;
// FreeBSD PTHREAD_CANCELED.
constexpr std::uint64_t Canceled = 1;

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

void checkIndex(Machine& guest, std::uint64_t address) {
    if (!address || address > std::numeric_limits<std::uint64_t>::max() - 16)
        throw std::runtime_error("Invalid SCE thread TLS index span");
    guest.CheckAccess(address, 16, Permission::Read);
}

}

struct SceThreadImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    SceHostTrampolines trampolines;
    const std::map<std::string, Service> services{
        {"rNhWz+lvOMU", Service::Dtors}, {"pB-yGZ2nQ9o", Service::AtexitCount},
        {"WhCc1w3EhSI", Service::AtexitReport}, {"6UgtwV+0zb4", Service::Create},
        {"T72hz6ffq08", Service::Yield}, {"onNY9Byn-W8", Service::Join},
        {"aI+OeCz8xrQ", Service::Self}, {"3PtV6p3QNX4", Service::Equal},
        {"9BcDykPmo1I", Service::Error}, {"vNe1w4diLCs", Service::Tls},
        {"3kg7rT0NQIs", Service::Exit}};
    const std::map<std::string, Service> priorityServices{
        {"nsYoNRywwNg", Service::AttrInit}, {"62KCwEMmzcM", Service::AttrDestroy},
        {"DzES9hQF4f4", Service::AttrSetPriority}, {"FXPWHNk8Of0", Service::AttrGetPriority},
        {"eXbUSpEaTsA", Service::AttrSetInherit}, {"4+h9EzwKF4I", Service::AttrSetPolicy},
        {"UTXzJbWhhTE", Service::AttrSetStackSize}, {"-fA+7ZlGDQs", Service::AttrGetStackSize},
        {"F+yfmduIBB8", Service::AttrSetStackAddress}, {"Ru36fiTtJzA", Service::AttrGetStackAddress},
        {"El+cQ20DynU", Service::AttrSetGuardSize}, {"txHtngJ+eyc", Service::AttrGetGuardSize},
        {"-Wreprtu0Qs", Service::AttrSetDetachState}, {"JaRMy+QcpeU", Service::AttrGetDetachState},
        {"3qxgM4ezETA", Service::AttrSetAffinity}, {"8+s5BzZjxSg", Service::AttrGetAffinity},
        {"NMyIQ9WgWbU", Service::AttrGetPolicy}, {"lpMP8HhkBbg", Service::AttrGetInherit},
        {"Bvn74vj6oLo", Service::AttrSetStack}, {"-quPa4SEJUw", Service::AttrGetStack},
        {"x1X76arYMxU", Service::AttrGet}, {"4qGrR6eoP9Y", Service::Detach},
        {"How7B8Oet6k", Service::GetName}, {"GBUY7ywdULE", Service::Rename},
        {"W0Hpm2X0uPE", Service::SetPriority}, {"1tKyG7RlMJo", Service::GetPriority},
        {"bt3CTBKmGyI", Service::SetAffinity}, {"rcrVFJsQWRY", Service::GetAffinity},
        {"qBDmpCyGssE", Service::Cancel}, {"LapIb799SSE", Service::TestCancel},
        {"OAmWq+OHSjw", Service::SetCancelState}, {"sCJd99Phct0", Service::SetCancelType},
        {"EI-5-jlq2dE", Service::ThreadId}};
    // libkernel/libScePosix POSIX names for the same objects (errno results).
    const std::map<std::string, Service> posixServices{
        {"OxhIB8LB-PQ", Service::Create}, {"h9CcP3J0oVM", Service::Join}, {"EotR8a3ASf4", Service::Self},
        {"6XG4B33N09g", Service::Yield}, {"7Xl257M4VNI", Service::Equal}, {"FJrT5LuUBAU", Service::Exit},
        {"wtkt-teR1so", Service::AttrInit}, {"zHchY8ft5pk", Service::AttrDestroy},
        {"euKRgm0Vn2M", Service::AttrSetPriority}, {"qlk9pSLsUmM", Service::AttrGetPriority},
        {"7ZlAakEf0Qg", Service::AttrSetInherit}, {"oLjPqUKhzes", Service::AttrGetInherit},
        {"JarMIy8kKEY", Service::AttrSetPolicy}, {"RtLRV-pBTTY", Service::AttrGetPolicy},
        {"2Q0z6rnBrTE", Service::AttrSetStackSize}, {"0qOtCR-ZHck", Service::AttrGetStackSize},
        {"suCrEbr0xIQ", Service::AttrSetStackAddress}, {"DxmIMUQ-wXY", Service::AttrGetStackAddress},
        {"JKyG3SWyA10", Service::AttrSetGuardSize}, {"JNkVVsVDmOk", Service::AttrGetGuardSize},
        {"E+tyo3lp5Lw", Service::AttrSetDetachState}, {"VUT1ZSrHT0I", Service::AttrGetDetachState},
        {"-SrbXpGR1f0", Service::AttrSetStack}, {"vQm4fDEsWi8", Service::AttrGetStack},
        {"Ucsu-OK+els", Service::AttrGet}, {"+U1R4WtXvoc", Service::Detach},
        {"9vyP6Z7bqzc", Service::Rename}, {"0D4-FVvEikw", Service::Cancel}, {"nYBrkGDqxh8", Service::TestCancel},
        {"lZzFeSxPl08", Service::SetCancelState}, {"2dEhvvjlq30", Service::SetCancelType},
        {"3eqs37G74-s", Service::ThreadId}};
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, const std::shared_ptr<GuestThreads>& runtime, std::uint64_t gateBase)
        : machine(guest), threads(runtime),
          trampolines(guest, checkedBase(guest, runtime, gateBase), SceHostTrampolines::DefaultCapacity, "SCE thread import") {}

    static std::uint64_t checkedBase(Machine& guest, const std::shared_ptr<GuestThreads>& runtime, std::uint64_t base) {
        if (!runtime) throw std::invalid_argument("SCE thread imports require a guest thread runtime");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE thread import gates require a nonzero aligned low canonical guest page");
        for (const auto& mapping : guest.Mappings()) {
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("SCE thread import gate page is already mapped");
        }
        return base;
    }

    void invoke(Machine& guest, Service service, bool posix) {
        // POSIX rows return positive errno values; SCE rows return 0x8002xxxx.
        const auto ret = [&](std::uint32_t code) {
            guest.Set(Register::Rax, posix && code >= 0x80020000u && code < 0x80020100u ? code - 0x80020000u : code);
        };
        const auto runtime = threads.lock();
        if (!runtime) throw std::runtime_error("SCE guest thread runtime has expired");
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        switch (service) {
        case Service::Dtors: runtime->RegisterThreadDtors(first); break;
        case Service::AtexitCount: runtime->RegisterThreadAtexitCount(first); break;
        case Service::AtexitReport: runtime->RegisterThreadAtexitReport(first); break;
        case Service::Create:
            if (const auto status = runtime->CreateCheck(second)) { ret(status); break; }
            // pthread_create has no name argument.
            runtime->CreateFromHostCall(first, second, guest.Get(Register::Rdx),
                                       guest.Get(Register::Rcx), posix ? 0 : guest.Get(Register::R8));
            break;
        case Service::Yield: runtime->YieldFromHostCall(); break;
        case Service::Join:
            if (const auto status = runtime->JoinCheck(first)) { ret(status); break; }
            runtime->JoinFromHostCall(first, second);
            break;
        case Service::Self: guest.Set(Register::Rax, runtime->ActiveThread()); break;
        case Service::Equal:
            guest.Set(Register::Rax, static_cast<std::uint64_t>(static_cast<std::int64_t>(runtime->Equal(first, second))));
            break;
        case Service::Error: {
            const auto address = runtime->ActiveErrnoAddress();
            if (!address || address > std::numeric_limits<std::uint64_t>::max() - 4)
                throw std::runtime_error("SCE active guest thread errno is not configured");
            guest.CheckAccess(address, 4, Permission::Read | Permission::Write);
            guest.Set(Register::Rax, address);
            break;
        }
        case Service::Tls: {
            checkIndex(guest, first);
            const auto tls = runtime->ActiveTls();
            if (!tls) throw std::runtime_error("SCE active guest thread TLS is not configured");
            if (guest.Get(Register::FsBase) != tls->FsBase())
                throw std::runtime_error("SCE active guest thread TLS does not match FS");
            guest.Set(Register::Rax, tls->ResolveIndex(first));
            break;
        }
        case Service::Exit: runtime->ExitThreadFromHostCall(first); break;
        case Service::AttrInit: ret(runtime->AttributeInit(first)); break;
        case Service::AttrDestroy: ret(runtime->AttributeDestroy(first)); break;
        case Service::AttrSetPriority: ret(runtime->AttributeSetPriority(first, second)); break;
        case Service::AttrGetPriority: ret(runtime->AttributeGetPriority(first, second)); break;
        case Service::AttrSetInherit:
            ret(runtime->AttributeSetInherit(first, static_cast<std::int32_t>(second))); break;
        case Service::AttrSetPolicy:
            ret(runtime->AttributeSetPolicy(first, static_cast<std::int32_t>(second))); break;
        case Service::TestCancel:
            if (runtime->CancelRequested()) runtime->ExitThreadFromHostCall(Canceled);
            else guest.Set(Register::Rax, 0);
            break;
        case Service::ThreadId: guest.Set(Register::Rax, extended(guest, *runtime, service, first, second)); break;
        default: ret(extended(guest, *runtime, service, first, second)); break;
        }
    }
    static std::uint32_t attrSet(GuestThreads& runtime, std::uint64_t slot, GuestThreadAttributeField field, std::uint64_t value) {
        return runtime.AttributeSet(slot, field, value);
    }
    std::uint32_t extended(Machine& guest, GuestThreads& runtime, Service service, std::uint64_t first, std::uint64_t second) {
        using Field = GuestThreadAttributeField;
        const auto narrow = static_cast<std::uint64_t>(static_cast<std::uint32_t>(second));
        switch (service) {
        case Service::AttrSetStackSize: return attrSet(runtime, first, Field::StackSize, second);
        case Service::AttrGetStackSize: return runtime.AttributeGet(first, Field::StackSize, second);
        case Service::AttrSetStackAddress: return attrSet(runtime, first, Field::StackAddress, second);
        case Service::AttrGetStackAddress: return runtime.AttributeGet(first, Field::StackAddress, second);
        case Service::AttrSetGuardSize: return attrSet(runtime, first, Field::GuardSize, second);
        case Service::AttrGetGuardSize: return runtime.AttributeGet(first, Field::GuardSize, second);
        case Service::AttrSetDetachState: return attrSet(runtime, first, Field::DetachState, narrow);
        case Service::AttrGetDetachState: return runtime.AttributeGet(first, Field::DetachState, second);
        case Service::AttrSetAffinity: return attrSet(runtime, first, Field::Affinity, second);
        case Service::AttrGetAffinity: return runtime.AttributeGet(first, Field::Affinity, second);
        case Service::AttrGetPolicy: return runtime.AttributeGet(first, Field::Policy, second);
        case Service::AttrGetInherit: return runtime.AttributeGet(first, Field::Inherit, second);
        case Service::AttrSetStack: {
            const auto size = guest.Get(Register::Rdx);
            if (!second) return Invalid;
            if (const auto status = attrSet(runtime, first, Field::StackSize, size)) return status;
            return attrSet(runtime, first, Field::StackAddress, second);
        }
        case Service::AttrGetStack: {
            const auto size = guest.Get(Register::Rdx);
            if (!second || !size) return Invalid;
            if (const auto status = runtime.AttributeGet(first, Field::StackAddress, second)) return status;
            return runtime.AttributeGet(first, Field::StackSize, size);
        }
        case Service::AttrGet: return runtime.AttributeFromThread(first, second);
        case Service::Detach: return runtime.Detach(first);
        case Service::GetName: return runtime.GetName(first, second);
        case Service::Rename: return runtime.Rename(first, second);
        case Service::SetPriority: return runtime.SetPriority(first, static_cast<std::int32_t>(second));
        case Service::GetPriority: return runtime.GetPriority(first, second);
        case Service::SetAffinity: return runtime.SetAffinity(first, second);
        case Service::GetAffinity: return runtime.GetAffinity(first, second);
        case Service::Cancel: return runtime.Cancel(first);
        case Service::SetCancelState:
        case Service::SetCancelType: {
            // Deferred, enabled cancellation is the only mode; the previous value is reported.
            if (first > 2) return Invalid;
            if (second) {
                if (second > std::numeric_limits<std::uint64_t>::max() - 4) return Invalid;
                guest.CheckAccess(second, 4, Permission::Write);
                const std::int32_t previous = 0;
                guest.Write(second, std::as_bytes(std::span(&previous, 1)));
            }
            return 0;
        }
        case Service::ThreadId:
            return static_cast<std::uint32_t>(runtime.ThreadId(runtime.ActiveThread()));
        default: break;
        }
        throw std::runtime_error("SCE thread import service is not routed");
    }
    std::optional<std::uint64_t> resolve(const SceImport& import, std::uint8_t symbolType,
                                         Service service, std::weak_ptr<Impl> weak, bool posix = false) {
        const bool library = import.LibraryName == "libkernel" || (posix && import.LibraryName == "libScePosix");
        if (!library || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
            import.ModuleMajor != 1 || import.ModuleMinor != 1)
            throw std::runtime_error("Unsupported SCE thread import scope/version: " + identity(import));
        if (symbolType != 2)
            throw std::runtime_error("Unsupported SCE thread import symbol type: " + identity(import));
        const Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                            import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
        if (const auto found = gates.find(key); found != gates.end()) return found->second;
        const auto gate = trampolines.Add([state = std::move(weak), operation = service, posix,
                                       qualified = identity(import)](Machine& guest) {
            try {
                const auto context = state.lock();
                if (!context) throw std::runtime_error("SCE thread import provider has expired");
                context->invoke(guest, operation, posix);
            } catch (const std::exception& error) {
                throw std::runtime_error("SCE thread import " + qualified + ": " + error.what());
            }
        });
        gates.emplace(key, gate);
        return gate;
    }

};

SceThreadImports::SceThreadImports(Machine& machine, std::shared_ptr<GuestThreads> threads, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, threads, gateBase)) {}
SceThreadImports::~SceThreadImports() = default;

std::optional<std::uint64_t> SceThreadImports::Resolve(const SceImport& import, std::uint8_t symbolType) {
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) return std::nullopt;
    return impl->resolve(import, symbolType, service->second, impl);
}

std::optional<std::uint64_t> SceThreadImports::ResolvePriority(const SceImport& import,
                                                              std::uint8_t symbolType) {
    if (const auto posix = impl->posixServices.find(import.Nid); posix != impl->posixServices.end())
        return impl->resolve(import, symbolType, posix->second, impl, true);
    const auto service = impl->priorityServices.find(import.Nid);
    if (service == impl->priorityServices.end()) return std::nullopt;
    return impl->resolve(import, symbolType, service->second, impl);
}

std::optional<std::uint64_t> SceThreadImports::ResolveTargetPriority(const SceImport& import,
        std::uint8_t symbolType) {
    if (const auto posix = impl->posixServices.find(import.Nid); posix != impl->posixServices.end()) {
        if ((import.LibraryName != "libkernel" && import.LibraryName != "libScePosix") || import.ModuleName != "libkernel" ||
            import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1 || symbolType != 2)
            throw std::runtime_error("Unsupported target POSIX thread scope/version/type: " + identity(import));
        return impl->resolve(import, symbolType, posix->second, impl, true);
    }
    const auto service = impl->priorityServices.find(import.Nid);
    if (service == impl->priorityServices.end()) return std::nullopt;
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1 || symbolType != 2)
        throw std::runtime_error("Unsupported target thread priority scope/version/type: " + identity(import));
    return impl->resolve(import, symbolType, service->second, impl);
}

}
