#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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
    std::shared_ptr<const void> Scope{};
    std::shared_ptr<const void> MachineScope{};
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
struct GuestDirectMemoryQuery { std::uint64_t Start; std::uint64_t End; std::int32_t MemoryType; };
struct GuestMemoryPoolStats {
    std::int32_t AvailableFlushedBlocks, AvailableCachedBlocks, AllocatedFlushedBlocks, AllocatedCachedBlocks;
};

class GuestMemoryRuntime {
public:
    using Transaction = std::function<void(const GuestMemorySnapshot&, const GuestMemorySnapshot&,
                                           const std::function<void()>&)>;
    static constexpr std::uint64_t PageSize = 0x4000;
    // The flexible budget a title gets when it does not configure one (SEC-05).
    static constexpr std::uint64_t DefaultFlexibleCapacity = 448ULL << 20;
    // Memory pools are accounted in 64 KiB blocks.
    static constexpr std::uint64_t PoolBlockSize = 0x10000;
    GuestMemoryRuntime(Machine& machine, std::uint64_t directCapacity, Transaction transaction = {},
                       std::uint64_t flexibleCapacity = DefaultFlexibleCapacity);
    ~GuestMemoryRuntime();
    GuestMemoryRuntime(const GuestMemoryRuntime&) = delete;
    GuestMemoryRuntime& operator=(const GuestMemoryRuntime&) = delete;
    std::uint64_t DirectMemorySize() const;
    std::uint64_t FlexibleMemorySize() const;
    std::uint64_t AvailableFlexible() const;
    std::uint64_t AllocateDirect(std::int64_t searchStart, std::int64_t searchEnd, std::uint64_t size,
                                 std::uint64_t alignment, std::int32_t memoryType);
    // The largest free physical run inside the search range, starting at the requested alignment.
    GuestPhysicalExtent AvailableDirect(std::int64_t searchStart, std::int64_t searchEnd,
                                         std::uint64_t alignment) const;
    // The physical allocation containing offset (or with findNext the first one after it).
    std::optional<GuestDirectMemoryQuery> QueryDirect(std::int64_t offset, bool findNext) const;
    // memoryType >= 0 retypes the mapping and its physical pages (sceKernelMapDirectMemory2).
    std::uint64_t MapDirect(std::uint64_t hint, std::uint64_t size, std::uint32_t protection,
                            std::uint32_t flags, std::int64_t physical, std::uint64_t alignment,
                            std::uint64_t outputAddress = 0, std::int32_t memoryType = -1);
    std::uint64_t MapFlexible(std::uint64_t hint, std::uint64_t size, std::uint32_t protection,
                              std::uint32_t flags, std::uint64_t outputAddress = 0);
    std::uint64_t Reserve(std::uint64_t hint, std::uint64_t size, std::uint32_t flags,
                          std::uint64_t alignment, std::uint64_t outputAddress = 0, bool pool = false);
    // memoryType >= 0 also retypes direct pages (sceKernelMtypeprotect).
    void Protect(std::uint64_t address, std::uint64_t size, std::uint32_t protection, std::int32_t memoryType = -1);
    // Rounds the length up to whole pages; holes in the range are not an error.
    void Unmap(std::uint64_t address, std::uint64_t size);
    // Unmaps every virtual alias of the released pages. Holes are skipped unless checked (ENOENT).
    void ReleaseDirect(std::int64_t physical, std::uint64_t size, bool checked = false);
    void SetName(std::uint64_t address, std::uint64_t size, std::string_view name);
    // ENOMEM unless every page of the rounded range is mapped (mlock/munlock).
    void CheckMapped(std::uint64_t address, std::uint64_t size) const;
    std::uint64_t PoolExpand(std::int64_t searchStart, std::int64_t searchEnd, std::uint64_t size, std::uint64_t alignment);
    void PoolCommit(std::uint64_t address, std::uint64_t size, std::int32_t memoryType, std::uint32_t protection);
    void PoolDecommit(std::uint64_t address, std::uint64_t size);
    GuestMemoryPoolStats PoolStats() const;
    // splitNames clips the extent to the range name covering the address, as VirtualQuery does.
    GuestMemoryQuery Query(std::uint64_t address, bool findNext = false, bool splitNames = true) const;
    GuestMemorySnapshot Snapshot() const;
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
