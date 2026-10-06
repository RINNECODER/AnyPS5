#pragma once

#include <cpu/Cpu.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Cpu {

class SceTls {
public:
    SceTls(Machine& machine, std::span<const std::byte> initialBytes,
           std::uint64_t memorySize, std::uint64_t alignment,
           std::uint64_t allocationBase = 0x7ffc00000000);
    std::uint64_t FsBase() const;
    std::uint64_t TlsBase() const;
    std::uint64_t ModuleId() const;
    std::uint64_t MemorySize() const;
    std::uint64_t Resolve(std::uint64_t moduleId, std::uint64_t offset) const;
    std::uint64_t ResolveIndex(std::uint64_t guestIndexAddress) const;
    std::uint64_t Dtpoff(std::uint64_t moduleId, std::uint64_t offset) const;
    std::int64_t Tpoff(std::uint64_t moduleId, std::uint64_t offset) const;
private:
    Machine& machine;
    std::uint64_t tlsBase = 0;
    std::uint64_t fsBase = 0;
    std::uint64_t memorySize = 0;
    std::uint64_t blockSize = 0;
};

}
