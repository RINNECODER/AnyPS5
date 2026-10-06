#include <cpu/GuestMemoryMetal.hpp>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Cpu {
namespace {

struct MappingLease {
    GuestMemorySnapshot snapshot;
    std::shared_ptr<const void> pinnedOwner;
};

void pinnedLifetime(std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
                    const std::shared_ptr<const void>& owner) {
    if (!ranges.empty() && !owner)
        throw std::invalid_argument("Pinned Metal guest ranges require their backing owner");
}

}

GuestMemoryMetalMappings BorrowGuestMemoryForMetal(const GuestMemorySnapshot& snapshot,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> pinnedRanges,
    std::shared_ptr<const void> pinnedOwner) {
    pinnedLifetime(pinnedRanges, pinnedOwner);
    if (!snapshot.Views.empty() && snapshot.Owners.empty())
        throw std::invalid_argument("Metal guest memory snapshot has no backing owners");
    for (const auto& owner : snapshot.Owners) if (!owner)
        throw std::invalid_argument("Metal guest memory snapshot contains an empty backing owner");
    GuestMemoryMetalMappings result;
    result.Ranges.reserve(pinnedRanges.size() + snapshot.Views.size());
    result.Ranges.insert(result.Ranges.end(), pinnedRanges.begin(), pinnedRanges.end());
    for (const auto& view : snapshot.Views) {
        if (view.Protection & ~0x37u)
            throw std::invalid_argument("Unsupported Metal guest memory protection bits");
        const auto gpu = view.Protection & 0x30u;
        if (gpu == 0x20u)
            throw std::invalid_argument("GPU write-only guest memory is not representable by Metal borrowed ranges");
        if (!gpu) continue;
        result.Ranges.push_back({view.Address, view.Bytes, (gpu & 0x20u) != 0, view.Identity});
    }
    std::sort(result.Ranges.begin(), result.Ranges.end(), [](const auto& a, const auto& b) {
        return a.guestAddress < b.guestAddress;
    });
    {
        AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(result.Ranges);
    }
    result.Owner = std::make_shared<MappingLease>(MappingLease{snapshot, std::move(pinnedOwner)});
    return result;
}

GuestMemoryRuntime::Transaction MakeGuestMemoryMetalTransaction(AgcDriver::Metal::MetalDriver& driver,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> pinnedRanges,
    std::shared_ptr<const void> pinnedOwner) {
    pinnedLifetime(pinnedRanges, pinnedOwner);
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> pinned(pinnedRanges.begin(), pinnedRanges.end());
    {
        AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(pinned);
    }
    return [&driver, pinned = std::move(pinned), owner = std::move(pinnedOwner)](
        const GuestMemorySnapshot& previous, const GuestMemorySnapshot& next,
        const std::function<void()>& mutateCpu) {
        auto before = BorrowGuestMemoryForMetal(previous, pinned, owner);
        auto after = BorrowGuestMemoryForMetal(next, pinned, owner);
        driver.MutateBorrowedRanges(after.Ranges, next.Generation, mutateCpu,
            std::move(before.Owner), std::move(after.Owner));
    };
}

}
