#include <cpu/SceCommonDialogImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <array>
#include <bit>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Cpu {
namespace {

constexpr std::int64_t alreadyInitialized = std::bit_cast<std::int32_t>(0x80b80002u);

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

}

struct SceCommonDialogImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    SceHostTrampolines trampolines;
    bool initialized = false;
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint64_t base)
        : machine(guest), trampolines(guest, base, SceHostTrampolines::DefaultCapacity, "SCE common dialog import") {}

    void initialize(Machine& guest) {
        const auto result = initialized ? alreadyInitialized : std::int64_t{0};
        initialized = true;
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceCommonDialogImports::SceCommonDialogImports(Machine& machine, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, gateBase)) {}
SceCommonDialogImports::~SceCommonDialogImports() = default;

std::optional<std::uint64_t> SceCommonDialogImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libSceCommonDialog" && import.ModuleName != "libSceCommonDialog")
        return std::nullopt;
    if (import.LibraryName != "libSceCommonDialog" || import.ModuleName != "libSceCommonDialog" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE common dialog import scope/version: " + identity(import));
    if (import.Nid != "uoUpLGNkygk")
        throw std::runtime_error("Unsupported SCE common dialog import service: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl)](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE common dialog import runtime has expired");
        context->initialize(guest);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
