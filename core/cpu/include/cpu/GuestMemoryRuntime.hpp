#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace Cpu {

class GuestMemoryError : public std::runtime_error {
public:
    GuestMemoryError(unsigned error, const std::string& message);
    std::int64_t Result() const;
private:
    std::int64_t result;
};

struct GuestPhysicalExtent { std::uint64_t Address; std::uint64_t Size; };
struct GuestMemoryView {
    std::uint64_t Address;
    std::span<std::byte> Bytes;
    std::uint32_t Protection;
    std::uint64_t Identity;
    std::uint64_t PhysicalId;
    std::uint64_t PhysicalOffset;
};
struct GuestMemorySnapshot {
    std::uint64_t Generation;
    std::vector<GuestMemoryView> Views;
    std::vector<std::shared_ptr<void>> Owners;
};
struct GuestMemoryQuery {
    std::uint64_t Start;
    std::uint64_t End;
    std::uint64_t Offset;
    std::uint32_t Protection;
    std::int32_t MemoryType;
    std::uint32_t Flags;
    std::array<char, 32> Name{};
    std::uint8_t GpuMaskId = 0;
    std::array<std::byte, 72> Serialize() const;
};

class GuestMemoryRuntime {
public:
    using Transaction = std::function<void(const GuestMemorySnapshot&, const GuestMemorySnapshot&,
                                           const std::function<void()>&)>;
    static constexpr std::uint64_t PageSize = 0x4000;
    GuestMemoryRuntime(Machine& machine, std::uint64_t directCapacity, Transaction transaction = {});
    ~GuestMemoryRuntime();
    GuestMemoryRuntime(const GuestMemoryRuntime&) = delete;
    GuestMemoryRuntime& operator=(const GuestMemoryRuntime&) = delete;
    std::uint64_t DirectMemorySize() const;
    std::uint64_t AllocateDirect(std::int64_t searchStart, std::int64_t searchEnd, std::uint64_t size,
                                 std::uint64_t alignment, std::int32_t memoryType);
    GuestPhysicalExtent AvailableDirect(std::int64_t searchStart, std::int64_t searchEnd,
                                         std::uint64_t alignment) const;
    std::uint64_t MapDirect(std::uint64_t hint, std::uint64_t size, std::uint32_t protection,
                            std::uint32_t flags, std::int64_t physical, std::uint64_t alignment,
                            std::uint64_t outputAddress = 0);
    std::uint64_t MapFlexible(std::uint64_t hint, std::uint64_t size, std::uint32_t protection,
                              std::uint32_t flags, std::uint64_t outputAddress = 0);
    std::uint64_t Reserve(std::uint64_t hint, std::uint64_t size, std::uint32_t flags,
                          std::uint64_t alignment, std::uint64_t outputAddress = 0);
    void Protect(std::uint64_t address, std::uint64_t size, std::uint32_t protection);
    void Unmap(std::uint64_t address, std::uint64_t size);
    void ReleaseDirect(std::int64_t physical, std::uint64_t size);
    GuestMemoryQuery Query(std::uint64_t address, bool findNext = false) const;
    GuestMemorySnapshot Snapshot() const;
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
