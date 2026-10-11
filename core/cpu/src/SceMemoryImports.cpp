#include <cpu/SceMemoryImports.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/SceElf.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace Cpu {
namespace {

enum class Service {
    Size, Available, Allocate, AllocateMain, MapDirect, MapFlexible, Reserve, Protect, Query, Unmap, Release,
    MapNamedFlexible, MapNamedSystemFlexible, MapNamedDirect, MapDirect2, BatchMap, BatchMap2,
    PoolExpand, PoolReserve, PoolCommit, PoolDecommit, PoolStats, PoolBatch, DirectQuery, DirectType,
    FlexibleAvailable, FlexibleConfigured, ReleaseFlexible, CheckedRelease, TypeProtect, SetName,
    QueryProtection, Lock, Unlock, KernelMmap,
    // POSIX aliases: -1 (MAP_FAILED for mmap) plus the active thread's errno instead of an SCE code.
    PosixMmap, PosixMunmap, PosixMprotect, PosixLock, PosixUnlock
};

bool posix(Service service) {
    return service == Service::PosixMmap || service == Service::PosixMunmap || service == Service::PosixMprotect ||
           service == Service::PosixLock || service == Service::PosixUnlock;
}

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

std::int32_t signedInt(std::uint64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

constexpr std::uint64_t page = GuestMemoryRuntime::PageSize;
constexpr std::uint32_t mapFixed = 0x10;
constexpr std::uint32_t mapNoOverwrite = 0x80;
// Room for every (NID, importer library/module id) pair a large module graph resolves.
constexpr std::uint64_t gatePages = 4;

}

struct SceMemoryImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    std::shared_ptr<GuestMemoryRuntime> memory;
    const std::uint64_t base;
    std::size_t nextSlot = 0;
    std::map<std::string, Service> services;
    std::map<Key, std::uint64_t> gates;
    std::function<std::uint64_t()> errnoLocation;

    Impl(Machine& guest, std::shared_ptr<GuestMemoryRuntime> runtime, std::uint64_t gateBase) :
        machine(guest), memory(std::move(runtime)), base(gateBase) {
        if (!memory) throw std::invalid_argument("SCE memory imports require a guest memory runtime");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE memory import gates require a nonzero aligned low canonical guest page");
        const std::pair<const char*, Service> table[] = {
                 {"sceKernelGetDirectMemorySize", Service::Size}, {"sceKernelAvailableDirectMemorySize", Service::Available},
                 {"sceKernelAllocateDirectMemory", Service::Allocate}, {"sceKernelAllocateMainDirectMemory", Service::AllocateMain},
                 {"sceKernelMapDirectMemory", Service::MapDirect}, {"sceKernelMapFlexibleMemory", Service::MapFlexible},
                 {"sceKernelReserveVirtualRange", Service::Reserve}, {"sceKernelMprotect", Service::Protect},
                 {"sceKernelVirtualQuery", Service::Query}, {"sceKernelMunmap", Service::Unmap},
                 {"sceKernelReleaseDirectMemory", Service::Release},
                 {"sceKernelMapNamedFlexibleMemory", Service::MapNamedFlexible},
                 {"sceKernelMapNamedSystemFlexibleMemory", Service::MapNamedSystemFlexible},
                 {"sceKernelMapNamedDirectMemory", Service::MapNamedDirect}, {"sceKernelMapDirectMemory2", Service::MapDirect2},
                 {"sceKernelBatchMap", Service::BatchMap}, {"sceKernelBatchMap2", Service::BatchMap2},
                 {"sceKernelMemoryPoolExpand", Service::PoolExpand}, {"sceKernelMemoryPoolReserve", Service::PoolReserve},
                 {"sceKernelMemoryPoolCommit", Service::PoolCommit}, {"sceKernelMemoryPoolDecommit", Service::PoolDecommit},
                 {"sceKernelMemoryPoolGetBlockStats", Service::PoolStats}, {"sceKernelMemoryPoolBatch", Service::PoolBatch},
                 {"sceKernelDirectMemoryQuery", Service::DirectQuery}, {"sceKernelGetDirectMemoryType", Service::DirectType},
                 {"sceKernelAvailableFlexibleMemorySize", Service::FlexibleAvailable},
                 {"sceKernelConfiguredFlexibleMemorySize", Service::FlexibleConfigured},
                 {"sceKernelReleaseFlexibleMemory", Service::ReleaseFlexible},
                 {"sceKernelCheckedReleaseDirectMemory", Service::CheckedRelease},
                 {"sceKernelMtypeprotect", Service::TypeProtect}, {"sceKernelSetVirtualRangeName", Service::SetName},
                 {"sceKernelQueryMemoryProtection", Service::QueryProtection},
                 {"sceKernelMlock", Service::Lock}, {"sceKernelMunlock", Service::Unlock}, {"sceKernelMmap", Service::KernelMmap},
                 {"mmap", Service::PosixMmap}, {"munmap", Service::PosixMunmap}, {"mprotect", Service::PosixMprotect},
                 {"mlock", Service::PosixLock}, {"munlock", Service::PosixUnlock}};
        for (const auto& [name, service] : table) services.emplace(Nid::ComputeNid(name, "libkernel"), service);
        if (base > 0x7ffffffff000 - gatePages * 4096)
            throw std::invalid_argument("SCE memory import gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, gatePages * 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

    void checkOutput(std::uint64_t address, std::size_t size, bool input = false) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address)
            throw GuestMemoryError(14, "Invalid guest memory import output span");
        try { machine.CheckAccess(address, size, input ? Permission::Read | Permission::Write : Permission::Write); }
        catch (const std::runtime_error&) { throw GuestMemoryError(14, "Guest memory import output span is inaccessible"); }
    }

