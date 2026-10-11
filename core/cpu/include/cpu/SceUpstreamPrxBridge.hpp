#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace Cpu {

struct SceImport;

// Routes guest imports to upstream core/libs/prx HLE exports compiled for this host
// (libSceNpManager, libSceNpWebApi2, libSceNpAuth, libSceNpEntitlementAccess).
// Upstream assumes guest pointers are host pointers, so each export goes through a
// hand-checked descriptor (src/upstream-prx/Marshal.hpp) that copies guest buffers in
// and out. Exports without a descriptor resolve to nullopt. All resolved imports share
// one SceHostTrampolines table (one host gate). Machine must outlive this provider.
class SceUpstreamPrxBridge {
public:
    explicit SceUpstreamPrxBridge(Machine& machine, std::uint64_t gateBase = 0x7ffda8000000);
    ~SceUpstreamPrxBridge();
    SceUpstreamPrxBridge(const SceUpstreamPrxBridge&) = delete;
    SceUpstreamPrxBridge& operator=(const SceUpstreamPrxBridge&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
    // Module (and same-named library) names whose bridged exports Resolve serves.
    static std::vector<std::string_view> Modules();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
