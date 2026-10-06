#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Cpu {

enum class SystemStop { Halted, InstructionLimit };

class SystemMachine {
public:
    explicit SystemMachine(std::size_t physicalRamBytes);
    ~SystemMachine();
    SystemMachine(const SystemMachine&) = delete;
    SystemMachine& operator=(const SystemMachine&) = delete;
    void WritePhysical(std::uint64_t address, std::span<const std::byte> input);
    void ReadPhysical(std::uint64_t address, std::span<std::byte> output) const;
    SystemStop Run(std::uint64_t entry, std::uint64_t stack, std::uint64_t instructionLimit);
    std::uint64_t GetCR0() const;
    std::uint64_t GetCR2() const;
    std::uint64_t GetCR3() const;
    std::uint64_t GetCR4() const;
    std::uint64_t GetRip() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
