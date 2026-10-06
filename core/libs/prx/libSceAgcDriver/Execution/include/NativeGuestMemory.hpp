#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver::NativeGuestMemory {

struct BorrowedRange {
    std::uint64_t guestAddress;
    std::span<std::byte> host;
    bool writable;
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
