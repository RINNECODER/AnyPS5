#include <cpu/SceThreadImports.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service { Dtors, AtexitCount, AtexitReport, Create, Yield, Join, Self, Equal, Error, Tls, Exit, AttrInit, AttrDestroy, AttrSetPriority, AttrGetPriority, AttrSetInherit, AttrSetPolicy };

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
    const std::uint64_t base;
    std::size_t nextSlot = 0;
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
        {"eXbUSpEaTsA", Service::AttrSetInherit}, {"4+h9EzwKF4I", Service::AttrSetPolicy}};
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, const std::shared_ptr<GuestThreads>& runtime, std::uint64_t gateBase)
        : machine(guest), threads(runtime), base(gateBase) {
        if (!runtime) throw std::invalid_argument("SCE thread imports require a guest thread runtime");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE thread import gates require a nonzero aligned low canonical guest page");
        for (const auto& mapping : machine.Mappings()) {
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("SCE thread import gate page is already mapped");
        }
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        try {
            machine.Write(base, bytes);
            machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
        } catch (...) {
            machine.Unmap(base, bytes.size());
            throw;
        }
    }

    void invoke(Machine& guest, Service service) {
        const auto runtime = threads.lock();
        if (!runtime) throw std::runtime_error("SCE guest thread runtime has expired");
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        switch (service) {
        case Service::Dtors: runtime->RegisterThreadDtors(first); break;
        case Service::AtexitCount: runtime->RegisterThreadAtexitCount(first); break;
        case Service::AtexitReport: runtime->RegisterThreadAtexitReport(first); break;
        case Service::Create:
            runtime->CreateFromHostCall(first, second, guest.Get(Register::Rdx),
                                       guest.Get(Register::Rcx), guest.Get(Register::R8));
            break;
        case Service::Yield: runtime->YieldFromHostCall(); break;
        case Service::Join: runtime->JoinFromHostCall(first, second); break;
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
        case Service::AttrInit: guest.Set(Register::Rax, runtime->AttributeInit(first)); break;
        case Service::AttrDestroy: guest.Set(Register::Rax, runtime->AttributeDestroy(first)); break;
        case Service::AttrSetPriority: guest.Set(Register::Rax, runtime->AttributeSetPriority(first, second)); break;
        case Service::AttrGetPriority: guest.Set(Register::Rax, runtime->AttributeGetPriority(first, second)); break;
        case Service::AttrSetInherit:
            guest.Set(Register::Rax, runtime->AttributeSetInherit(first, static_cast<std::int32_t>(second))); break;
        case Service::AttrSetPolicy:
            guest.Set(Register::Rax, runtime->AttributeSetPolicy(first, static_cast<std::int32_t>(second))); break;
        }
    }
    std::optional<std::uint64_t> resolve(const SceImport& import, std::uint8_t symbolType,
                                         Service service, std::weak_ptr<Impl> weak) {
        if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
            import.ModuleMajor != 1 || import.ModuleMinor != 1)
            throw std::runtime_error("Unsupported SCE thread import scope/version: " + identity(import));
        if (symbolType != 2)
            throw std::runtime_error("Unsupported SCE thread import symbol type: " + identity(import));
        const Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                            import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
        if (const auto found = gates.find(key); found != gates.end()) return found->second;
        if (nextSlot == 256) throw std::runtime_error("SCE thread import gate page is exhausted");
        const auto gate = base + nextSlot * 16;
        const std::array ret{std::byte{0xc3}};
        machine.Write(gate, ret);
        machine.AddHostCall(gate, [state = std::move(weak), operation = service,
                                       qualified = identity(import)](Machine& guest) {
            try {
                const auto context = state.lock();
                if (!context) throw std::runtime_error("SCE thread import provider has expired");
                context->invoke(guest, operation);
            } catch (const std::exception& error) {
                throw std::runtime_error("SCE thread import " + qualified + ": " + error.what());
            }
        });
        gates.emplace(key, gate);
        ++nextSlot;
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
    const auto service = impl->priorityServices.find(import.Nid);
    if (service == impl->priorityServices.end()) return std::nullopt;
    return impl->resolve(import, symbolType, service->second, impl);
}

std::optional<std::uint64_t> SceThreadImports::ResolveTargetPriority(const SceImport& import,
        std::uint8_t symbolType) {
    const auto service = impl->priorityServices.find(import.Nid);
    if (service == impl->priorityServices.end()) return std::nullopt;
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1 || symbolType != 2)
        throw std::runtime_error("Unsupported target thread priority scope/version/type: " + identity(import));
    return impl->resolve(import, symbolType, service->second, impl);
}

}
