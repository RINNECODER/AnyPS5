#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// Offline PSN for the NP exports SceUpstreamPrxBridge does not serve: libSceNpWebApi,
// callback registration and exports NotImplemented upstream. Initialisation and
// handles succeed, the user stays signed out, and every network request fails.
// Unknown NIDs return nullopt.
// Machine must outlive this provider.
class SceNpOfflineImports {
public:
    explicit SceNpOfflineImports(Machine& machine, std::uint64_t gateBase = 0x7ffda0000000);
    ~SceNpOfflineImports();
    SceNpOfflineImports(const SceNpOfflineImports&) = delete;
    SceNpOfflineImports& operator=(const SceNpOfflineImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
