#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {

struct SceLifecycleImports::Impl {
    using Key = std::tuple<std::uint16_t, std::uint16_t>;
    SceHostTrampolines trampolines;
    std::map<Key, std::uint64_t> gates;
    std::function<void(int)> processExit;

    Impl(Machine& guest, std::uint64_t base)
        : trampolines(guest, base, SceHostTrampolines::DefaultCapacity, "SCE lifecycle import") {}
};

SceLifecycleImports::SceLifecycleImports(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceLifecycleImports::~SceLifecycleImports() = default;
void SceLifecycleImports::SetProcessExitHandler(std::function<void(int)> handler) { impl->processExit = std::move(handler); }

std::optional<std::uint64_t> SceLifecycleImports::Resolve(const SceImport& import) {
    if (import.Nid != "6Z83sYWFlA8") return std::nullopt;
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE lifecycle import scope/version: " + import.Nid +
            " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
            " module=" + import.ModuleName + ":" + std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor));
    const Impl::Key key{import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl)](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE lifecycle import runtime has expired");
        const auto code = static_cast<int>(guest.Get(Register::Rdi) & 255);
        const auto handler = context->processExit;
        if (handler) handler(code);
        else guest.Exit(code);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
