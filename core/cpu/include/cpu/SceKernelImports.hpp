#pragma once

#include <cpu/Cpu.hpp>
#include <cpu/GuestFiles.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace Cpu {

struct SceImport;
class SceTls;

class SceKernelImports {
public:
    SceKernelImports(Machine& machine, const std::filesystem::path& resourceRoot,
                     std::uint64_t gateBase = 0x7ffdfe000000);
    SceKernelImports(Machine& machine, const std::filesystem::path& resourceRoot, GuestFilesOptions files,
                     std::uint64_t gateBase = 0x7ffdfe000000);
    ~SceKernelImports();
    SceKernelImports(const SceKernelImports&) = delete;
    SceKernelImports& operator=(const SceKernelImports&) = delete;
    std::uint64_t Resolve(const SceImport& import);
    void SetTls(std::shared_ptr<SceTls> tls);
    // Where POSIX aliases (open, read, stat, ...) store errno: the active guest thread's cell,
    // the same one __error returns. Without a provider they still return -1 but store nothing.
    void SetErrnoAddress(std::function<std::uint64_t()> provider);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
