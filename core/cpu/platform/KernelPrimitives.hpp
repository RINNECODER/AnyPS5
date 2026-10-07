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
// This selects only the nine mutex contracts, never another component family.
class TargetKernelMutexes final {
public:
    TargetKernelMutexes(Machine&, const std::shared_ptr<GuestThreads>&);
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t observedSymbolType,
                                       KernelMutexConsumer);
private:
    KernelPrimitives provider;
};
}
