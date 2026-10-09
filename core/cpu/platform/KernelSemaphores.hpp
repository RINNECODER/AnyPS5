#pragma once
#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Cpu { struct SceImport; class GuestThreads; }
namespace Cpu::Platform {
struct KernelSemaphoreImport { std::string_view Nid, Name; };
std::span<const KernelSemaphoreImport> KernelSemaphoreInventory();

// Explicit engineering provider: opaque guest64 identity, signed low32 counts,
// attr0 priority snapshot, null options and indefinite null-timeout waits.
// This is not a universal firmware ABI. Poll/Cancel and timed waits are unknown.
class KernelSemaphores {
public:
    // Machine and GuestThreads must outlive this owner-only provider.
    KernelSemaphores(Machine&, const std::shared_ptr<GuestThreads>&,
                     std::uint64_t gateBase = 0x7ffdc3000000);
    ~KernelSemaphores();
    KernelSemaphores(const KernelSemaphores&) = delete;
    KernelSemaphores& operator=(const KernelSemaphores&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType,
                                       std::uint64_t symbolSize = 0);
    // Owner-idle boundary only. Abandons parked/queued calls through the
    // scheduler's terminal withdrawal, never a fabricated semaphore return.
    void Shutdown();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

// Production selection: admits the four semaphore rows for any importing image
// by NID, libkernel scope/version, symbol type and size.
class TargetKernelSemaphores {
public:
    TargetKernelSemaphores(Machine&, const std::shared_ptr<GuestThreads>&);
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t type, std::uint64_t size);
    KernelSemaphores& Provider() { return provider; }
private:
    KernelSemaphores provider;
};
}
