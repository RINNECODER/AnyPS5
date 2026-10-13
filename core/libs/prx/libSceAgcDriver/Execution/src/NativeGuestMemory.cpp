#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <cstdio>

namespace AgcDriver::NativeGuestMemory {
namespace {
thread_local std::vector<BorrowedRange> scopedRanges;
thread_local GuestMemory::ReadSite readSite = GuestMemory::ReadSite::Unknown;
std::atomic<ProcessRangesProvider> processRanges{nullptr};
thread_local std::shared_ptr<const std::vector<BorrowedRange>> processSnapshot;

// The borrowed scope of this thread, or the process-wide set outside any scope. References into
// the process-wide set stay valid until this thread asks again.
const std::vector<BorrowedRange>& Active() {
    if (!scopedRanges.empty()) return scopedRanges;
    const auto provider = processRanges.load(std::memory_order_acquire);
    if (provider == nullptr) return scopedRanges;
    processSnapshot = provider();
    return processSnapshot ? *processSnapshot : scopedRanges;
}

std::uint64_t End(std::uint64_t address, std::size_t bytes) {
    if (bytes > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Native guest memory range overflows");
    return address + bytes;
}

[[noreturn]] void NotBorrowed(std::uint64_t address) {
    char message[96];
    std::snprintf(message, sizeof(message), "Native guest memory address %#llx is not borrowed", static_cast<unsigned long long>(address));
    throw std::out_of_range(message);
}

const BorrowedRange& Find(std::uint64_t address, bool writable) {
    const auto& ranges = Active();
    auto found = std::upper_bound(ranges.begin(), ranges.end(), address,
        [](std::uint64_t value, const BorrowedRange& range) { return value < range.guestAddress; });
    if (found == ranges.begin()) NotBorrowed(address);
    --found;
    if (address >= End(found->guestAddress, found->host.size())) NotBorrowed(address);
    if (writable && !found->writable)
        throw ReadOnlyWriteError("Native guest memory range is read-only");
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
        const auto hostAddress = reinterpret_cast<std::uintptr_t>(range.host.data());
        if (range.host.size() > std::numeric_limits<std::uintptr_t>::max() - hostAddress)
            throw std::invalid_argument("Native guest memory host range overflows");
        const auto end = End(range.guestAddress, range.host.size());
        if (range.guestAddress < previousEnd)
            throw std::invalid_argument("Native guest memory borrows overlap");
        previousEnd = end;
    }
    previous.swap(scopedRanges);
    scopedRanges.swap(validated);
}

BorrowedRangesScope::~BorrowedRangesScope() { scopedRanges.swap(previous); }

void SetProcessRangesProvider(ProcessRangesProvider provider) { processRanges.store(provider, std::memory_order_release); }
std::span<std::byte> ContiguousBorrowedRange(std::uint64_t address, std::size_t bytes, bool writable) {
    Check(address, bytes, 1, writable);
    if (bytes == 0) return {};
    const auto& range = Find(address, writable);
    const auto offset = static_cast<std::size_t>(address - range.guestAddress);
    auto* host = range.host.data() + offset;
    // A range may continue into the next borrowed span (separately mapped, adjacent guest memory)
    // when that span also follows on in host memory: the bytes are then one host run.
    auto guest = End(range.guestAddress, range.host.size());
    auto* hostEnd = range.host.data() + range.host.size();
    const auto end = address + bytes;
    while (guest < end) {
        const auto& next = Find(guest, writable);
        if (next.guestAddress != guest || next.host.data() != hostEnd)
            throw std::invalid_argument("Native guest memory range crosses borrowed spans that are not contiguous on the host");
        guest = End(next.guestAddress, next.host.size());
        hostEnd = next.host.data() + next.host.size();
    }
    return {host, bytes};
}

std::size_t ReadableBorrowedBytes(std::uint64_t address, std::size_t maxBytes) {
    if (address == 0) throw std::invalid_argument("Native guest memory address is null");
    const auto end = End(address, maxBytes);
    auto cursor = address;
    for (const auto& range : Active()) {
        const auto rangeEnd = End(range.guestAddress, range.host.size());
        if (rangeEnd <= cursor) continue;
        if (range.guestAddress > cursor) break;
        cursor = std::min(end, rangeEnd);
        if (cursor == end) break;
    }
    return static_cast<std::size_t>(cursor - address);
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

// Native guest memory keeps no deferred GPU writes: Read and Write go straight to the mapped ranges.
void FlushGpuWrites(std::uint64_t, std::size_t) {}

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
