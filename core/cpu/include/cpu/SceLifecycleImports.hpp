#pragma once

#include <cpu/Cpu.hpp>
#include <functional>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

class SceLifecycleImports {
public:
    explicit SceLifecycleImports(Machine& machine, std::uint64_t gateBase = 0x7ffdf4000000);
    ~SceLifecycleImports();
    SceLifecycleImports(const SceLifecycleImports&) = delete;
    SceLifecycleImports& operator=(const SceLifecycleImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
    void SetProcessExitHandler(std::function<void(int)> handler);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
