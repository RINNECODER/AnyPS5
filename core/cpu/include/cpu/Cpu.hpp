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
enum class StopReason { Address, Exit, InstructionLimit, Requested, Paused };
struct Mapping {
    std::uint64_t Address;
    std::size_t Size;
    Permission Permissions;
    bool Borrowed;
};
struct OwnedMappingView {
    Mapping Region;
    std::span<std::byte> Bytes;
    std::span<std::byte> Allocation;
    std::uint64_t BackingIdentity;
    std::shared_ptr<void> Owner;
};
struct OwnedMappingSnapshot {
    std::shared_ptr<const void> Scope;
    std::uint64_t Generation;
    std::vector<OwnedMappingView> Views;
};

class Machine {
public:
    class Context {
    public:
        Context();
        ~Context();
        Context(Context&&) noexcept;
        Context& operator=(Context&&) noexcept;
        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
    private:
        friend class Machine;
        struct Payload;
        explicit Context(std::unique_ptr<Payload> value);
        std::unique_ptr<Payload> payload;
    };
    class SuspendedCall {
    public:
        SuspendedCall();
        ~SuspendedCall();
        SuspendedCall(SuspendedCall&&) noexcept;
        SuspendedCall& operator=(SuspendedCall&&) noexcept;
        SuspendedCall(const SuspendedCall&) = delete;
        SuspendedCall& operator=(const SuspendedCall&) = delete;
    private:
        friend class Machine;
        struct Payload;
        explicit SuspendedCall(std::unique_ptr<Payload> value);
        std::unique_ptr<Payload> payload;
    };
    Machine();
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    void Map(std::uint64_t address, std::size_t size, Permission permissions);
    void MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions);
    void MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions,
                     std::span<std::byte> fullBacking);
    void Unmap(std::uint64_t address, std::size_t size);
    void ReplaceBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions);
    void Protect(std::uint64_t address, std::size_t size, Permission permissions);
    void ProtectFragment(std::uint64_t address, std::size_t size, Permission permissions);
    std::vector<Mapping> Mappings() const;
    // Capture on the idle CPU owner. Descriptors describe this generation;
    // bytes remain live. Pins retain allocations, not guest address bindings:
    // callers must synchronize publication and retirement with CPU mutations.
    // Borrowed aliases into owned storage are included; external storage is not.
    OwnedMappingSnapshot PinOwnedMappings() const;
    // Owner-only scope lookup and binding validation also work during an owner
    // host callback. Unrelated mapping generations do not invalidate selection.
    std::shared_ptr<const void> OwnedMappingScope() const;
    void ValidateOwnedMappings(const OwnedMappingSnapshot& snapshot) const;
    using OwnedMappingTransaction = std::function<void(const OwnedMappingSnapshot&, const OwnedMappingSnapshot&,
                                                       const std::function<void()>&)>;
    // Install/remove on the idle owner. Remove only after graphics has drained.
    // A transaction must commit exactly once, synchronously on that owner.
    void SetOwnedMappingTransaction(OwnedMappingTransaction transaction);
    // Only the exact private stage of an active, not-yet-committed transaction
    // is accepted; public snapshot metadata alone cannot authorize a candidate.
    void ValidateOwnedMappingCandidate(const OwnedMappingSnapshot& candidate) const;
    void ValidateOwnedMappingCandidate(const OwnedMappingSnapshot& previous,
                                      const OwnedMappingSnapshot& candidate) const;
    void CheckAccess(std::uint64_t address, std::size_t size, Permission permissions) const;
    void Read(std::uint64_t address, std::span<std::byte> output) const;
    void Write(std::uint64_t address, std::span<const std::byte> input);
    std::uint64_t Get(Register reg) const;
    void Set(Register reg, std::uint64_t value);
    Context CaptureContext();
    void SaveContext(Context& context);
    void RestoreContext(const Context& context);
    SuspendedCall PauseHostCall();
    void ValidateSuspendedCall(const SuspendedCall& call) const;
    void CompleteHostCall(SuspendedCall& call);
    void SetSyscallHandler(std::function<void(Machine&)> handler);
    void AddHostCall(std::uint64_t address, std::function<void(Machine&)> handler);
    StopReason Run(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit);
    StopReason RunSlice(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit);
    std::uint64_t LastRunInstructions() const;
    void Exit(int code);
    void RequestStop();
    int ExitCode() const;
    static const char* Backend();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
