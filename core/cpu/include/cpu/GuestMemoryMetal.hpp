#pragma once

#include <cpu/GuestMemoryRuntime.hpp>
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"

namespace AgcDriver::Metal {
class MetalDriver;
}

namespace Cpu {

struct GuestMemoryMetalMappings {
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> Ranges;
    std::shared_ptr<const void> Owner;
};

GuestMemoryMetalMappings BorrowGuestMemoryForMetal(const GuestMemorySnapshot& snapshot,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> pinnedRanges = {},
    std::shared_ptr<const void> pinnedOwner = {});

GuestMemoryRuntime::Transaction MakeGuestMemoryMetalTransaction(AgcDriver::Metal::MetalDriver& driver,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> pinnedRanges = {},
    std::shared_ptr<const void> pinnedOwner = {});

// Composes explicitly selected genuine Machine-owned spans
// with one runtime's mappings. Selected spans must be CPU-readable; GPU writes
// require an exact selected start address and CPU Read|Write permission.
// Construct and use on the CPU owner. Construct the runtime with MakeTransaction,
// BindRuntime its freshly empty Snapshot, then configure the native session with
// InitialMappings and Generation before any runtime mutation. InitialMappings is
// only for this initial setup, before any later publication succeeds. Publisher must call
// SceNativeGraphicsSession::MutateBorrowedRanges synchronously exactly once.
// Machine/session must outlive their callbacks, and this compositor must outlive
// its transactions and graphics shutdown. Failed post-CPU publications retain
// both leases until compositor destruction. Fixed-selected mode validates frozen
// owned bindings. Live owned publication
// requires installing MakeOwnedTransaction on Machine before those mutations;
// its explicit selector must reproduce the initial selected views and policy.
class GuestMemoryMetalCompositor {
public:
    using Publisher = std::function<void(
        std::span<const AgcDriver::NativeGuestMemory::BorrowedRange>, std::uint64_t,
        const std::function<void()>&, std::shared_ptr<const void>, std::shared_ptr<const void>)>;
    enum class OwnedGpuAccess { CpuOnly, ReadOnly, ReadWrite };
    using OwnedSelector = std::function<OwnedGpuAccess(const OwnedMappingView&)>;
    GuestMemoryMetalCompositor(Machine& machine, OwnedMappingSnapshot selected,
        std::span<const std::uint64_t> writableOwnedAddresses = {});
    ~GuestMemoryMetalCompositor();
    GuestMemoryMetalCompositor(const GuestMemoryMetalCompositor&) = delete;
    GuestMemoryMetalCompositor& operator=(const GuestMemoryMetalCompositor&) = delete;
    void BindRuntime(const GuestMemorySnapshot& initial);
    GuestMemoryMetalMappings InitialMappings() const;
    std::uint64_t Generation() const;
    GuestMemoryRuntime::Transaction MakeTransaction(Publisher publisher);
    // Create while idle after BindRuntime, then install on Machine. The selector
    // only inspects genuine full views, including guards; CpuOnly excludes a view.
    // ReadOnly requires CPU Read and ReadWrite requires CPU Read|Write. All
    // owned mapping changes must use Machine's staged hook until graphics drain.
    Machine::OwnedMappingTransaction MakeOwnedTransaction(Publisher publisher, OwnedSelector selector);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
