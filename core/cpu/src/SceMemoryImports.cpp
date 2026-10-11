#include <cpu/SceMemoryImports.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service { Size, Available, Allocate, AllocateMain, MapDirect, MapFlexible, Reserve, Protect, Query, Unmap, Release };

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

std::int32_t signedInt(std::uint64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

}

struct SceMemoryImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    std::shared_ptr<GuestMemoryRuntime> memory;
    std::optional<SceHostTrampolines> trampolines;
    std::map<std::string, Service> services;
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::shared_ptr<GuestMemoryRuntime> runtime, std::uint64_t base) :
        machine(guest), memory(std::move(runtime)) {
        if (!memory) throw std::invalid_argument("SCE memory imports require a guest memory runtime");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE memory import gates require a nonzero aligned low canonical guest page");
        for (const auto& [name, service] : std::array<std::pair<const char*, Service>, 11>{{
                 {"sceKernelGetDirectMemorySize", Service::Size}, {"sceKernelAvailableDirectMemorySize", Service::Available},
                 {"sceKernelAllocateDirectMemory", Service::Allocate}, {"sceKernelAllocateMainDirectMemory", Service::AllocateMain},
                 {"sceKernelMapDirectMemory", Service::MapDirect}, {"sceKernelMapFlexibleMemory", Service::MapFlexible},
                 {"sceKernelReserveVirtualRange", Service::Reserve}, {"sceKernelMprotect", Service::Protect},
                 {"sceKernelVirtualQuery", Service::Query}, {"sceKernelMunmap", Service::Unmap},
                 {"sceKernelReleaseDirectMemory", Service::Release}}})
            services.emplace(Nid::ComputeNid(name, "libkernel"), service);
        trampolines.emplace(machine, base, SceHostTrampolines::DefaultCapacity, "SCE memory import");
    }

    void checkOutput(std::uint64_t address, std::size_t size, bool input = false) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address)
            throw GuestMemoryError(14, "Invalid guest memory import output span");
        try { machine.CheckAccess(address, size, input ? Permission::Read | Permission::Write : Permission::Write); }
        catch (const std::runtime_error&) { throw GuestMemoryError(14, "Guest memory import output span is inaccessible"); }
    }

    std::uint64_t readAddress(std::uint64_t address) const {
        checkOutput(address, 8, true);
        std::uint64_t value = 0;
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }

    void writeAddress(std::uint64_t address, std::uint64_t value) {
        machine.Write(address, std::as_bytes(std::span(&value, 1)));
    }

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        const auto fourth = guest.Get(Register::Rcx);
        const auto fifth = guest.Get(Register::R8);
        const auto sixth = guest.Get(Register::R9);
        if (service == Service::Size) { guest.Set(Register::Rax, memory->DirectMemorySize()); return; }
        std::int64_t result = 0;
        try {
            switch (service) {
            case Service::Available: {
                checkOutput(fourth, 8);
                checkOutput(fifth, 8);
                const auto extent = memory->AvailableDirect(std::bit_cast<std::int64_t>(first), std::bit_cast<std::int64_t>(second), third);
                writeAddress(fourth, extent.Address);
                writeAddress(fifth, extent.Size);
                break;
            }
            case Service::Allocate:
                checkOutput(sixth, 8);
                writeAddress(sixth, memory->AllocateDirect(std::bit_cast<std::int64_t>(first), std::bit_cast<std::int64_t>(second), third, fourth, signedInt(fifth)));
                break;
            case Service::AllocateMain:
                checkOutput(fourth, 8);
                writeAddress(fourth, memory->AllocateDirect(0, static_cast<std::int64_t>(memory->DirectMemorySize()), first, second, signedInt(third)));
                break;
            case Service::MapDirect: {
                const auto hint = readAddress(first);
                const auto address = memory->MapDirect(hint, second, static_cast<std::uint32_t>(third), static_cast<std::uint32_t>(fourth),
                                                      std::bit_cast<std::int64_t>(fifth), sixth, first);
                writeAddress(first, address);
                break;
            }
            case Service::MapFlexible: {
                const auto hint = readAddress(first);
                writeAddress(first, memory->MapFlexible(hint, second, static_cast<std::uint32_t>(third), static_cast<std::uint32_t>(fourth), first));
                break;
            }
            case Service::Reserve: {
                const auto hint = readAddress(first);
                writeAddress(first, memory->Reserve(hint, second, static_cast<std::uint32_t>(third), fourth, first));
                break;
            }
            case Service::Protect: memory->Protect(first, second, static_cast<std::uint32_t>(third)); break;
            case Service::Query: {
                if (fourth < 72) throw GuestMemoryError(22, "Guest virtual query information is too small");
                checkOutput(third, 72);
                const auto flags = static_cast<std::uint32_t>(second);
                if (flags & ~1u) throw GuestMemoryError(22, "Unsupported guest memory virtual query flags");
                const auto bytes = memory->Query(first, flags != 0).Serialize();
                machine.Write(third, bytes);
                break;
            }
            case Service::Unmap: memory->Unmap(first, second); break;
            case Service::Release: memory->ReleaseDirect(std::bit_cast<std::int64_t>(first), second); break;
            default: throw std::runtime_error("Unsupported SCE memory import operation");
            }
        } catch (const GuestMemoryError& error) {
            result = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(error.Result()));
        }
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceMemoryImports::SceMemoryImports(Machine& machine, std::shared_ptr<GuestMemoryRuntime> memory, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, std::move(memory), gateBase)) {}
SceMemoryImports::~SceMemoryImports() = default;

std::optional<std::uint64_t> SceMemoryImports::Resolve(const SceImport& import) {
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) return std::nullopt;
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE memory import scope/version: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines->Add([state = std::weak_ptr<Impl>(impl), operation = service->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE memory import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
