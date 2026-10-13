#include "SceTypes.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <system_error>

#include <sys/mman.h>

bool GuestThreadStack(std::uintptr_t, std::uintptr_t*, std::uintptr_t*) { return false; }
void NoteGuestStackProtection(std::uintptr_t, std::size_t, bool) {}
bool GuestThreadStackHostRangeOverlaps(std::uintptr_t, std::uintptr_t) { return false; }

// Export.cpp's own dependencies, which this harness does not link.
extern "C" std::uint64_t GuestImageSegmentBytes_nid_no_patch() { return 0; }
extern "C" void* mmap_nid_postfix(void*, std::size_t, int, int, int, std::int64_t) noexcept { return MAP_FAILED; }
extern "C" int* __error_nid_postfix() { return &errno; }
extern "C" int APS5_VABI sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, std::uint64_t infoSize);

namespace {

constexpr int ProtCpuReadWrite = 0x3;
constexpr int MapFixed = 0x10;
constexpr int MapNoOverwrite = 0x80;
constexpr std::size_t Length = 0x10000;
constexpr std::uintptr_t RefusedStart = 0x1000000000ull;
constexpr std::uintptr_t RefusedEnd = 0x8000000000ull;
constexpr std::uintptr_t FixedAddress = 0x900000000ull;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

auto* Bytes(void* address) { return static_cast<volatile unsigned char*>(address); }

void AllocateMapFixedAndAlias() {
    std::int64_t phys = -1;
    Check(DirectMemoryAlloc(0, DIRECT_MEMORY_SIZE, Length, Length, 0, &phys) == 0, "allocate direct memory");
    Check(phys >= 0 && phys % Length == 0, "allocation is aligned");

    void* fixed = reinterpret_cast<void*>(FixedAddress);
    Check(DoMapDirect(&fixed, Length, ProtCpuReadWrite, MapFixed | MapNoOverwrite, phys, 0) == 0, "map direct memory at a fixed address");
    Check(fixed == reinterpret_cast<void*>(FixedAddress), "fixed view lands at the requested address");

    void* alias = nullptr;
    Check(DoMapDirect(&alias, Length, ProtCpuReadWrite, 0, phys, 0) == 0, "map a second view of the same range");
    Check(alias != nullptr && alias != fixed, "second view has its own address");

    void* tail = nullptr;
    Check(DoMapDirect(&tail, PS5_PAGE_SIZE, ProtCpuReadWrite, 0, phys + PS5_PAGE_SIZE, 0) == 0, "map a view of the second page");

    Check(Bytes(fixed)[0] == 0 && Bytes(fixed)[Length - 1] == 0, "fresh direct memory reads as zero");
    Bytes(fixed)[0] = 0x5a;
    Bytes(alias)[Length - 1] = 0xa5;
    Bytes(tail)[1] = 0x3c;
    Check(Bytes(alias)[0] == 0x5a, "write through the fixed view is visible in the alias");
    Check(Bytes(fixed)[Length - 1] == 0xa5, "write through the alias is visible in the fixed view");
    Check(Bytes(fixed)[PS5_PAGE_SIZE + 1] == 0x3c && Bytes(alias)[PS5_PAGE_SIZE + 1] == 0x3c, "offset view aliases the second page");

    Check(DoMunmap(tail, PS5_PAGE_SIZE) == 0, "unmap the offset view");
    Check(DoMunmap(alias, Length) == 0, "unmap the alias");
    Check(DoMunmap(fixed, Length) == 0, "unmap the fixed view");
    DirectMemoryFree(phys, Length);

    std::int64_t again = -1;
    Check(DirectMemoryAlloc(0, DIRECT_MEMORY_SIZE, Length, Length, 0, &again) == 0 && again == phys, "reallocate the released range");
    void* reused = nullptr;
    Check(DoMapDirect(&reused, Length, ProtCpuReadWrite, 0, again, 0) == 0, "map the reallocated range");
    Check(Bytes(reused)[0] == 0 && Bytes(reused)[Length - 1] == 0, "reallocated direct memory is zeroed");
    DoMunmap(reused, Length);
    DirectMemoryFree(again, Length);
}

void NoOverwriteKeepsHostMapping() {
    void* region = mmap(nullptr, 2 * Length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(region != MAP_FAILED, "host mapping");
    if (region == MAP_FAILED) return;
    void* host = reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(region) + Length - 1) & ~(std::uintptr_t{Length} - 1));
    Bytes(host)[0] = 0x77;
    void* address = host;
    int error = 0;
    try {
        DoMapAnon(&address, Length, ProtCpuReadWrite, MapFixed | MapNoOverwrite);
    } catch (const std::system_error& failure) {
        error = failure.code().value();
    }
    Check(error == EEXIST, "no-overwrite fixed mapping over a host mapping fails with EEXIST");
    Check(Bytes(host)[0] == 0x77, "host mapping is left intact");
    munmap(region, 2 * Length);
}

