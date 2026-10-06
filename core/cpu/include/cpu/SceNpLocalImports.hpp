#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

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
