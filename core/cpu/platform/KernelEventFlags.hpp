#pragma once
#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Cpu { struct SceImport; class GuestThreads; }
namespace Cpu::Platform {
struct KernelEventFlagImport { std::string_view Nid, Name; };
std::span<const KernelEventFlagImport> KernelEventFlagInventory();

// libkernel event flags: Create/Delete/Set/Clear/Wait/Poll/Cancel with the
// AND/OR match and CLEAR_ALL/CLEAR_PAT consume modes, single or multiple
// waiters, FIFO or base-priority wake order and microsecond timed waits.
// Handles are opaque guest64 identities that are never reused. Blocked waits
// park in the guest scheduler, so other guest threads keep running and a host
// stop request abandons them without a fabricated return.
class KernelEventFlags {
public:
    // Machine and GuestThreads must outlive this owner-only provider.
    KernelEventFlags(Machine&, const std::shared_ptr<GuestThreads>&,
                     std::uint64_t gateBase = 0x7ffdc5000000);
    ~KernelEventFlags();
    KernelEventFlags(const KernelEventFlags&) = delete;
    KernelEventFlags& operator=(const KernelEventFlags&) = delete;
    // Admits the inventory rows by NID for libkernel 1/1.1 FUNC imports of any
    // importing image; rejects a wrong scope, type or size before allocating.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType,
                                         std::uint64_t symbolSize = 0);
    // Owner-idle boundary only. Abandons parked calls through the scheduler's
    // terminal withdrawal, never a fabricated event flag return.
    void Shutdown();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