void HintSkipsRefusedRange() {
    void* address = reinterpret_cast<void*>(RefusedStart + 0x100000000ull);
    Check(DoMapAnon(&address, Length, ProtCpuReadWrite, 0, Length) == 0, "hinted mapping inside the refused range");
    const auto placed = reinterpret_cast<std::uintptr_t>(address);
    Check(placed >= RefusedEnd, "hinted mapping is placed above the refused range");
    Check(placed % Length == 0, "hinted mapping keeps its alignment");
    Bytes(address)[0] = 1;
    Check(Bytes(address)[0] == 1, "hinted mapping is writable");
    DoMunmap(address, Length);
}

void FixedInRefusedRangeRelocates() {
    constexpr std::uintptr_t Shift = RefusedEnd - RefusedStart;
    std::int64_t phys = -1;
    Check(DirectMemoryAlloc(0, DIRECT_MEMORY_SIZE, Length, Length, 0, &phys) == 0, "allocate for the refused-range check");
    const auto directWanted = RefusedStart + 0x200000000ull;
    void* direct = reinterpret_cast<void*>(directWanted);
    Check(DoMapDirect(&direct, Length, ProtCpuReadWrite, MapFixed, phys, 0) == 0, "fixed direct mapping in the refused range is relocated");
    Check(reinterpret_cast<std::uintptr_t>(direct) == directWanted + Shift, "relocated direct mapping is returned through addr, above the refused range");
    Bytes(direct)[0] = 5;
    Check(Bytes(direct)[0] == 5, "relocated direct mapping is writable");
    const auto anonymousWanted = RefusedEnd - Length;
    void* anonymous = reinterpret_cast<void*>(anonymousWanted);
    Check(DoMapAnon(&anonymous, Length, ProtCpuReadWrite, MapFixed) == 0, "fixed flexible mapping in the refused range is relocated");
    Check(reinterpret_cast<std::uintptr_t>(anonymous) == anonymousWanted + Shift, "relocated flexible mapping keeps the same shift");
    const auto reservedWanted = RefusedStart + 0x300000000ull;
    void* reserved = reinterpret_cast<void*>(reservedWanted);
    Check(DoReserveVirtual(&reserved, 2 * PS5_PAGE_SIZE, MapFixed, 0) == 0, "fixed reservation in the refused range is relocated");
    Check(reinterpret_cast<std::uintptr_t>(reserved) == reservedWanted + Shift, "relocated reservation keeps the same shift");
    void* straddling = reinterpret_cast<void*>(RefusedStart - PS5_PAGE_SIZE);
    Check(DoReserveVirtual(&straddling, 2 * PS5_PAGE_SIZE, MapFixed, 0) == SCE_KERNEL_ERROR_ENOMEM, "fixed reservation running into the refused range fails with ENOMEM");
    DoMunmap(direct, Length);
    DoMunmap(anonymous, Length);
    DoMunmap(reserved, 2 * PS5_PAGE_SIZE);
    DirectMemoryFree(phys, Length);
}

