// C++ port of core/aot/guest_regions.py: the pure guest page-range helpers.
//
// The whole-image `sealed_tables` scan couples to capstone and the unit model, so this port
// covers the two total functions the compiler uses to classify immutable guest ranges.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace aot_cpp {

constexpr std::int64_t kPageSize = 16384;
constexpr unsigned __int128 kGuestLimit = static_cast<unsigned __int128>(1) << 64;

struct GuestRegionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GuestSegment {
    std::int64_t vaddr = 0;
    std::int64_t memsz = 0;
    std::int64_t flags = 0;
};

struct GuestRange {
    std::int64_t low = 0;
    std::int64_t high = 0;
};

std::vector<GuestRange> immutable_ranges(const std::vector<GuestSegment>& loads,
                                         std::int64_t relro_start, std::int64_t relro_size);
std::int64_t available_bytes(const std::vector<GuestRange>& ranges, std::int64_t address);

}  // namespace aot_cpp
