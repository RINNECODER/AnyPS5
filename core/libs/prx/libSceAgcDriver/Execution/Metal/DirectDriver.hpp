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
// on a thread stack), and words outside guest memory (host-built packets such as VideoOut's flip,
// or words in CPU-only memory) are staged in a driver-owned range first, which grows to fit; the
// driver captures them during the call. Words that cannot be read (a null or unmapped address)
// are skipped with a log line naming `call`, the address and the size.
void DirectSubmit(const Packet& packet, std::uint32_t queue, const char* call = nullptr);

// The current guest allocations as identity ranges (sorted; adjacent ranges with the same access
// are merged, then split by SplitBorrowedRanges at a power of two under the device's buffer limit).
// Refreshed whenever the registry's generation changes.
std::shared_ptr<const std::vector<NativeGuestMemory::BorrowedRange>> DirectGuestRanges();

// Splits ranges at multiples of `limit` (a power of two), so no range is longer than `limit` and
// every range maps to one Metal buffer. A title's direct memory can be one mapping larger than the
// device's maxBufferLength.
std::vector<NativeGuestMemory::BorrowedRange> SplitBorrowedRanges(const std::vector<NativeGuestMemory::BorrowedRange>& ranges, std::uint64_t limit);

// Whether [address, address + bytes) lies in `ranges` (sorted, non-overlapping), possibly across
// adjacent ranges.
bool RangesCover(const std::vector<NativeGuestMemory::BorrowedRange>& ranges, std::uint64_t address, std::size_t bytes);

}
