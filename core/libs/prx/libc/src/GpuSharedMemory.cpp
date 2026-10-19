#include "prx/libc/include/GpuSharedMemory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace GpuSharedMemory {
namespace {

struct Region {
    std::uint64_t address;
    std::uint64_t bytes;
    std::string object;
    std::uint64_t offset;
};

struct State {
    std::mutex lock;
    std::vector<Region> regions;
    std::vector<std::string> objects;
    bool exitInstalled = false;
};

// Removes the part of every registered range that [address, end) covers, keeping the fragments
// before and after it with their object offsets advanced by the bytes kept, so a partial remap or
// unmap does not lose the ranges around it.
void SplittingErase(std::vector<Region>& regions, std::uint64_t address, std::uint64_t end) {
    std::vector<Region> kept;
    kept.reserve(regions.size() + 2);
    for (const auto& region : regions) {
        const auto regionEnd = region.address + region.bytes;
        if (regionEnd <= address || end <= region.address) {
            kept.push_back(region);
            continue;
        }
        if (region.address < address) kept.push_back({region.address, address - region.address, region.object, region.offset});
        if (regionEnd > end) kept.push_back({end, regionEnd - end, region.object, region.offset + (end - region.address)});
    }
    regions = std::move(kept);
}

State& state() {
    static auto* value = new State;
    return *value;
}

bool enabled() {
    static const bool value = [] {
#if defined(__APPLE__)
        const char* text = std::getenv(EnableEnvironment);
        return text != nullptr && *text != '\0' && std::strcmp(text, "0") != 0;
#else
        return false;
#endif
    }();
    return value;
}

#if defined(__APPLE__)
const char* manifestPath() {
    const char* text = std::getenv(ManifestEnvironment);
    return text != nullptr && *text != '\0' ? text : nullptr;
}

bool writeManifest() {
    const char* path = manifestPath();
    if (path == nullptr) return false;
    auto& shared = state();
    std::vector<Region> regions;
    std::vector<std::string> objects;
    {
        std::lock_guard lock(shared.lock);
        regions = shared.regions;
        objects = shared.objects;
    }
    std::sort(regions.begin(), regions.end(), [](const Region& left, const Region& right) {
        if (left.address != right.address) return left.address < right.address;
        return left.object < right.object;
    });
    std::sort(objects.begin(), objects.end());
    objects.erase(std::unique(objects.begin(), objects.end()), objects.end());
    const std::string temporary = std::string(path) + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "w");
    if (file == nullptr) return false;
    std::fprintf(file, "anyps5-gpu-shared-memory 1\n");
    for (const auto& object : objects) {
        std::uint64_t bytes = 0;
        const int descriptor = shm_open(object.c_str(), O_RDWR, 0600);
        if (descriptor >= 0) {
            struct stat status = {};
            if (fstat(descriptor, &status) == 0) bytes = static_cast<std::uint64_t>(status.st_size);
            ::close(descriptor);
        }
        std::fprintf(file, "object %s %llu\n", object.c_str(), static_cast<unsigned long long>(bytes));
    }
    for (const auto& region : regions) {
        std::fprintf(file, "region 0x%llx 0x%llx %s 0x%llx\n", static_cast<unsigned long long>(region.address),
            static_cast<unsigned long long>(region.bytes), region.object.c_str(), static_cast<unsigned long long>(region.offset));
    }
    if (std::fflush(file) != 0 || std::fclose(file) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    if (std::rename(temporary.c_str(), path) != 0) {
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}

void atExit() {
    writeManifest();
    auto& shared = state();
    std::vector<std::string> objects;
    {
        std::lock_guard lock(shared.lock);
        objects = shared.objects;
    }
    for (const auto& object : objects) shm_unlink(object.c_str());
}

void installExit() {
    auto& shared = state();
    std::lock_guard lock(shared.lock);
    if (shared.exitInstalled) return;
    shared.exitInstalled = true;
    std::atexit(atExit);
}
#endif

}

bool GpuSharedMemoryEnabled_nid_postfix() {
    return enabled();
}

int GpuSharedMemoryCreateObject_nid_postfix(const char* suffix, std::uint64_t bytes, char* name, std::size_t nameCapacity) {
#if defined(__APPLE__)
    if (!enabled() || suffix == nullptr || name == nullptr || nameCapacity == 0) return -1;
    char candidate[MaximumObjectName];
    const int written = std::snprintf(candidate, sizeof(candidate), "%s%d-%s", ObjectPrefix, static_cast<int>(getpid()), suffix);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(candidate) || static_cast<std::size_t>(written) >= nameCapacity) return -1;
    shm_unlink(candidate);
    const int descriptor = shm_open(candidate, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return -1;
    if (bytes != 0 && ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
        const int error = errno;
        ::close(descriptor);
        shm_unlink(candidate);
        errno = error;
        return -1;
    }
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
        const int error = errno;
        ::close(descriptor);
        shm_unlink(candidate);
        errno = error;
        return -1;
    }
    {
        auto& shared = state();
        std::lock_guard lock(shared.lock);
        shared.objects.emplace_back(candidate);
    }
    std::memcpy(name, candidate, static_cast<std::size_t>(written) + 1);
    installExit();
    return descriptor;
#else
    static_cast<void>(suffix);
    static_cast<void>(bytes);
    static_cast<void>(name);
    static_cast<void>(nameCapacity);
    return -1;
#endif
}

void GpuSharedMemoryAdoptObject_nid_postfix(const char* name) {
#if defined(__APPLE__)
    if (!enabled() || name == nullptr) return;
    {
        auto& shared = state();
        std::lock_guard lock(shared.lock);
        shared.objects.emplace_back(name);
    }
    installExit();
#else
    static_cast<void>(name);
#endif
}

bool GpuSharedMemoryRegister_nid_postfix(std::uint64_t address, std::uint64_t bytes, const char* object, std::uint64_t offset) {
#if defined(__APPLE__)
    if (!enabled() || bytes == 0 || object == nullptr) return false;
    auto& shared = state();
    std::lock_guard lock(shared.lock);
    SplittingErase(shared.regions, address, address + bytes);
    shared.regions.push_back({address, bytes, object, offset});
    return true;
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
    static_cast<void>(object);
    static_cast<void>(offset);
    return false;
#endif
}

void GpuSharedMemoryUnregister_nid_postfix(std::uint64_t address, std::uint64_t bytes) {
#if defined(__APPLE__)
    if (!enabled() || bytes == 0) return;
    auto& shared = state();
    std::lock_guard lock(shared.lock);
    SplittingErase(shared.regions, address, address + bytes);
#else
    static_cast<void>(address);
    static_cast<void>(bytes);
#endif
}

std::size_t GpuSharedMemoryRegionCount_nid_postfix() {
    auto& shared = state();
    std::lock_guard lock(shared.lock);
    return shared.regions.size();
}

bool GpuSharedMemoryPublish_nid_postfix() {
#if defined(__APPLE__)
    if (!enabled()) return false;
    return writeManifest();
#else
    return false;
#endif
}

}
