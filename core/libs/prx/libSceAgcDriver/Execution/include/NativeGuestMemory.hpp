#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace AgcDriver::NativeGuestMemory {

// A guest write targets memory the guest mapped read-only: a memory-protection
// fault, the bind-time twin of a GPU permission fault. The Metal driver keeps it
// sticky instead of skipping the packet, so such writes are never dropped silently.
class ReadOnlyWriteError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

struct BorrowedRange {
    std::uint64_t guestAddress;
    std::span<std::byte> host;
    bool writable;
    std::uint64_t identity = 0;
};

class BorrowedRangesScope {
public:
    explicit BorrowedRangesScope(std::span<const BorrowedRange> ranges);
    ~BorrowedRangesScope();
    BorrowedRangesScope(const BorrowedRangesScope&) = delete;
    BorrowedRangesScope& operator=(const BorrowedRangesScope&) = delete;

private:
    std::vector<BorrowedRange> previous;
};

std::span<std::byte> ContiguousBorrowedRange(std::uint64_t address, std::size_t bytes, bool writable = false);
std::size_t ReadableBorrowedBytes(std::uint64_t address, std::size_t maxBytes);

}
