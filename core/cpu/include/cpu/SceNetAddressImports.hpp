#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// Local IPv4 conversion only; no socket, resolver, or guest errno services.
class SceNetAddressImports {
public:
    explicit SceNetAddressImports(Machine& machine, std::uint64_t gateBase = 0x7ffdf3000000);
    ~SceNetAddressImports();
    SceNetAddressImports(const SceNetAddressImports&) = delete;
    SceNetAddressImports& operator=(const SceNetAddressImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
