#include "aot_cpp/guest_regions.hpp"

#include <algorithm>
#include <utility>

namespace aot_cpp {
namespace {

using u128 = unsigned __int128;

std::int64_t page_floor(u128 value) {
    return static_cast<std::int64_t>((value / kPageSize) * kPageSize);
}

std::int64_t page_ceil(u128 value) {
    return static_cast<std::int64_t>(((value + kPageSize - 1) / kPageSize) * kPageSize);
}

[[noreturn]] void fail(const std::string& message) { throw GuestRegionError(message); }

}  // namespace

std::vector<GuestRange> immutable_ranges(const std::vector<GuestSegment>& loads,
                                         std::int64_t relro_start, std::int64_t relro_size) {
    std::vector<std::uint64_t> boundaries;
    std::vector<std::pair<std::int64_t, std::int64_t>> mapped;
    std::vector<std::pair<std::int64_t, std::int64_t>> writable;
    for (const GuestSegment& segment : loads) {
        if (!segment.memsz || !(segment.flags & 7)) {
            continue;
        }
        if (segment.vaddr < 0 || segment.memsz < 0 ||
            static_cast<u128>(segment.vaddr) + static_cast<u128>(segment.memsz) >= kGuestLimit) {
            fail("invalid guest segment range");
        }
        std::int64_t first = page_floor(static_cast<u128>(segment.vaddr));
        std::int64_t last = page_ceil(static_cast<u128>(segment.vaddr) + static_cast<u128>(segment.memsz));
        mapped.emplace_back(first, last);
        if (segment.flags & 2) {
            writable.emplace_back(first, last);
        }
        boundaries.push_back(static_cast<std::uint64_t>(first));
        boundaries.push_back(static_cast<std::uint64_t>(last));
    }
    if (relro_start < 0 || relro_size < 0 ||
        static_cast<u128>(relro_start) + static_cast<u128>(relro_size) >= kGuestLimit) {
        fail("invalid guest RELRO range");
    }
    std::int64_t relro_low = page_floor(static_cast<u128>(relro_start) + kPageSize - 1);
    std::int64_t relro_high = page_floor(static_cast<u128>(relro_start) + static_cast<u128>(relro_size));
    if (relro_low < relro_high) {
        boundaries.push_back(static_cast<std::uint64_t>(relro_low));
        boundaries.push_back(static_cast<std::uint64_t>(relro_high));
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    auto inside = [](const std::vector<std::pair<std::int64_t, std::int64_t>>& spans,
                     std::int64_t low, std::int64_t high) {
        return std::any_of(spans.begin(), spans.end(), [low, high](const auto& span) {
            return span.first <= low && high <= span.second;
        });
    };

    std::vector<GuestRange> result;
    for (std::size_t index = 0; index + 1 < boundaries.size(); ++index) {
        std::int64_t low = static_cast<std::int64_t>(boundaries[index]);
        std::int64_t high = static_cast<std::int64_t>(boundaries[index + 1]);
        if (!inside(mapped, low, high)) {
            continue;
        }
        if (inside(writable, low, high) && !(relro_low <= low && high <= relro_high)) {
            continue;
        }
        if (!result.empty() && result.back().high == low) {
            result.back().high = high;
        } else {
            result.push_back({low, high});
        }
    }
    return result;
}

std::int64_t available_bytes(const std::vector<GuestRange>& ranges, std::int64_t address) {
    for (const GuestRange& range : ranges) {
        if (range.low <= address && address < range.high) {
            return range.high - address;
        }
    }
    return 0;
}

}  // namespace aot_cpp