    void checkInput(std::uint64_t address, std::size_t size) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address)
            throw GuestMemoryError(14, "Invalid guest memory import input span");
        try { machine.CheckAccess(address, size, Permission::Read); }
        catch (const std::runtime_error&) { throw GuestMemoryError(14, "Guest memory import input span is inaccessible"); }
    }

    template<class T> T read(std::uint64_t address) const {
        checkInput(address, sizeof(T));
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }

    std::uint64_t readAddress(std::uint64_t address) const {
        checkOutput(address, 8, true);
        std::uint64_t value = 0;
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }

    template<class T> void write(std::uint64_t address, const T& value) {
        machine.Write(address, std::as_bytes(std::span(&value, 1)));
    }

    void writeAddress(std::uint64_t address, std::uint64_t value) { write(address, value); }

    // The seventh SysV argument: the gate is entered by CALL, so it sits above the return address.
    std::uint64_t stackArgument(Machine& guest) const {
        const auto stack = guest.Get(Register::Rsp);
        if (stack > std::numeric_limits<std::uint64_t>::max() - 16) throw GuestMemoryError(14, "Invalid guest stack argument");
        return read<std::uint64_t>(stack + 8);
    }

    // A range name is at most 31 bytes, NUL-terminated or truncated; a null pointer means no name.
    std::optional<std::string> readName(std::uint64_t address) const {
        if (!address) return std::nullopt;
        std::string text;
        for (std::uint64_t index = 0; index < 31; ++index) {
            if (address > std::numeric_limits<std::uint64_t>::max() - index) throw GuestMemoryError(14, "Invalid guest range name");
            const auto value = read<char>(address + index);
            if (!value) break;
            text.push_back(value);
        }
        return text;
    }

    // Runs inside the error path, so it must not turn the guest's -1 into a host abort: without
    // an active thread or a writable slot the call still fails, only errno stays unchanged.
    void writeErrno(std::int64_t result) {
        if (!errnoLocation) return;
        try {
            const auto address = errnoLocation();
            if (!address || address > std::numeric_limits<std::uint64_t>::max() - 4) return;
            machine.CheckAccess(address, 4, Permission::Read | Permission::Write);
            write(address, static_cast<std::int32_t>(result & 0xffff));
        } catch (const std::runtime_error&) {}
    }

    std::uint64_t mapNamedFlexible(std::uint64_t slot, std::uint64_t size, std::uint64_t protection, std::uint64_t flags,
                                   std::uint64_t name) {
        const auto hint = readAddress(slot);
        const auto text = readName(name);
        const auto address = memory->MapFlexible(hint, size, static_cast<std::uint32_t>(protection), static_cast<std::uint32_t>(flags), slot);
        writeAddress(slot, address);
        if (text) memory->SetName(address, size, *text);
        return 0;
    }

    // FreeBSD mmap(2) for anonymous memory: flexible memory, or a reservation for MAP_GUARD.
    // output, when set, is checked by the runtime against the new mapping before it commits.
    std::uint64_t mmap(std::uint64_t hint, std::uint64_t size, std::uint64_t protection, std::uint64_t flagsValue,
                       std::uint64_t descriptor, std::uint64_t offset, std::uint64_t output = 0) {
        constexpr std::uint32_t shared = 0x1, privateMap = 0x2, stack = 0x400, noSync = 0x800, anonymous = 0x1000,
            guard = 0x2000, exclusive = 0x4000, noCore = 0x20000, noCoalesce = 0x400000, alignedMask = 0xff000000u;
        const auto flags = static_cast<std::uint32_t>(flagsValue);
        const auto prot = static_cast<std::uint32_t>(protection);
        if (!size || size > std::numeric_limits<std::uint64_t>::max() - (page - 1)) throw GuestMemoryError(22, "Invalid mmap length");
        const auto rounded = (size + page - 1) & ~(page - 1);
        if (prot & ~7u) throw GuestMemoryError(22, "Invalid mmap protection");
        if (flags & ~(shared | privateMap | mapFixed | stack | noSync | anonymous | guard | exclusive | noCore | noCoalesce | alignedMask))
            throw GuestMemoryError(22, "Unsupported mmap flags");
        auto placement = flags & (mapFixed | noCoalesce | alignedMask);
        if (flags & exclusive) {
            if (!(flags & mapFixed)) throw GuestMemoryError(22, "MAP_EXCL without MAP_FIXED");
            placement |= mapNoOverwrite;
        }
        if (flags & guard) {
            if (prot || signedInt(descriptor) != -1 || offset) throw GuestMemoryError(22, "Invalid MAP_GUARD request");
            return memory->Reserve(hint, rounded, placement, 0, output);
        }
        if (!(flags & anonymous)) {
            if (signedInt(descriptor) == -1) throw GuestMemoryError(9, "mmap without MAP_ANON needs a descriptor");
            throw GuestMemoryError(45, "File-backed guest mmap is not supported");
        }
        if (signedInt(descriptor) != -1 || offset) throw GuestMemoryError(22, "Anonymous mmap with a descriptor or offset");
        return memory->MapFlexible(hint, rounded, prot, placement, output);
    }

    // Applies 32-byte entries in order, stops at the first failure and reports how many succeeded.
    template<class Apply>
    void batch(std::uint64_t entries, std::uint64_t countValue, std::uint64_t processedOutput, Apply&& apply) {
        const auto count = signedInt(countValue);
        if (!entries || count < 0) throw GuestMemoryError(22, "Invalid memory batch entries");
        if (processedOutput) checkOutput(processedOutput, 4);
        std::int32_t processed = 0;
        // An earlier entry may have unmapped or protected the count slot, so recheck it.
        const auto report = [&] {
            if (!processedOutput) return;
            checkOutput(processedOutput, 4);
            write(processedOutput, processed);
        };
        try {
            for (; processed < count; ++processed) {
                const auto entry = entries + 32ULL * static_cast<std::uint64_t>(processed);
                checkOutput(entry, 32, true);
                apply(entry);
            }
        } catch (const GuestMemoryError&) {
            report();
            throw;
        }
        report();
    }

    // sceKernelBatchMap entry: start, offset, length, protection:8, type:8, reserved:16, operation:32.
    void mapEntry(std::uint64_t entry, std::uint32_t flags) {
        const auto start = read<std::uint64_t>(entry), offset = read<std::uint64_t>(entry + 8), size = read<std::uint64_t>(entry + 16);
        const auto protection = static_cast<std::uint32_t>(read<std::uint8_t>(entry + 24));
        const auto type = static_cast<std::int32_t>(read<std::uint8_t>(entry + 25));
        if (!size) throw GuestMemoryError(22, "Empty batch map entry");
        // Mapping operations write the chosen address back into the entry; the runtime checks
        // that slot against the new mapping before committing.
        switch (read<std::int32_t>(entry + 28)) {
        case 0: write(entry, memory->MapDirect(start, size, protection, flags, std::bit_cast<std::int64_t>(offset), 0, entry)); break;
        case 1: memory->Unmap(start, size); break;
        case 2: memory->Protect(start, size, protection); break;
        case 3: write(entry, memory->MapFlexible(start, size, protection, flags, entry)); break;
        case 4: memory->Protect(start, size, protection, type); break;
        default: throw GuestMemoryError(22, "Unsupported batch map operation");
        }
    }

    // sceKernelMemoryPoolBatch entry: operation:32, flags:32, address, length, protection:8, type:8.
    void poolEntry(std::uint64_t entry) {
        const auto address = read<std::uint64_t>(entry + 8), size = read<std::uint64_t>(entry + 16);
        const auto protection = static_cast<std::uint32_t>(read<std::uint8_t>(entry + 24));
        const auto type = static_cast<std::int32_t>(read<std::uint8_t>(entry + 25));
        switch (read<std::uint32_t>(entry)) {
        case 1: memory->PoolCommit(address, size, type, protection); break;
        case 2: memory->PoolDecommit(address, size); break;
        case 3: memory->Protect(address, size, protection); break;
        case 4: memory->Protect(address, size, protection, type); break;
        default: throw GuestMemoryError(22, "Unsupported memory pool batch operation");
        }
    }

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        const auto fourth = guest.Get(Register::Rcx);
        const auto fifth = guest.Get(Register::R8);
        const auto sixth = guest.Get(Register::R9);
        if (service == Service::Size) { guest.Set(Register::Rax, memory->DirectMemorySize()); return; }
        std::uint64_t value = 0;
        try {
            switch (service) {
            case Service::Available: {
                checkOutput(fourth, 8);
                checkOutput(fifth, 8);
                const auto extent = memory->AvailableDirect(std::bit_cast<std::int64_t>(first), std::bit_cast<std::int64_t>(second), third);
                writeAddress(fourth, extent.Address);
                writeAddress(fifth, extent.Size);
                break;
            }
            case Service::Allocate:
                checkOutput(sixth, 8);
                writeAddress(sixth, memory->AllocateDirect(std::bit_cast<std::int64_t>(first), std::bit_cast<std::int64_t>(second), third, fourth, signedInt(fifth)));
                break;
            case Service::AllocateMain:
                checkOutput(fourth, 8);
                writeAddress(fourth, memory->AllocateDirect(0, static_cast<std::int64_t>(memory->DirectMemorySize()), first, second, signedInt(third)));
                break;
            case Service::MapDirect: {
                const auto hint = readAddress(first);
                const auto address = memory->MapDirect(hint, second, static_cast<std::uint32_t>(third), static_cast<std::uint32_t>(fourth),
                                                      std::bit_cast<std::int64_t>(fifth), sixth, first);
                writeAddress(first, address);
                break;
            }
            case Service::MapNamedDirect: {
                const auto hint = readAddress(first);
                const auto text = readName(stackArgument(guest));
                const auto address = memory->MapDirect(hint, second, static_cast<std::uint32_t>(third), static_cast<std::uint32_t>(fourth),
                                                      std::bit_cast<std::int64_t>(fifth), sixth, first);
                writeAddress(first, address);
                if (text) memory->SetName(address, second, *text);
                break;
            }
            case Service::MapDirect2: {
                const auto hint = readAddress(first);
                const auto alignment = stackArgument(guest);
                const auto type = signedInt(third);
                if (type < 0) throw GuestMemoryError(22, "Invalid guest direct memory type");
                writeAddress(first, memory->MapDirect(hint, second, static_cast<std::uint32_t>(fourth), static_cast<std::uint32_t>(fifth),
                                                      std::bit_cast<std::int64_t>(sixth), alignment, first, type));
                break;
            }
            case Service::MapFlexible: {
                const auto hint = readAddress(first);
                writeAddress(first, memory->MapFlexible(hint, second, static_cast<std::uint32_t>(third), static_cast<std::uint32_t>(fourth), first));
                break;
            }
            case Service::MapNamedFlexible: case Service::MapNamedSystemFlexible:
                value = mapNamedFlexible(first, second, third, fourth, fifth);
                break;
            case Service::Reserve: {
                const auto hint = readAddress(first);
                writeAddress(first, memory->Reserve(hint, second, static_cast<std::uint32_t>(third), fourth, first));
                break;
            }
            case Service::Protect: memory->Protect(first, second, static_cast<std::uint32_t>(third)); break;
            case Service::TypeProtect: {
                const auto type = signedInt(third);
                if (type < 0) throw GuestMemoryError(22, "Invalid guest memory type");
                memory->Protect(first, second, static_cast<std::uint32_t>(fourth), type);
                break;
            }
            case Service::Query: {
                if (fourth < 72) throw GuestMemoryError(22, "Guest virtual query information is too small");
                checkOutput(third, 72);
                const auto flags = static_cast<std::uint32_t>(second);
                if (flags & ~1u) throw GuestMemoryError(22, "Unsupported guest memory virtual query flags");
                const auto bytes = memory->Query(first, flags != 0).Serialize();
                machine.Write(third, bytes);
                break;
            }
            case Service::QueryProtection: {
                if (second) checkOutput(second, 8);
                if (third) checkOutput(third, 8);
                if (fourth) checkOutput(fourth, 4);
                // The protection's own extent, not a range-name fragment of it.
                const auto query = memory->Query(first, false, false);
                if (second) writeAddress(second, query.Start);
                if (third) writeAddress(third, query.End);
                if (fourth) write(fourth, query.Protection);
                break;
            }
            case Service::Unmap: case Service::ReleaseFlexible: memory->Unmap(first, second); break;
            case Service::Release: memory->ReleaseDirect(std::bit_cast<std::int64_t>(first), second); break;
            case Service::CheckedRelease:
                if (std::bit_cast<std::int64_t>(first) < 0 || first % page || second % page)
                    throw GuestMemoryError(22, "Invalid checked direct memory release");
                if (second) memory->ReleaseDirect(std::bit_cast<std::int64_t>(first), second, true);
                break;
            case Service::BatchMap: batch(first, second, third, [&](std::uint64_t entry) { mapEntry(entry, mapFixed); }); break;
            case Service::BatchMap2:
                batch(first, second, third, [&](std::uint64_t entry) { mapEntry(entry, static_cast<std::uint32_t>(fourth)); });
                break;
            case Service::PoolBatch: batch(first, second, third, [&](std::uint64_t entry) { poolEntry(entry); }); break;
            case Service::PoolExpand:
                checkOutput(fifth, 8);
                writeAddress(fifth, memory->PoolExpand(std::bit_cast<std::int64_t>(first), std::bit_cast<std::int64_t>(second), third, fourth));
                break;
            case Service::PoolReserve:
                checkOutput(fifth, 8);
                writeAddress(fifth, memory->Reserve(first, second, static_cast<std::uint32_t>(fourth), third, fifth, true));
                break;
            case Service::PoolCommit: memory->PoolCommit(first, second, signedInt(third), static_cast<std::uint32_t>(fourth)); break;
            case Service::PoolDecommit: memory->PoolDecommit(first, second); break;
            case Service::PoolStats: {
                if (second < 16) throw GuestMemoryError(22, "Guest memory pool statistics output is too small");
                checkOutput(first, 16);
                const auto stats = memory->PoolStats();
                write(first, std::array<std::int32_t, 4>{stats.AvailableFlushedBlocks, stats.AvailableCachedBlocks,
                                                         stats.AllocatedFlushedBlocks, stats.AllocatedCachedBlocks});
                break;
            }
            case Service::DirectQuery: {
                if (fourth < 24) throw GuestMemoryError(22, "Guest direct memory query information is too small");
                checkOutput(third, 24);
                const auto found = memory->QueryDirect(std::bit_cast<std::int64_t>(first), (second & 1) != 0);
                if (!found) throw GuestMemoryError(13, "Guest direct memory query found no allocation");
                struct { std::uint64_t start, end; std::int32_t type, padding; } info{found->Start, found->End, found->MemoryType, 0};
                write(third, info);
                break;
            }
            case Service::DirectType: {
                checkOutput(second, 4); checkOutput(third, 8); checkOutput(fourth, 8);
                const auto found = memory->QueryDirect(std::bit_cast<std::int64_t>(first), false);
                if (!found) throw GuestMemoryError(2, "Guest direct memory offset is not allocated");
                write(second, found->MemoryType);
                writeAddress(third, found->Start);
                writeAddress(fourth, found->End);
                break;
            }
            case Service::FlexibleAvailable: checkOutput(first, 8); writeAddress(first, memory->AvailableFlexible()); break;
            case Service::FlexibleConfigured: checkOutput(first, 8); writeAddress(first, memory->FlexibleMemorySize()); break;
            case Service::SetName: {
                if (!third) throw GuestMemoryError(22, "Missing guest range name");
                memory->SetName(first, second, *readName(third));
                break;
            }
            case Service::Lock: case Service::Unlock: case Service::PosixLock: case Service::PosixUnlock:
                memory->CheckMapped(first, second);
                break;
            case Service::KernelMmap: {
                const auto result = stackArgument(guest);
                if (!result) throw GuestMemoryError(22, "Missing sceKernelMmap result pointer");
                checkOutput(result, 8);
                writeAddress(result, mmap(first, second, third, fourth, fifth, sixth, result));
                break;
            }
            case Service::PosixMmap: value = mmap(first, second, third, fourth, fifth, sixth); break;
            case Service::PosixMunmap: memory->Unmap(first, second); break;
            case Service::PosixMprotect: if (second) memory->Protect(first, second, static_cast<std::uint32_t>(third)); break;
            default: throw std::runtime_error("Unsupported SCE memory import operation");
            }
        } catch (const GuestMemoryError& error) {
            if (posix(service)) {
                writeErrno(error.Result());
                value = ~std::uint64_t{0};
            } else {
                value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(error.Result())));
            }
        }
        guest.Set(Register::Rax, value);
    }
};

SceMemoryImports::SceMemoryImports(Machine& machine, std::shared_ptr<GuestMemoryRuntime> memory, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, std::move(memory), gateBase)) {}
SceMemoryImports::~SceMemoryImports() = default;

void SceMemoryImports::SetErrnoLocation(std::function<std::uint64_t()> location) {
    impl->errnoLocation = std::move(location);
}

std::optional<std::uint64_t> SceMemoryImports::Resolve(const SceImport& import) {
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) return std::nullopt;
    // libkernel also exports the POSIX aliases through its libScePosix library.
    const bool library = import.LibraryName == "libkernel" || (posix(service->second) && import.LibraryName == "libScePosix");
    if (!library || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE memory import scope/version: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->nextSlot == gatePages * 4096 / 16) throw std::runtime_error("SCE memory import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation = service->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE memory import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
