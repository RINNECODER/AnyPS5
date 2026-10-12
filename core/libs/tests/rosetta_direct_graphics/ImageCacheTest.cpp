// The Metal driver's cross-draw image cache bookkeeping (GuestImageCache.hpp) and the guest stores
// that invalidate it or release a wait (PacketStoreRange.hpp). A stale entry would draw with old
// texels, so these cover what may and may not keep an entry.
#include "prx/libSceAgcDriver/Execution/Metal/PacketStoreRange.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/GuestImageCache.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) ++failures;
}

struct Descriptor {
    std::uint64_t base;
    std::uint32_t width;
};

struct FakeTexture {
    Descriptor descriptor;
    std::size_t bytes;
    bool CanShareBacking(const Descriptor& other) const { return other.base == descriptor.base && other.width == descriptor.width; }
    std::size_t GuestBytes() const { return bytes; }
};

using Cache = AgcDriver::Metal::GuestImageCache<FakeTexture>;
using AgcDriver::Metal::GuestStoreRange;
using AgcDriver::Metal::OwnLabelStores;
using AgcDriver::Metal::PacketStoreRange;

void CacheTests() {
    std::vector<std::byte> memory(0x10000);
    auto* host = memory.data();
    Cache cache;
    auto a = std::make_shared<FakeTexture>(FakeTexture{{0x1000, 64}, 0x1000});
    auto b = std::make_shared<FakeTexture>(FakeTexture{{0x4000, 32}, 0x800});
    cache.Insert(0x1000, host + 0x1000, a, false);
    cache.Insert(0x4000, host + 0x4000, b, true);

    auto hit = cache.Find(0x1000, host + 0x1000, Descriptor{0x1000, 64});
    Check(hit && hit->texture == a && !hit->compare, "a surface bound again finds its texture");
    Check(!cache.Find(0x1000, host + 0x1000, Descriptor{0x1000, 128}), "a descriptor that cannot share the backing misses");
    Check(!cache.Find(0x1000, host + 0x2000, Descriptor{0x1000, 64}), "the same guest address backed by other host memory misses");
    hit = cache.Find(0x4000, host + 0x4000, Descriptor{0x4000, 32});
    Check(hit && hit->texture == b && hit->compare, "the compare flag comes back with the texture");

    cache.Invalidate(host + 0x1fff, 1);
    Check(!cache.Find(0x1000, host + 0x1000, Descriptor{0x1000, 64}), "a store into the last byte of a surface drops it");
    Check(cache.Find(0x4000, host + 0x4000, Descriptor{0x4000, 32}).has_value(), "a store elsewhere keeps other surfaces");
    cache.Invalidate(host + 0x3000, 0x1000);
    Check(cache.Find(0x4000, host + 0x4000, Descriptor{0x4000, 32}).has_value(), "a store ending where a surface begins keeps it");

    // A written surface replaces every overlapping entry, including another view's.
    auto c = std::make_shared<FakeTexture>(FakeTexture{{0x3800, 16}, 0x1000});
    cache.Insert(0x3800, host + 0x3800, c, false);
    Check(!cache.Find(0x4000, host + 0x4000, Descriptor{0x4000, 32}), "an overlapping insert replaces the older surface");
    Check(cache.Size() == 1, "only the newest of overlapping surfaces stays");

    cache.Clear(2);
    Check(cache.Size() == 0, "clear drops every surface");
    const auto statistics = cache.TakeStatistics();
    Check(statistics.clears == 1 && statistics.clearsBy[2] == 1 && statistics.inserts == 3, "statistics count inserts and clears by reason");

    // Bounded: the least recently used entry goes first.
    std::vector<std::shared_ptr<FakeTexture>> many;
    for (std::size_t i = 0; i < Cache::MaxEntries; ++i) {
        many.push_back(std::make_shared<FakeTexture>(FakeTexture{{0x100 * i, 1}, 0x100}));
        cache.Insert(0x100 * i, host + 0x100 * i, many.back(), false);
    }
    Check(cache.Find(0, host, Descriptor{0, 1}).has_value(), "a full cache still finds its oldest entry");
    auto extra = std::make_shared<FakeTexture>(FakeTexture{{0x9000, 1}, 0x100});
    cache.Insert(0x9000, host + 0x9000, extra, false);
    Check(cache.Size() == Cache::MaxEntries, "a full cache stays at its bound");
    Check(cache.Find(0, host, Descriptor{0, 1}).has_value(), "the entry used last survives eviction");
    Check(!cache.Find(0x100, host + 0x100, Descriptor{0x100, 1}), "the least recently used entry is evicted");
}

