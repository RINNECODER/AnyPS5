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

// Composes a fixed, explicitly selected set of genuine Machine-owned spans
// with one runtime's mappings. Selected spans must be CPU-readable; GPU writes
// require an exact selected start address and CPU Read|Write permission.
// Construct and use on the CPU owner. Construct the runtime with MakeTransaction,
// BindRuntime its freshly empty Snapshot, then configure the native session with
// InitialMappings and Generation before any runtime mutation. InitialMappings is
// only for this initial setup, before a dynamic publication succeeds. Publisher must call
// SceNativeGraphicsSession::MutateBorrowedRanges synchronously exactly once.
// Machine/session must outlive their callbacks, and this compositor must outlive
// its transactions and graphics shutdown. Failed post-CPU publications retain
// both leases until compositor destruction. This validates frozen owned bindings;
// it does not provide atomic live mutation of selected Machine-owned mappings.
class GuestMemoryMetalCompositor {
public:
    using Publisher = std::function<void(
        std::span<const AgcDriver::NativeGuestMemory::BorrowedRange>, std::uint64_t,
        const std::function<void()>&, std::shared_ptr<const void>, std::shared_ptr<const void>)>;
    GuestMemoryMetalCompositor(Machine& machine, OwnedMappingSnapshot selected,
        std::span<const std::uint64_t> writableOwnedAddresses = {});
    ~GuestMemoryMetalCompositor();
    GuestMemoryMetalCompositor(const GuestMemoryMetalCompositor&) = delete;
    GuestMemoryMetalCompositor& operator=(const GuestMemoryMetalCompositor&) = delete;
    void BindRuntime(const GuestMemorySnapshot& initial);
    GuestMemoryMetalMappings InitialMappings() const;
    std::uint64_t Generation() const;
    GuestMemoryRuntime::Transaction MakeTransaction(Publisher publisher);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
