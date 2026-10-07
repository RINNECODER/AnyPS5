#include "NpServices.hpp"
#include <cpu/SceElf.hpp>
#include <bit>
#include <map>
#include <stdexcept>
#include <tuple>

namespace Cpu::Platform {
namespace {
constexpr std::int64_t invalidArgument = std::bit_cast<std::int32_t>(0x80550003u);
constexpr std::int64_t signedOut = std::bit_cast<std::int32_t>(0x80550006u);
}
struct NpServices::Impl {
    using Key = std::tuple<std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::uint32_t user;
    std::uint64_t base;
    std::map<Key, std::uint64_t> gates;
    Impl(Machine& guest, std::uint32_t sessionUser, std::uint64_t gateBase)
        : machine(guest), user(sessionUser), base(gateBase) {
        if (user == 0xffffffffu) throw std::invalid_argument("NP offline session requires a valid user");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("NP gates require an aligned low canonical page");
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
    ~Impl() {
        // Host-call callbacks hold weak references only; release the owned guest page.
        try { machine.Unmap(base, 4096); } catch (...) { }
    }
    void getOnlineId(Machine& guest) const {
        // Offline queries never dereference or write a non-null identity pointer.
        // Thus no unproven PS5 identity structure size is used on this branch.
        const bool valid = static_cast<std::uint32_t>(guest.Get(Register::Rdi)) == user &&
                           guest.Get(Register::Rsi) != 0;
        guest.Set(Register::Rax, static_cast<std::uint64_t>(valid ? signedOut : invalidArgument));
    }
};
NpServices::NpServices(Machine& machine, std::uint32_t user, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, user, gateBase)) {}
NpServices::~NpServices() = default;

std::optional<std::uint64_t> NpServices::Resolve(const SceImport& import, std::uint8_t type) {
    const auto& registration = NpRegistrations.front();
    if (import.LibraryName != registration.Library && import.ModuleName != registration.Module)
        return std::nullopt;
    // Other Manager calls, including the existing GetState provider, are left to
    // their actual owner. No other NP family is claimed by this component.
    if (import.Nid != registration.Nid) return std::nullopt;
    if (import.LibraryName != registration.Library || import.ModuleName != registration.Module ||
        import.LibraryVersion != registration.LibraryVersion || import.ModuleMajor != registration.ModuleMajor ||
        import.ModuleMinor != registration.ModuleMinor || type != registration.Type)
        throw std::runtime_error("Unsupported NP offline query scope/version/type");
    const Impl::Key key{import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() == 256) throw std::runtime_error("NP offline gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [weak = std::weak_ptr<Impl>(impl)](Machine& machine) {
        const auto owner = weak.lock();
        if (!owner) throw std::runtime_error("NP offline provider expired");
        owner->getOnlineId(machine);
    });
    impl->gates.emplace(key, gate);
    return gate;
}
}
