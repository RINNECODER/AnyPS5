#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AgcDriver::NativeGuestMemory {
namespace {
thread_local std::vector<BorrowedRange> ranges;
thread_local GuestMemory::ReadSite readSite = GuestMemory::ReadSite::Unknown;

std::uint64_t End(std::uint64_t address, std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Native guest memory range overflows");
    return address + bytes;
}

const BorrowedRange& Find(std::uint64_t address, bool writable) {
    auto found = std::upper_bound(ranges.begin(), ranges.end(), address,
        [](std::uint64_t value, const BorrowedRange& range) { return value < range.guestAddress; });
    if (found == ranges.begin()) throw std::out_of_range("Native guest memory address is not borrowed");
    --found;
    if (address >= End(found->guestAddress, found->host.size()))
        throw std::out_of_range("Native guest memory address is not borrowed");
    if (writable && !found->writable)
        throw std::invalid_argument("Native guest memory range is read-only");
    return *found;
}

void Check(std::uint64_t address, std::size_t bytes, std::size_t alignment, bool writable) {
    if (address == 0 || alignment == 0 || address % alignment != 0)
        throw std::invalid_argument("Native guest memory address has invalid alignment");
    const auto end = End(address, bytes);
    for (auto cursor = address; cursor < end;) {
        const auto& range = Find(cursor, writable);
        cursor = std::min(end, End(range.guestAddress, range.host.size()));
    }
}

template <typename Operation>
void Visit(std::uint64_t address, std::size_t bytes, Operation&& operation) {
    std::size_t copied = 0;
    while (copied < bytes) {
        const auto& range = Find(address + copied, false);
        const auto offset = static_cast<std::size_t>(address + copied - range.guestAddress);
        const auto count = std::min(bytes - copied, range.host.size() - offset);
        operation(range.host.subspan(offset, count), copied);
        copied += count;
    }
}
}

BorrowedRangesScope::BorrowedRangesScope(std::span<const BorrowedRange> borrowed) {
    std::vector<BorrowedRange> validated(borrowed.begin(), borrowed.end());
    std::sort(validated.begin(), validated.end(), [](const auto& a, const auto& b) { return a.guestAddress < b.guestAddress; });
    std::uint64_t previousEnd = 0;
    for (const auto& range : validated) {
        if (range.guestAddress == 0 || range.host.empty() || range.host.data() == nullptr)
            throw std::invalid_argument("Native guest memory borrow is empty or null");
        const auto end = End(range.guestAddress, range.host.size());
        if (range.guestAddress < previousEnd)
            throw std::invalid_argument("Native guest memory borrows overlap");
        previousEnd = end;
    }
    previous.swap(ranges);
    ranges.swap(validated);
}

BorrowedRangesScope::~BorrowedRangesScope() { ranges.swap(previous); }
std::span<std::byte> ContiguousBorrowedRange(std::uint64_t address, std::size_t bytes, bool writable) {
    Check(address, bytes, 1, writable);
    if (bytes == 0) return {};
    const auto& range = Find(address, writable);
    const auto offset = static_cast<std::size_t>(address - range.guestAddress);
    if (bytes > range.host.size() - offset)
        throw std::invalid_argument("Native guest memory range crosses borrowed spans");
    return range.host.subspan(offset, bytes);
}
}

namespace AgcDriver::GuestMemory {

void CheckRange(const void* pointer, std::size_t bytes, std::size_t alignment, bool writable) {
    NativeGuestMemory::Check(reinterpret_cast<std::uintptr_t>(pointer), bytes, alignment, writable);
}

bool Accessible(const void* pointer, std::size_t bytes, bool writable) {
    try {
        CheckRange(pointer, bytes, 1, writable);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void Read(std::uint64_t address, std::span<std::byte> destination, std::size_t alignment) {
    NativeGuestMemory::Check(address, destination.size(), alignment, false);
    NativeGuestMemory::Visit(address, destination.size(), [&](std::span<std::byte> source, std::size_t offset) {
        std::memmove(destination.data() + offset, source.data(), source.size());
    });
}

void Write(std::uint64_t address, std::span<const std::byte> source, std::size_t alignment) {
    NativeGuestMemory::Check(address, source.size(), alignment, true);
    NativeGuestMemory::Visit(address, source.size(), [&](std::span<std::byte> destination, std::size_t offset) {
        std::memmove(destination.data(), source.data() + offset, destination.size());
    });
}

ReadSite SetReadSite(ReadSite site) {
    const auto previous = NativeGuestMemory::readSite;
    NativeGuestMemory::readSite = site;
    return previous;
}

ReadSite CurrentReadSite() { return NativeGuestMemory::readSite; }

}
