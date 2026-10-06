#include <cpu/SceNpLocalImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Cpu {
namespace {

constexpr std::int64_t invalidArgument = std::bit_cast<std::int32_t>(0x80550003u);
constexpr std::array offlineState{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

}

struct SceNpLocalImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    const std::uint32_t userId;
    const std::uint64_t base;
    std::size_t nextSlot = 0;
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint32_t sessionUserId, std::uint64_t gateBase) :
        machine(guest), userId(sessionUserId), base(gateBase) {
        if (userId == 0xffffffffu)
            throw std::invalid_argument("SCE local NP requires a valid session user");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE local NP import gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

    bool writable(std::uint64_t address) const {
        if (!address || offlineState.size() > std::numeric_limits<std::uint64_t>::max() - address)
            return false;
        try { machine.CheckAccess(address, offlineState.size(), Permission::Write); }
        catch (const std::runtime_error&) { return false; }
        return true;
    }

    void getState(Machine& guest) {
        const auto user = static_cast<std::uint32_t>(guest.Get(Register::Rdi));
        const auto destination = guest.Get(Register::Rsi);
        std::int64_t result = invalidArgument;
        if (user == userId && writable(destination)) {
            machine.Write(destination, offlineState);
            result = 0;
        }
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceNpLocalImports::SceNpLocalImports(Machine& machine, std::uint32_t sessionUserId,
                                   std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, sessionUserId, gateBase)) {}
SceNpLocalImports::~SceNpLocalImports() = default;

std::optional<std::uint64_t> SceNpLocalImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libSceNpManager" && import.ModuleName != "libSceNpManager")
        return std::nullopt;
    if (import.LibraryName != "libSceNpManager" || import.ModuleName != "libSceNpManager" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE local NP import scope/version: " + identity(import));
    if (import.Nid != "eQH7nWPcAgc")
        throw std::runtime_error("Unsupported SCE local NP import service: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->nextSlot == 256) throw std::runtime_error("SCE local NP import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl)](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE local NP import runtime has expired");
        context->getState(guest);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
