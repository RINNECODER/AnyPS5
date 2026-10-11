#pragma once

#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <memory>
#include <vector>

// Rosetta direct path (fork-only, macOS x86_64): the guest runs natively, so a guest pointer is a
// host pointer. The Metal driver borrows the guest allocation registry (libc GuestAllocations) as
// identity ranges instead of the TCG engine's translated mappings.
namespace AgcDriver::Metal {

// The process-wide driver, configured on first use with the system default device, the utility
// metallib (ANYPS5_METAL_UTILITY_LIBRARY, else AnyPS5Utilities.metallib next to this library)
// and EOP interrupts delivered to the AGC event queues. Outside the driver's worker threads, each
// call also hands the driver the current guest allocations when they changed since the last call.
MetalDriver& DirectDriver();

// Submits the command words a descriptor names. The descriptor is read as a host pointer (it may be
// on a thread stack), and words outside guest memory (host-built packets such as VideoOut's flip)
// are staged in a driver-owned range first; the driver captures them during the call.
void DirectSubmit(const Packet& packet, std::uint32_t queue);

// The current guest allocations as identity ranges (sorted; adjacent ranges with the same access
// are merged). Refreshed whenever the registry's generation changes.
std::shared_ptr<const std::vector<NativeGuestMemory::BorrowedRange>> DirectGuestRanges();

}
