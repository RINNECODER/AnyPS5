#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

class SceSystemImports {
public:
    explicit SceSystemImports(Machine& machine, std::uint64_t gateBase = 0x7ffdfa000000);
    ~SceSystemImports();
    SceSystemImports(const SceSystemImports&) = delete;
    SceSystemImports& operator=(const SceSystemImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
