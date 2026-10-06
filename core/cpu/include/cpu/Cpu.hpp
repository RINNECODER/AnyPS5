#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace Cpu {

enum class Register { Rax, Rbx, Rcx, Rdx, Rsi, Rdi, Rbp, Rsp, R8, R9, R10, R11, R12, R13, R14, R15, Rip, Rflags, FsBase, GsBase };
enum class Permission : unsigned { Read = 1, Write = 2, Execute = 4 };
constexpr Permission operator|(Permission a, Permission b) { return static_cast<Permission>(static_cast<unsigned>(a) | static_cast<unsigned>(b)); }
enum class StopReason { Address, Exit, InstructionLimit, Requested };
struct Mapping {
    std::uint64_t Address;
    std::size_t Size;
    Permission Permissions;
    bool Borrowed;
};

class Machine {
public:
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    void Map(std::uint64_t address, std::size_t size, Permission permissions);
    void MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions);
    void Unmap(std::uint64_t address, std::size_t size);
    void ReplaceBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions);
    void Protect(std::uint64_t address, std::size_t size, Permission permissions);
    std::vector<Mapping> Mappings() const;
    void CheckAccess(std::uint64_t address, std::size_t size, Permission permissions) const;
    void Read(std::uint64_t address, std::span<std::byte> output) const;
    void Write(std::uint64_t address, std::span<const std::byte> input);
    std::uint64_t Get(Register reg) const;
    void Set(Register reg, std::uint64_t value);
    void SetSyscallHandler(std::function<void(Machine&)> handler);
    void AddHostCall(std::uint64_t address, std::function<void(Machine&)> handler);
    StopReason Run(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit);
    void Exit(int code);
    void RequestStop();
    int ExitCode() const;
    static const char* Backend();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
