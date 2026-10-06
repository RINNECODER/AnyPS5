#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Cpu {

struct SceImport;

class SceLibcBootstrapImports {
public:
    explicit SceLibcBootstrapImports(Machine& machine, std::string programName = "eboot.bin",
                                    std::uint64_t gateBase = 0x7ffdf9000000);
    ~SceLibcBootstrapImports();
    SceLibcBootstrapImports(const SceLibcBootstrapImports&) = delete;
    SceLibcBootstrapImports& operator=(const SceLibcBootstrapImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
    void SetProcessParameters(std::uint64_t address, std::size_t size);
    std::optional<std::array<std::uint64_t, 10>> HeapCallbacks() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
