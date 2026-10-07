#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <functional>
#include <memory>

namespace Cpu {

struct SceImport;

class SceImports {
public:
    explicit SceImports(Machine& machine, std::uint64_t gateBase = 0x7ffdff000000);
    ~SceImports();
    SceImports(const SceImports&) = delete;
    SceImports& operator=(const SceImports&) = delete;
    std::uint64_t Resolve(const SceImport& import);
    std::uint64_t ExitGate() const;
    void SetProcessExitHandler(std::function<void(int)> handler);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
