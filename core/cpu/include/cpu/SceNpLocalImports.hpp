#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// A session-local, offline NP state query. This provides no account, authentication,
// entitlement or callback service. The user must match the session's local user.
class SceNpLocalImports {
public:
    explicit SceNpLocalImports(Machine& machine, std::uint32_t sessionUserId = 0x10000000,
                              std::uint64_t gateBase = 0x7ffdf5000000);
    ~SceNpLocalImports();
    SceNpLocalImports(const SceNpLocalImports&) = delete;
    SceNpLocalImports& operator=(const SceNpLocalImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
