#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

class SceUserImports {
public:
    explicit SceUserImports(Machine& machine, std::uint64_t gateBase = 0x7ffdfc000000);
    ~SceUserImports();
    SceUserImports(const SceUserImports&) = delete;
    SceUserImports& operator=(const SceUserImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
