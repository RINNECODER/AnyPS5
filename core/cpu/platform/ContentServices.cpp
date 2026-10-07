#include "ContentServices.hpp"
#include <cpu/SceElf.hpp>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace Cpu::Platform {
namespace {
constexpr std::int64_t ParameterError = std::bit_cast<std::int32_t>(0x80d90002u);
}
struct ContentServices::Impl {
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::weak_ptr<const InstalledContentRecord> record;
    std::optional<ContentAbi> abi;
    std::uint64_t base;
    std::map<Key, std::uint64_t> gates;
    bool mapped = false;

    Impl(Machine& machine, std::weak_ptr<const InstalledContentRecord> record,
         std::optional<ContentAbi> abi, std::uint64_t base)
        : machine(machine), record(std::move(record)), abi(abi), base(base) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("Content gates require an aligned low canonical guest page");
        if (abi && *abi != ContentAbi::PublicOrbisCandidate)
            throw std::invalid_argument("Unsupported content ABI profile");
        if (abi) {
            const auto installed = this->record.lock();
            if (!installed || installed->provenance.empty())
                throw std::invalid_argument("Content ABI requires an owned installed record with provenance");
            if (installed->skuFlag != 1 && installed->skuFlag != 3)
                throw std::invalid_argument("Content record has unsupported SKU flag");
        }
    }
    ~Impl() {
        // Session ownership requires the Machine to remain alive and idle during provider teardown.
        // Unmap also removes this page's native host-call registrations.
        if (mapped) {
            try { machine.Unmap(base, 4096); }
            catch (...) { /* Destructors cannot propagate a failed runtime teardown. */ }
        }
    }
    bool writable(std::uint64_t address) const {
        if (!address || address > std::numeric_limits<std::uint64_t>::max() - 4) return false;
        try { machine.CheckAccess(address, 4, Permission::Write); }
        catch (const std::runtime_error&) { return false; }
        return true;
    }
    void invoke(Machine& guest) {
        const auto installed = record.lock();
        if (!installed) throw std::runtime_error("Content installed-record owner has expired");
        const auto id = static_cast<std::uint32_t>(guest.Get(Register::Rdi));
        const auto destination = guest.Get(Register::Rsi);
        if (id > 4 || !writable(destination)) {
            guest.Set(Register::Rax, static_cast<std::uint64_t>(ParameterError));
            return;
        }
        std::int32_t value;
        if (id == 0) value = installed->skuFlag;
        else {
            if (!installed->userDefined[id - 1])
                throw std::runtime_error("Content installed record lacks requested user parameter");
            value = *installed->userDefined[id - 1];
        }
        // Guest data are four LE bytes even on a native host with different alignment/layout.
        const auto bits = std::bit_cast<std::uint32_t>(value);
        std::array<std::byte, 4> bytes;
        for (unsigned i = 0; i < 4; ++i) bytes[i] = std::byte((bits >> (8 * i)) & 255);
        guest.Write(destination, bytes);
        guest.Set(Register::Rax, 0);
    }
};
ContentServices::ContentServices(Machine& machine, std::weak_ptr<const InstalledContentRecord> record,
    std::optional<ContentAbi> abi, std::uint64_t base)
    : impl(std::make_shared<Impl>(machine, std::move(record), abi, base)) {}
ContentServices::~ContentServices() = default;

std::optional<std::uint64_t> ContentServices::Resolve(const SceImport& import, std::uint8_t type) {
    const auto& entry = Inventory[0];
    if (import.LibraryName != entry.library && import.ModuleName != entry.module) return std::nullopt;
    if (import.LibraryName != entry.library || import.ModuleName != entry.module ||
        import.LibraryVersion != entry.libraryVersion || import.ModuleMajor != entry.moduleMajor ||
        import.ModuleMinor != entry.moduleMinor || type != 2)
        throw std::runtime_error("Unsupported content import scope/version/type");
    if (import.Nid != entry.nid || !impl->abi) return std::nullopt;
    if (impl->record.expired()) throw std::runtime_error("Content installed-record owner has expired");
    const Impl::Key key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() >= 256) throw std::runtime_error("Content gate page exhausted");
    bool createdPage = false;
    try {
        if (!impl->mapped) {
            std::array<std::byte, 4096> page;
            page.fill(std::byte{0xcc});
            impl->machine.Map(impl->base, page.size(), Permission::Read | Permission::Write);
            // Record ownership immediately so all subsequent setup failures release the new page.
            impl->mapped = true;
            createdPage = true;
            impl->machine.Write(impl->base, page);
            impl->machine.Protect(impl->base, page.size(), Permission::Read | Permission::Execute);
        }
        const auto gate = impl->base + impl->gates.size() * 16;
        constexpr std::array ret{std::byte{0xc3}};
        impl->machine.Write(gate, ret);
        impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl)](Machine& guest) {
            const auto service = state.lock();
            if (!service) throw std::runtime_error("Content service owner has expired");
            service->invoke(guest);
        });
        impl->gates.emplace(key, gate);
        return gate;
    } catch (...) {
        if (createdPage) {
            impl->machine.Unmap(impl->base, 4096);
            impl->mapped = false;
        }
        throw;
    }
}
}
