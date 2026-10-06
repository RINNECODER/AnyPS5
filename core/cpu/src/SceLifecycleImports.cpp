#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Cpu {

struct SceLifecycleImports::Impl {
    using Key = std::tuple<std::uint16_t, std::uint16_t>;
    Machine& machine;
    const std::uint64_t base;
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint64_t address) : machine(guest), base(address) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE lifecycle import gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes{};
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }
};

SceLifecycleImports::SceLifecycleImports(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceLifecycleImports::~SceLifecycleImports() = default;

std::optional<std::uint64_t> SceLifecycleImports::Resolve(const SceImport& import) {
    if (import.Nid != "6Z83sYWFlA8") return std::nullopt;
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE lifecycle import scope/version: " + import.Nid +
            " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
            " module=" + import.ModuleName + ":" + std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor));
    const Impl::Key key{import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() == 256) throw std::runtime_error("SCE lifecycle import gate page is exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl)](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE lifecycle import runtime has expired");
        guest.Exit(static_cast<int>(guest.Get(Register::Rdi) & 255));
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
