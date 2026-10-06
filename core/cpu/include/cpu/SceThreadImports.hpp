#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;
class GuestThreads;

class SceThreadImports {
public:
    SceThreadImports(Machine& machine, std::shared_ptr<GuestThreads> threads,
                     std::uint64_t gateBase = 0x7ffdf1000000);
    ~SceThreadImports();
    SceThreadImports(const SceThreadImports&) = delete;
    SceThreadImports& operator=(const SceThreadImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import, std::uint8_t symbolType = 2);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
