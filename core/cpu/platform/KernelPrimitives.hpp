#pragma once
#include <cpu/Cpu.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Cpu { struct SceImport; class GuestThreads; }
namespace Cpu::Platform {
// Public PS4 contract inventory. Target selection additionally requires the
// pinned consumer admission below; fixture construction alone admits no target.
struct KernelPrimitiveImport { std::string_view Nid, Name; };
std::span<const KernelPrimitiveImport> KernelPrimitiveInventory();
std::span<const KernelPrimitiveImport> KernelConditionInventory();
class KernelPrimitives {
public:
    // Machine must outlive this provider. Destroy only while guest execution is idle;
    // the provider exclusively owns its gate page and unmaps it on destruction.
    // Must identify the active guest thread, never the host thread.
    KernelPrimitives(Machine&, std::function<std::uint64_t()> activeGuestThread,
                     std::uint64_t gateBase = 0x7ffdc1000000);
    KernelPrimitives(Machine&, const std::shared_ptr<GuestThreads>&,
                     std::uint64_t gateBase = 0x7ffdc1000000);
    ~KernelPrimitives();
    KernelPrimitives(const KernelPrimitives&) = delete;
    KernelPrimitives& operator=(const KernelPrimitives&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

// Title-agnostic engineering profile: rows are admitted by NID, libkernel
// scope/version, symbol type and size for any importing image. Protocol 1 uses the actual guest scheduler's live waiter inheritance
// and effective-priority ordering (FIFO ties). Protocol 2 remains unsupported.
// Mutex and condition semantics follow FreeBSD libthr: the guest word holds a
// handle value (copies name the same object), 0 and 1 (adaptive mutex) are lazy
// static initializers, 2 (mutex) and 1 (condition) are the destroyed sentinels,
// init always overwrites, and an owner that exits keeps a non-robust mutex owned.
// Selects the nine SCE mutex contracts, scePthreadMutexTimedlock and the POSIX
// lock/unlock pair. The zero static initializer uses type1/protocol0;
// that default is public-source engineering inference, not vendor certification.
class TargetKernelMutexes final {
public:
    TargetKernelMutexes(Machine&, const std::shared_ptr<GuestThreads>&);
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t observedSymbolType);
    // Shares this exact provider's mutex ownership and wait domain. Condition
    // rows, the two timed waits and the two condattr clock setters are admitted.
    // Absolute POSIX time is signed seconds/nanoseconds on the condition's clock
    // (realtime by default, or monotonic via setclock), compared each owner turn;
    // relative SCE time is unsigned32 microseconds on a saturated steady
    // deadline. Both reacquire the same mutex before returning.
    // The caller must supply the actual imported symbol size.
    std::optional<std::uint64_t> ResolveCondition(const SceImport&, std::uint8_t observedSymbolType,
                                                std::uint64_t observedSymbolSize);
private:
    KernelPrimitives provider;
};
}
