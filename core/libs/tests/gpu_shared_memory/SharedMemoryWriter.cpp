#include "SceTypes.hpp"
#include "prx/libc/include/GpuSharedMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestLayout.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

bool GuestThreadStack(std::uintptr_t, std::uintptr_t*, std::uintptr_t*) { return false; }
void NoteGuestStackProtection(std::uintptr_t, std::size_t, bool) {}
bool GuestThreadStackHostRangeOverlaps(std::uintptr_t, std::uintptr_t) { return false; }

extern "C" std::uint64_t GuestImageSegmentBytes_nid_no_patch() { return 0; }
extern "C" void* mmap_nid_postfix(void*, std::size_t, int, int, int, std::int64_t) noexcept { return MAP_FAILED; }
extern "C" int* __error_nid_postfix() { return &errno; }
extern "C" int APS5_VABI sceKernelVirtualQuery(const void* addr, int flags, VirtualQueryInfo* info, std::uint64_t infoSize);

extern "C" std::filesystem::path ResolvePath_nid_no_patch(const char* path) { return path; }

namespace {

constexpr int ProtCpuReadWrite = 0x3;
constexpr int ProtGpuReadWrite = 0x30;
constexpr int MapFixed = 0x10;
constexpr std::size_t Length = 0x10000;
constexpr std::uintptr_t RawDirectAddress = 0x1000000000ull;
constexpr std::uintptr_t RawFlexibleAddress = 0x2000000000ull;
constexpr std::uintptr_t RawLowAddress = 0x180000000ull;
constexpr std::uintptr_t ExpectedSlide = 0x8000000000ull;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

unsigned char WriterPattern(std::uintptr_t address, std::size_t offset) {
    return static_cast<unsigned char>((address >> 12) ^ (offset * 131));
}

unsigned char VerifierPattern(std::uintptr_t address, std::size_t offset) {
    return static_cast<unsigned char>(WriterPattern(address, offset) ^ 0x5a);
}

void Fill(void* address, std::size_t bytes, unsigned char (*pattern)(std::uintptr_t, std::size_t)) {
    auto* bytesAddress = static_cast<unsigned char*>(address);
    for (std::size_t offset = 0; offset < bytes; ++offset) bytesAddress[offset] = pattern(reinterpret_cast<std::uintptr_t>(address), offset);
}

bool Checked(void* address, std::size_t bytes, unsigned char (*pattern)(std::uintptr_t, std::size_t)) {
    const auto* bytesAddress = static_cast<const unsigned char*>(address);
    for (std::size_t offset = 0; offset < bytes; ++offset) {
        if (bytesAddress[offset] == pattern(reinterpret_cast<std::uintptr_t>(address), offset)) continue;
        std::fprintf(stderr, "FAIL: %p+0x%zx holds 0x%02x, not 0x%02x\n", address, offset, bytesAddress[offset],
            pattern(reinterpret_cast<std::uintptr_t>(address), offset));
        return false;
    }
    return true;
}

std::vector<std::uintptr_t> MappedRanges;

bool ManifestMentions(const std::filesystem::path& path, std::uintptr_t begin, std::uintptr_t end) {
    std::ifstream manifest(path);
    std::string line;
    while (std::getline(manifest, line)) {
        if (line.rfind("region ", 0) != 0) continue;
        std::istringstream fields(line);
        std::string kind;
        std::uint64_t address = 0;
        std::uint64_t bytes = 0;
        if (!(fields >> kind >> std::hex >> address >> bytes)) continue;
        if (address < end && begin < address + bytes) return true;
    }
    return false;
}

bool WaitForFile(const std::filesystem::path& path, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(path, error)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <manifest>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path manifest = argv[1];
    const std::filesystem::path done = manifest.string() + ".done";
    std::filesystem::remove(done);
    Check(GpuSharedMemory::GpuSharedMemoryEnabled_nid_postfix(), "the shared-memory flag is on");
    Check(GpuSharedMemory::GpuSharedMemoryEnabled_nid_postfix() == false || GuestArena::GuestArenaAvailable_nid_postfix(), "the guest arena is available");
    Check(GuestArena::GuestArenaSlide_nid_postfix() == ExpectedSlide, "the arena uses the 512 GiB slide");
    if (g_failures != 0) return 1;

    try {
        std::int64_t physical = -1;
        Check(DirectMemoryAlloc(0, DIRECT_MEMORY_SIZE, Length, Length, 0, &physical) == 0, "allocate direct memory");
        void* direct = reinterpret_cast<void*>(RawDirectAddress);
        Check(DoMapDirect(&direct, Length, ProtCpuReadWrite, MapFixed, physical, 0) == 0, "map direct memory at a raw guest address");
        Check(reinterpret_cast<std::uintptr_t>(direct) == RawDirectAddress + ExpectedSlide, "the mapping is returned at the slid address");
        Fill(direct, Length, WriterPattern);
        MappedRanges.push_back(reinterpret_cast<std::uintptr_t>(direct));

        void* flexible = nullptr;
        Check(DoMapAnon(&flexible, Length, ProtGpuReadWrite, 0) == 0, "map a GPU-flagged anonymous range");
        Check(reinterpret_cast<std::uintptr_t>(flexible) >= GuestLayout::NativeSlide, "the GPU mapping is above the macOS hole");
        Fill(flexible, Length, WriterPattern);
        MappedRanges.push_back(reinterpret_cast<std::uintptr_t>(flexible));

        void* fixed = reinterpret_cast<void*>(RawFlexibleAddress);
        Check(DoMapAnon(&fixed, Length, ProtGpuReadWrite, MapFixed) == 0, "map a fixed anonymous range in the raw application area");
        Check(reinterpret_cast<std::uintptr_t>(fixed) == RawFlexibleAddress + ExpectedSlide, "a fixed mapping is slid exactly once");
        Fill(fixed, Length, WriterPattern);
        MappedRanges.push_back(reinterpret_cast<std::uintptr_t>(fixed));

        void* heap = GuestArena::GuestArenaAllocate_nid_postfix(Length, 0x4000);
        GuestArena::GuestArenaCommit_nid_postfix(heap, Length, 0x04, Length);
        Fill(heap, Length, WriterPattern);
        MappedRanges.push_back(reinterpret_cast<std::uintptr_t>(heap));

        std::int64_t lowPhysical = -1;
        Check(DirectMemoryAlloc(0, DIRECT_MEMORY_SIZE, Length, Length, 0, &lowPhysical) == 0, "allocate direct memory for the low mapping");
        void* low = reinterpret_cast<void*>(RawLowAddress);
        Check(DoMapDirect(&low, Length, ProtCpuReadWrite, MapFixed, lowPhysical, 0) == 0, "map direct memory below the guest application area");
        Check(reinterpret_cast<std::uintptr_t>(low) == RawLowAddress, "a mapping outside the application area keeps its host address");
        Fill(low, Length, WriterPattern);

        Check(GpuSharedMemory::GpuSharedMemoryRegionCount_nid_postfix() >= 4, "every GPU-visible range is registered");
        Check(GpuSharedMemory::GpuSharedMemoryPublish_nid_postfix(), "the manifest is published");
        Check(!ManifestMentions(manifest, RawLowAddress, RawLowAddress + Length), "a mapping outside the shared address space is not published");
        std::printf("writer: %zu regions at", GpuSharedMemory::GpuSharedMemoryRegionCount_nid_postfix());
        for (const auto address : MappedRanges) std::printf(" 0x%llx", static_cast<unsigned long long>(address));
        std::printf("\n");
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "FAIL: writer threw: %s\n", failure.what());
        return 1;
    }

    if (!WaitForFile(done, std::chrono::seconds(120))) {
        std::fprintf(stderr, "FAIL: the verifier did not finish\n");
        return 1;
    }
    for (const auto address : MappedRanges) {
        Check(Checked(reinterpret_cast<void*>(address), Length, VerifierPattern), "the verifier's bytes are visible here");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d shared-memory check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("gpu shared memory writer: ok");
    return 0;
}
