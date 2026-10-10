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
}

namespace {

std::array<UpstreamPrx::Library, 4> Libraries() {
    return {UpstreamPrx::NpManagerExports(), UpstreamPrx::NpWebApi2Exports(),
            UpstreamPrx::NpAuthExports(), UpstreamPrx::NpEntitlementAccessExports()};
}

std::size_t ExportCount() {
    std::size_t count = 0;
    for (const auto& library : Libraries()) count += library.Exports.size();
    return count;
}

}

struct SceUpstreamPrxBridge::Impl {
    struct Function { std::string_view Library; UpstreamPrx::Export Export; };
    SceHostTrampolines trampolines;
    std::map<std::string, Function> functions;
    std::map<std::string, std::uint64_t> gates;

    Impl(Machine& machine, std::uint64_t base) : trampolines(machine, base, ExportCount()) {
        for (const auto& library : Libraries())
            for (const auto& entry : library.Exports)
                if (!functions.emplace(Nid::ComputeNid(std::string(entry.Name), ""), Function{library.Name, entry}).second)
                    throw std::logic_error("Duplicate upstream prx bridge NID for " + std::string(entry.Name));
    }
};

SceUpstreamPrxBridge::SceUpstreamPrxBridge(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceUpstreamPrxBridge::~SceUpstreamPrxBridge() = default;

std::vector<std::string_view> SceUpstreamPrxBridge::Modules() {
    std::vector<std::string_view> names;
    for (const auto& library : Libraries()) names.push_back(library.Name);
    return names;
}

std::optional<std::uint64_t> SceUpstreamPrxBridge::Resolve(const SceImport& import) {
    const auto found = impl->functions.find(import.Nid);
    if (found == impl->functions.end()) return std::nullopt;
    const auto library = found->second.Library;
    // Same audited scope as the hand-written NP providers: the export's own library in
    // its own module, library version 1, module 1.1.
    if (import.LibraryName != library || import.ModuleName != library || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    const auto gate = impl->trampolines.Add([entry = found->second.Export](Machine& guest) {
        entry.Call(guest, entry.Name);
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
