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

struct KernelMutexConsumer {
    std::string_view Name;
    std::string_view Sha256;
};

// Engineering qualification for the observed PPSA04203 consumer import rows.
// The loader must compute the consumer hash from its actual input before calling
// Resolve. Protocol 1 uses the actual guest scheduler's live waiter inheritance
// and effective-priority ordering (FIFO ties). Protocol 2 and abandoned-owner
// recovery remain unsupported. Direct opaque pointee/copy-slot access is not
// admitted by these contracts.
// Selects the nine SCE mutex contracts and the exact pinned libc POSIX lock/
// unlock pair. Its observed zero static initializer uses type1/protocol0;
// that default is public-source engineering inference, not vendor certification.
class TargetKernelMutexes final {
public:
    TargetKernelMutexes(Machine&, const std::shared_ptr<GuestThreads>&);
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t observedSymbolType,
                                       KernelMutexConsumer);
    // Shares this exact provider's mutex ownership and wait domain. Only the
    // pinned untimed/default-attribute rows and two observed timed rows are
    // admitted. Absolute POSIX time is signed seconds/nanoseconds on default
    // realtime, compared each owner turn; relative SCE time is unsigned32
    // microseconds on a saturated steady deadline. Both reacquire the same
    // mutex before returning.
    // The caller must supply the actual imported symbol size; clock setters and
    // arbitrary attributes remain outside this engineering profile.
    std::optional<std::uint64_t> ResolveCondition(const SceImport&, std::uint8_t observedSymbolType,
                                                std::uint64_t observedSymbolSize, KernelMutexConsumer);
private:
    KernelPrimitives provider;
};
}
