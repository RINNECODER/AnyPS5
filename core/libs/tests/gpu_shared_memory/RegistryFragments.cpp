#include "prx/libc/include/GpuSharedMemory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
}

std::vector<std::string> RegionLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream manifest(path);
    std::string line;
    while (std::getline(manifest, line)) {
        if (line.rfind("region ", 0) == 0) lines.push_back(line);
    }
    return lines;
}

std::string RegionLine(std::uint64_t address, std::uint64_t bytes, const char* object, std::uint64_t offset) {
    char text[256];
    std::snprintf(text, sizeof(text), "region 0x%llx 0x%llx %s 0x%llx", static_cast<unsigned long long>(address),
        static_cast<unsigned long long>(bytes), object, static_cast<unsigned long long>(offset));
    return text;
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <manifest>\n", argv[0]);
        return 2;
    }
    const std::string manifest = argv[1];
    const std::string unwritable = manifest + ".missing/manifest.txt";
    setenv(GpuSharedMemory::EnableEnvironment, "1", 1);
    setenv(GpuSharedMemory::ManifestEnvironment, manifest.c_str(), 1);
    Check(GpuSharedMemory::GpuSharedMemoryEnabled_nid_postfix(), "the shared-memory flag is on");

    char object[GpuSharedMemory::MaximumObjectName] = {};
    Check(GpuSharedMemory::GpuSharedMemoryCreateObject_nid_postfix("frag", 0x40000, object, sizeof(object)) >= 0, "create a backing object");

    Check(GpuSharedMemory::GpuSharedMemoryRegister_nid_postfix(0x9000000000ull, 0x10000, object, 0x1000), "register a whole range");
    GpuSharedMemory::GpuSharedMemoryUnregister_nid_postfix(0x9000004000ull, 0x4000);
    Check(GpuSharedMemory::GpuSharedMemoryPublish_nid_postfix(), "publish after a partial unregister");
    {
        const auto lines = RegionLines(manifest);
        Check(lines.size() == 2, "a partial unregister keeps the two fragments");
        if (lines.size() == 2) {
            Check(lines[0] == RegionLine(0x9000000000ull, 0x4000, object, 0x1000), "the head keeps its object offset");
            Check(lines[1] == RegionLine(0x9000008000ull, 0x8000, object, 0x9000), "the tail advances the object offset by the bytes removed");
        }
    }

    Check(GpuSharedMemory::GpuSharedMemoryRegister_nid_postfix(0x9000010000ull, 0x10000, object, 0x20000), "register a second range");
    Check(GpuSharedMemory::GpuSharedMemoryRegister_nid_postfix(0x9000014000ull, 0x4000, object, 0x30000), "overwrite the middle of it");
    Check(GpuSharedMemory::GpuSharedMemoryPublish_nid_postfix(), "publish after a partial register");
    {
        const auto lines = RegionLines(manifest);
        Check(lines.size() == 5, "a partial register keeps the fragments around it");
        if (lines.size() == 5) {
            Check(lines[2] == RegionLine(0x9000010000ull, 0x4000, object, 0x20000), "the head of the overwritten range is kept");
            Check(lines[3] == RegionLine(0x9000014000ull, 0x4000, object, 0x30000), "the new range is in the middle");
            Check(lines[4] == RegionLine(0x9000018000ull, 0x8000, object, 0x28000), "the tail of the overwritten range is kept");
        }
    }

    Check(GpuSharedMemory::GpuSharedMemoryPublish_nid_postfix(), "a publish with nothing changed is a success");

    Check(GpuSharedMemory::GpuSharedMemoryRegister_nid_postfix(0x9000020000ull, 0x4000, object, 0x100), "register a third range");
    setenv(GpuSharedMemory::ManifestEnvironment, unwritable.c_str(), 1);
    Check(!GpuSharedMemory::GpuSharedMemoryPublish_nid_postfix(), "an unwritable manifest path is reported as a failure");
    setenv(GpuSharedMemory::ManifestEnvironment, manifest.c_str(), 1);
    Check(GpuSharedMemory::GpuSharedMemoryPublish_nid_postfix(), "the failed publish is retried after the path is usable again");
    {
        const auto lines = RegionLines(manifest);
        Check(lines.size() == 6, "the retry publishes the changed set");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d registry check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("gpu shared memory registry: ok");
    return 0;
}