void VirtualQueryDescribesHostMemory() {
    // Memory the guest registry does not know (a title's own heap block) is described by the host's region.
    const std::size_t bytes = 3 * 0x4000;
    void* block = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    Check(block != MAP_FAILED, "map a host block");
    if (block == MAP_FAILED) return;
    const auto address = reinterpret_cast<std::uintptr_t>(block);
    VirtualQueryInfo info{};
    Check(sceKernelVirtualQuery(reinterpret_cast<void*>(address + 0x4010), 0, &info, sizeof(info)) == 0, "query a host block");
    Check(info.start <= address && info.end >= address + bytes, "the extent covers the host block");
    Check((info.protection & 3) == 3, "the host block is readable and writable");
    mprotect(block, bytes, PROT_NONE);
    Check(sceKernelVirtualQuery(block, 0, &info, sizeof(info)) == SCE_KERNEL_ERROR_EACCES, "an unreadable host block is refused");
    munmap(block, bytes);
    Check(sceKernelVirtualQuery(block, 0, &info, sizeof(info)) == SCE_KERNEL_ERROR_EACCES, "unmapped memory is refused");
}

void LookupFindsAllocationByAddress() {
    void* first = nullptr;
    Check(DoMapAnon(&first, Length, ProtCpuReadWrite, 0) == 0, "map an anonymous range");
    const auto base = reinterpret_cast<std::uintptr_t>(first);
    {
        // A found range pins its allocation while it is held.
        const auto inside = GuestAllocations::GuestAllocationsLookup_nid_postfix(base + 0x123, false);
        Check(inside && inside->address <= base && inside->address + inside->bytes >= base + Length, "an address inside a range finds it");
    }
    Check(GuestAllocations::GuestAllocationsLookup_nid_postfix(base + Length + (1ull << 40), false) == nullptr, "an address past every range finds nothing");
    Check(DoMunmap(static_cast<unsigned char*>(first) + PS5_PAGE_SIZE, PS5_PAGE_SIZE) == 0, "unmap the second page");
    {
        const auto head = GuestAllocations::GuestAllocationsLookup_nid_postfix(base, false);
        const auto hole = GuestAllocations::GuestAllocationsLookup_nid_postfix(base + PS5_PAGE_SIZE, false);
        const auto tail = GuestAllocations::GuestAllocationsLookup_nid_postfix(base + 2 * PS5_PAGE_SIZE, false);
        Check(head && head->address + head->bytes <= base + PS5_PAGE_SIZE, "the head keeps only the first page");
        Check(!hole, "the unmapped page finds nothing");
        Check(tail && tail->address == base + 2 * PS5_PAGE_SIZE, "the tail starts after the hole");
    }
    DoMunmap(first, PS5_PAGE_SIZE);
    DoMunmap(static_cast<unsigned char*>(first) + 2 * PS5_PAGE_SIZE, Length - 2 * PS5_PAGE_SIZE);
    Check(!GuestAllocations::GuestAllocationsLookup_nid_postfix(base, false), "nothing is left after unmapping every page");
}

void Run(const char* name, void (*test)()) {
    try {
        test();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "FAIL: %s threw: %s\n", name, failure.what());
        ++g_failures;
    }
}

} // namespace

int main() {
    Run("allocate, map fixed and alias", AllocateMapFixedAndAlias);
    Run("no-overwrite keeps host mapping", NoOverwriteKeepsHostMapping);
    Run("hint skips refused range", HintSkipsRefusedRange);
    Run("fixed in refused range relocates", FixedInRefusedRangeRelocates);
    Run("virtual query describes host memory", VirtualQueryDescribesHostMemory);
    Run("lookup finds an allocation by address", LookupFindsAllocationByAddress);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d direct memory check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("direct memory darwin: ok");
    return 0;
}
