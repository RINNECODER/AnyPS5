#include <cpu/SceUpstreamPrxBridge.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include "upstream-prx/Marshal.hpp"
#include <array>
#include <map>
#include <stdexcept>
#include <string>

namespace Cpu {
namespace UpstreamPrx {
Library NpManagerExports();
Library NpWebApi2Exports();
Library NpAuthExports();
Library NpEntitlementAccessExports();
Library SysmoduleExports();
Library AcmExports();
Library MouseExports();
Library ConvertKeycodeExports();
Library NetCtlExports();
Library SystemGestureExports();
Library NpGameIntentExports();
Library ImeExports();
Library MsgDialogExports();
Library ErrorDialogExports();
Library SigninDialogExports();
Library NpCommerceExports();
Library PlayerInvitationDialogExports();
Library NpUniversalDataSystemExports();
Library NpTrophy2Exports();
Library RtcExports();
}

namespace {

const std::array<UpstreamPrx::Library, 20>& Libraries() {
    static const std::array<UpstreamPrx::Library, 20> libraries{
        UpstreamPrx::NpManagerExports(),
        UpstreamPrx::NpWebApi2Exports(),
        UpstreamPrx::NpAuthExports(),
        UpstreamPrx::NpEntitlementAccessExports(),
        UpstreamPrx::SysmoduleExports(),
        UpstreamPrx::AcmExports(),
        UpstreamPrx::MouseExports(),
        UpstreamPrx::ConvertKeycodeExports(),
        UpstreamPrx::NetCtlExports(),
        UpstreamPrx::SystemGestureExports(),
        UpstreamPrx::NpGameIntentExports(),
        UpstreamPrx::ImeExports(),
        UpstreamPrx::MsgDialogExports(),
        UpstreamPrx::ErrorDialogExports(),
        UpstreamPrx::SigninDialogExports(),
        UpstreamPrx::NpCommerceExports(),
        UpstreamPrx::PlayerInvitationDialogExports(),
        UpstreamPrx::NpUniversalDataSystemExports(),
        UpstreamPrx::NpTrophy2Exports(),
        UpstreamPrx::RtcExports()};
    return libraries;
}

std::size_t ExportCount() {
    std::size_t count = 0;
    for (const auto& library : Libraries()) count += library.Exports.size();
    return count;
}

}

struct SceUpstreamPrxBridge::Impl {
    struct Function { UpstreamPrx::Library Scope; UpstreamPrx::Export Export; };
    SceHostTrampolines trampolines;
    std::map<std::string, Function> functions;
    std::map<std::string, std::uint64_t> gates;

    Impl(Machine& machine, std::uint64_t base) : trampolines(machine, base, ExportCount()) {
        for (const auto& library : Libraries())
            for (const auto& entry : library.Exports)
                if (!functions.emplace(Nid::ComputeNid(std::string(entry.Name), ""), Function{library, entry}).second)
                    throw std::logic_error("Duplicate upstream prx bridge NID for " + std::string(entry.Name));
    }
};

SceUpstreamPrxBridge::SceUpstreamPrxBridge(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceUpstreamPrxBridge::~SceUpstreamPrxBridge() = default;

std::vector<SceUpstreamHost> SceUpstreamPrxBridge::Hosts() {
    std::vector<SceUpstreamHost> hosts;
    for (const auto& library : Libraries())
        hosts.push_back({std::string(library.File.empty() ? std::string(library.Name) + ".prx" : std::string(library.File)),
            std::string(library.Name), std::string(library.LibraryName.empty() ? library.Name : library.LibraryName),
            library.ModuleMajor, library.ModuleMinor});
    return hosts;
}

std::vector<std::string_view> SceUpstreamPrxBridge::Modules() {
    std::vector<std::string_view> names;
    for (const auto& library : Libraries()) names.push_back(library.Name);
    return names;
}

std::optional<std::uint64_t> SceUpstreamPrxBridge::Resolve(const SceImport& import) {
    const auto found = impl->functions.find(import.Nid);
    if (found == impl->functions.end()) return std::nullopt;
    // Same audited scope as the hand-written NP providers: the export's own library in its
    // own module, library version 1, and the module version the title was linked against.
    const auto& scope = found->second.Scope;
    if (import.LibraryName != (scope.LibraryName.empty() ? scope.Name : scope.LibraryName) ||
        import.ModuleName != scope.Name || import.LibraryVersion != 1 ||
        import.ModuleMajor != scope.ModuleMajor || import.ModuleMinor != scope.ModuleMinor)
        return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    const auto gate = impl->trampolines.Add([entry = found->second.Export](Machine& guest) {
        entry.Call(guest, entry.Name);
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
