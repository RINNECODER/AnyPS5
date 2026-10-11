#pragma once
#include <cpu/Cpu.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Cpu { struct SceImport; struct SceModuleRecord; class GuestThreads; }
namespace Cpu::Platform {
struct KernelThreadExtraImport { std::string_view Nid, Name; };
std::span<const KernelThreadExtraImport> KernelThreadExtraInventory();

// Title-agnostic libkernel rows that libc and titles use beside mutexes and
// conditions: rwlocks (FreeBSD libthr writer preference, recursive readers),
// pthread once, TSD keys with destructors run at thread exit, SyncOnAddress
// futex waits, module information for unwinding, and debug exceptions.
//
// Guest words hold handle values: 0 is the static initializer, 1 is the
// destroyed sentinel. Every failure is an SCE error code returned to the
// guest; guest pointers are only reached through Machine reads and writes.
// Once and TSD destructors call guest code through a provider-owned guest
// code page that chains host gates, so no host->guest callback is nested.
class KernelThreadExtras {
public:
    using ModuleSource = std::function<std::span<const SceModuleRecord>()>;
    // Machine must outlive this provider; destroy only while guest execution is idle.
    // The provider installs the scheduler's thread exit hook (one provider per scheduler).
    KernelThreadExtras(Machine&, const std::shared_ptr<GuestThreads>&,
                       std::uint64_t base = 0x7ffdc9000000);
    ~KernelThreadExtras();
    KernelThreadExtras(const KernelThreadExtras&) = delete;
    KernelThreadExtras& operator=(const KernelThreadExtras&) = delete;
    // Loaded modules for sceKernelGetModuleInfoFromAddr/ForUnwind, queried at call time.
    void SetModuleSource(ModuleSource);
    // Validates libkernel scope, version, FUNC type and zero size per row.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType, std::uint64_t symbolSize);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
