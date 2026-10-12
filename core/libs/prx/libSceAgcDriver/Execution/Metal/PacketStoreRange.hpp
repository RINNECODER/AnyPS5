#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <limits>
#include <optional>
#include <span>

namespace AgcDriver::Metal {

// Guest bytes a PM4 packet that the driver executes on the CPU may store to (Pm4::Execute's
// RELEASE_MEM, CONST_RAM dump, COND_WRITE, WRITE_DATA, COPY_DATA and DMA_DATA). The bound is
// conservative: a destination the packet does not actually write (a register copy, a GDS or
// "nowhere" DMA) only costs a cache entry. A packet too short to decode reports all memory.
struct GuestStoreRange {
    std::uint64_t begin = 0;
    std::uint64_t bytes = 0;

    static constexpr GuestStoreRange Everything() { return {0, std::numeric_limits<std::uint64_t>::max()}; }
    [[nodiscard]] bool Overlaps(std::uint64_t otherBegin, std::uint64_t otherBytes) const {
        if (bytes == 0 || otherBytes == 0) return false;
        const auto end = begin + bytes < begin ? std::numeric_limits<std::uint64_t>::max() : begin + bytes;
        const auto otherEnd = otherBegin + otherBytes < otherBegin ? std::numeric_limits<std::uint64_t>::max() : otherBegin + otherBytes;
        return begin < otherEnd && otherBegin < end;
    }
};

// std::nullopt: the packet is not one that stores to guest memory.
inline std::optional<GuestStoreRange> PacketStoreRange(std::span<const std::uint32_t> packet) {
    if (packet.empty()) return std::nullopt;
    const auto at = [&](std::size_t low) {
        return static_cast<std::uint64_t>(packet[low]) | (static_cast<std::uint64_t>(packet[low + 1]) << 32u);
    };
    switch ((packet[0] >> 8u) & 0xffu) {
        case 0x49:  // RELEASE_MEM: 4 data bytes with DATA_SEL 1, 8 with 2 or 3, none with 0.
            if (packet.size() < 5) return GuestStoreRange::Everything();
            if ((packet[2] >> 29u) == 0 || at(3) == 0) return GuestStoreRange{};
            return GuestStoreRange{at(3), (packet[2] >> 29u) == 1 ? 4u : 8u};
        case 0x83:  // DUMP_CONST_RAM: packet[2] dwords.
            if (packet.size() < 5) return GuestStoreRange::Everything();
            return GuestStoreRange{at(3), std::uint64_t{packet[2]} * 4u};
        case 0x45:  // COND_WRITE: one dword.
            if (packet.size() < 8) return GuestStoreRange::Everything();
            return GuestStoreRange{at(6), 4};
        case 0x37:  // WRITE_DATA: the payload, or one dword with WR_ONE_ADDR.
            if (packet.size() < 4) return GuestStoreRange::Everything();
            return GuestStoreRange{at(2), (packet[1] & 0x10000u) != 0 ? 4u : std::uint64_t{packet.size() - 4} * 4u};
        case 0x40:  // COPY_DATA: 4 or 8 bytes.
            if (packet.size() < 6) return GuestStoreRange::Everything();
            return GuestStoreRange{at(4), 8};
        case 0x50:  // DMA_DATA: BYTE_COUNT bytes.
            if (packet.size() < 7) return GuestStoreRange::Everything();
            return GuestStoreRange{at(4), packet[6] & 0x3ffffffu};
        default:
            return std::nullopt;
    }
}

// The bytes this driver itself last stored with small CPU-executed stores (labels and fences:
// RELEASE_MEM, WRITE_DATA, COPY_DATA). A WAIT_REG_MEM whose memory still holds what the driver
// stored there was released by earlier GPU work of this submission, not by the guest CPU, so
// guest memory the cache describes cannot have changed behind it.
class OwnLabelStores {
public:
    static constexpr std::size_t MaxBytes = 4096;

    void Record(std::uint64_t address, std::span<const std::byte> stored) {
        if (bytes.size() + stored.size() > MaxBytes) bytes.clear();
        for (std::size_t i = 0; i < stored.size(); ++i) bytes[address + i] = stored[i];
    }

    // Whether every byte of `current` (read at `address`) is what the driver last stored there.
    [[nodiscard]] bool Holds(std::uint64_t address, std::span<const std::byte> current) const {
        if (current.empty()) return false;
        for (std::size_t i = 0; i < current.size(); ++i) {
            const auto found = bytes.find(address + i);
            if (found == bytes.end() || found->second != current[i]) return false;
        }
        return true;
    }

    void Reset() { bytes.clear(); }

private:
    std::unordered_map<std::uint64_t, std::byte> bytes;
};

}
