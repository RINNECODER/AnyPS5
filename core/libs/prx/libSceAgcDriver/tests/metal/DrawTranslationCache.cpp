// The per-draw translation cache (DrawTranslationCache.hpp) returns a draw's translation only while
// its key words and the guest bytes its capture read are unchanged: a changed descriptor or constant
// must miss, and the bytes coming back must hit the variant recorded for them.
#include "prx/libSceAgcDriver/Execution/include/DrawTranslationCache.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {

using Cache = AgcDriver::DrawTranslationCache<std::string>;

// "Guest memory": host buffers addressed by their host pointers.
alignas(16) std::array<std::uint32_t, 8> descriptor{0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x7000, 0x8000};
alignas(16) std::array<std::uint32_t, 4> constants{1, 2, 3, 4};
bool unreadable = false;

bool readHost(std::uint64_t address, std::span<std::byte> destination) {
    if (unreadable) return false;
    std::memcpy(destination.data(), reinterpret_cast<const void*>(address), destination.size());
    return true;
}

std::uint64_t addressOf(const void* pointer) { return reinterpret_cast<std::uint64_t>(pointer); }

std::vector<Cache::Region> captured() {
    const auto bytes = [](const void* pointer, std::size_t size) {
        const auto* first = static_cast<const std::byte*>(pointer);
        return std::vector<std::byte>(first, first + size);
    };
    // Out of order and split across adjacent runs, as page regions come from ShaderMemory.
    return {
        {addressOf(constants.data()), bytes(constants.data(), sizeof(constants))},
        {addressOf(descriptor.data() + 4), bytes(descriptor.data() + 4, 16)},
        {addressOf(descriptor.data()), bytes(descriptor.data(), 16)},
    };
}

void expect(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}

std::string found(Cache& cache, std::span<const std::uint64_t> key) {
    const auto payload = cache.Find(key, &readHost);
    return payload ? *payload : std::string("<miss>");
}

}

int main() {
    try {
        Cache cache;
        const std::array<std::uint64_t, 4> key{0xabc, 7, 0x10, 0x20};
        cache.Insert(key, captured(), std::make_shared<const std::string>("A"));
        expect(found(cache, key) == "A", "unchanged inputs and bytes hit");

        descriptor[5] ^= 1u;
        expect(found(cache, key) == "<miss>", "a changed descriptor word (in the second adjacent run) misses");
        descriptor[5] ^= 1u;
        expect(found(cache, key) == "A", "the restored descriptor hits again");

        constants[3] = 99;
        expect(found(cache, key) == "<miss>", "changed constant contents miss");
        cache.Insert(key, captured(), std::make_shared<const std::string>("B"));
        expect(found(cache, key) == "B", "the new contents hit their own variant");
        constants[3] = 4;
        expect(found(cache, key) == "A", "the old contents still hit the old variant");

        const std::array<std::uint64_t, 4> userData{0xabc, 8, 0x10, 0x20};
        expect(found(cache, userData) == "<miss>", "a changed key word (user data) misses");
        const std::array<std::uint64_t, 3> shorter{0xabc, 7, 0x10};
        expect(found(cache, shorter) == "<miss>", "a key that is a prefix of a stored key misses");

        unreadable = true;
        expect(found(cache, key) == "<miss>", "unreadable guest memory misses");
        unreadable = false;

        const auto counters = cache.Snapshot();
        expect(counters.hits == 4 && counters.contentMisses == 3 && counters.absent == 2,
            "counters split hits, content misses and new keys");
        std::puts("draw translation cache: changed descriptors, constants and key words miss");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        return 1;
    }
}
