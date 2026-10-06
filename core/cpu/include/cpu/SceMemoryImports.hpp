#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;
class GuestMemoryRuntime;

class SceMemoryImports {
public:
    SceMemoryImports(Machine& machine, std::shared_ptr<GuestMemoryRuntime> memory,
                     std::uint64_t gateBase = 0x7ffdf6000000);
    ~SceMemoryImports();
    SceMemoryImports(const SceMemoryImports&) = delete;
    SceMemoryImports& operator=(const SceMemoryImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
