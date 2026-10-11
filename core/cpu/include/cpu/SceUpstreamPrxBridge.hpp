#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Cpu {

struct SceImport;

// One host-provided module/library pair a provider serves: the module file titles list in
// DT_NEEDED, the import module and library names and the module version they were linked with.
struct SceUpstreamHost {
    std::string File;
    std::string Module;
    std::string Library;
    std::uint16_t Major = 1;
    std::uint16_t Minor = 1;
};

// Routes guest imports to upstream core/libs/prx HLE exports compiled for this host
// (NP, system, dialog and input leaf libraries; see src/upstream-prx).
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
    // Every module/library scope Resolve can serve, for declaring them as host modules.
    static std::vector<SceUpstreamHost> Hosts();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