void StoreRangeTests() {
    const auto header = [](std::uint32_t opcode, std::size_t words) { return 0xc0000000u | (static_cast<std::uint32_t>(words - 2) << 16u) | (opcode << 8u); };
    // RELEASE_MEM with DATA_SEL 1 (32-bit data) to 0x12345678.
    const std::array<std::uint32_t, 8> release{header(0x49, 8), 0, 1u << 29u, 0x12345678u, 0, 7, 0, 0};
    auto range = PacketStoreRange(release);
    Check(range && range->begin == 0x12345678u && range->bytes == 4, "RELEASE_MEM with 32-bit data stores one dword at its data address");
    const std::array<std::uint32_t, 8> noData{header(0x49, 8), 0, 0, 0x12345678u, 0, 7, 0, 0};
    range = PacketStoreRange(noData);
    Check(range && range->bytes == 0, "RELEASE_MEM without data stores nothing");
    // WRITE_DATA of three dwords to 0x2_0000_1000, then with WR_ONE_ADDR.
    const std::array<std::uint32_t, 7> write{header(0x37, 7), 0, 0x1000, 2, 1, 2, 3};
    range = PacketStoreRange(write);
    Check(range && range->begin == 0x200001000ull && range->bytes == 12, "WRITE_DATA stores its payload");
    auto oneAddress = write;
    oneAddress[1] = 0x10000u;
    range = PacketStoreRange(oneAddress);
    Check(range && range->bytes == 4, "WRITE_DATA with one address stores one dword");
    // DMA_DATA of 0x40000 bytes to 0x8000.
    const std::array<std::uint32_t, 7> dma{header(0x50, 7), 2u << 29u, 0xffffffffu, 0, 0x8000, 0, 0x40000};
    range = PacketStoreRange(dma);
    Check(range && range->begin == 0x8000 && range->bytes == 0x40000, "DMA_DATA stores its byte count");
    Check(range->Overlaps(0x47fff, 1) && !range->Overlaps(0x48000, 0x100), "a store range overlaps exactly its bytes");
    const std::array<std::uint32_t, 3> shortCopy{header(0x40, 3), 0, 0};
    range = PacketStoreRange(shortCopy);
    Check(range && range->bytes == GuestStoreRange::Everything().bytes && range->Overlaps(~0ull - 4, 4), "an undecodable store covers all memory");
    const std::array<std::uint32_t, 4> state{header(0x69, 4), 0, 0, 0};
    Check(!PacketStoreRange(state), "a register write stores nothing to memory");
}

void OwnLabelTests() {
    OwnLabelStores labels;
    const std::array<std::byte, 4> one{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<std::byte, 4> two{std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}};
    Check(!labels.Holds(0x100, one), "an address the driver never stored to is the guest's");
    labels.Record(0x100, one);
    Check(labels.Holds(0x100, one), "a label still holding the driver's value was released by the GPU");
    Check(!labels.Holds(0x100, two), "a label the guest rewrote is the guest's");
    Check(!labels.Holds(0x0fe, one), "a wait on bytes partly outside the driver's store is the guest's");
    labels.Reset();
    Check(!labels.Holds(0x100, one), "a new submission forgets the driver's stores");
}

}

int main() {
    CacheTests();
    StoreRangeTests();
    OwnLabelTests();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("all image cache checks passed\n");
    return EXIT_SUCCESS;
}
