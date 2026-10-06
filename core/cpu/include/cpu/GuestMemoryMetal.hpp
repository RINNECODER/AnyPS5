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

}
