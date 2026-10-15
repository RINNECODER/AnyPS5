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
// Applies the process working-set mode DirectDriver configures with (ANYPS5_METAL_WORKING_SET_MIB,
// default 3 GiB in 4 MiB intervals); the frame replay calls it to bind memory as the game does.
void ApplyDirectWorkingSet();

// Submits the command words a descriptor names. The descriptor is read as a host pointer (it may be
// on a thread stack), and words outside guest memory (host-built packets such as VideoOut's flip,
// or words in CPU-only memory) are staged in a driver-owned range first, which grows to fit; the
// driver captures them during the call. A staged copy is retaken while its packets do not parse
// (a title still writing the buffer), and a staged buffer holding a REWIND is skipped (the driver
// would read its tail later from the reused staging buffer). Words that cannot be read (a null or
// unmapped address) or fail PM4 validation are skipped with a log line naming `call`, the address,
// the size, whether the words were staged and the failing header and DWORD offset;
// ANYPS5_AGC_STRICT_SUBMISSIONS=1 makes a validation failure abort instead.
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
