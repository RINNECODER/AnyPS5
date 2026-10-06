#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace Cpu {

struct SceImport;
class SceTls;

class SceKernelImports {
public:
    SceKernelImports(Machine& machine, const std::filesystem::path& resourceRoot,
                     std::uint64_t gateBase = 0x7ffdfe000000);
    ~SceKernelImports();
    SceKernelImports(const SceKernelImports&) = delete;
    SceKernelImports& operator=(const SceKernelImports&) = delete;
    std::uint64_t Resolve(const SceImport& import);
    void SetTls(std::shared_ptr<SceTls> tls);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
