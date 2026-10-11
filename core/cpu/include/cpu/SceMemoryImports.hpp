#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <functional>
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
    // Where the POSIX aliases (mmap, munmap, mprotect, mlock, munlock) store errno: the active
    // guest thread's errno slot. Without it they still return -1 but leave errno untouched.
    void SetErrnoLocation(std::function<std::uint64_t()> location);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
