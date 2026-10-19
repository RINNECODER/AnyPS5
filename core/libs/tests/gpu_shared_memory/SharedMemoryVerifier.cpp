#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

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

struct Region {
    std::uint64_t address;
    std::uint64_t bytes;
    std::string object;
    std::uint64_t offset;
};

bool Checked(void* address, std::size_t bytes) {
    const auto* bytesAddress = static_cast<const unsigned char*>(address);
    for (std::size_t offset = 0; offset < bytes; ++offset) {
        const auto expected = WriterPattern(reinterpret_cast<std::uintptr_t>(address), offset);
        if (bytesAddress[offset] == expected) continue;
        std::fprintf(stderr, "FAIL: %p+0x%zx holds 0x%02x, not the writer's 0x%02x\n", address, offset, bytesAddress[offset], expected);
        return false;
    }
    return true;
}

void Filled(void* address, std::size_t bytes) {
    auto* bytesAddress = static_cast<unsigned char*>(address);
    for (std::size_t offset = 0; offset < bytes; ++offset) bytesAddress[offset] = VerifierPattern(reinterpret_cast<std::uintptr_t>(address), offset);
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <manifest>\n", argv[0]);
        return 2;
    }
    std::ifstream manifest(argv[1]);
    if (!manifest.is_open()) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", argv[1]);
        return 1;
    }
    std::string header;
    if (!std::getline(manifest, header) || header != "anyps5-gpu-shared-memory 1") {
        std::fprintf(stderr, "FAIL: unexpected manifest header \"%s\"\n", header.c_str());
        return 1;
    }
    std::vector<Region> regions;
    std::string line;
    while (std::getline(manifest, line)) {
        std::istringstream fields(line);
        std::string kind;
        fields >> kind;
        if (kind == "object") continue;
        if (kind != "region") {
            std::fprintf(stderr, "FAIL: unexpected manifest line \"%s\"\n", line.c_str());
            return 1;
        }
        Region region{};
        if (!(fields >> std::hex >> region.address >> region.bytes) || !(fields >> region.object) || !(fields >> std::hex >> region.offset)) {
            std::fprintf(stderr, "FAIL: malformed region line \"%s\"\n", line.c_str());
            return 1;
        }
        regions.push_back(region);
    }
    Check(regions.size() >= 3, "the manifest lists the writer's ranges");
    std::map<std::string, int> objects;
    for (const auto& region : regions) {
        auto found = objects.find(region.object);
        if (found == objects.end()) {
            const int descriptor = shm_open(region.object.c_str(), O_RDWR, 0600);
            if (descriptor < 0) {
                std::fprintf(stderr, "FAIL: cannot open %s: %s\n", region.object.c_str(), std::strerror(errno));
                ++g_failures;
                continue;
            }
            found = objects.emplace(region.object, descriptor).first;
        }
        void* mapping = mmap(reinterpret_cast<void*>(region.address), region.bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
            found->second, static_cast<off_t>(region.offset));
        if (mapping == MAP_FAILED) {
            std::fprintf(stderr, "FAIL: cannot map 0x%llx+0x%llx of %s at offset 0x%llx: %s\n",
                static_cast<unsigned long long>(region.address), static_cast<unsigned long long>(region.bytes), region.object.c_str(),
                static_cast<unsigned long long>(region.offset), std::strerror(errno));
            ++g_failures;
            continue;
        }
        Check(mapping == reinterpret_cast<void*>(region.address), "the region maps at the address the writer registered");
        Check(Checked(mapping, region.bytes), "the bytes the x86_64 process wrote are visible here");
        Filled(mapping, region.bytes);
    }
    for (const auto& object : objects) ::close(object.second);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d shared-memory check(s) failed\n", g_failures);
        return 1;
    }
    std::FILE* done = std::fopen((std::string(argv[1]) + ".done").c_str(), "w");
    if (done == nullptr) {
        std::fprintf(stderr, "FAIL: cannot write the done file\n");
        return 1;
    }
    std::fputs("ok\n", done);
    std::fclose(done);
    std::printf("gpu shared memory verifier (arm64): ok over %zu region(s)\n", regions.size());
    return 0;
}
