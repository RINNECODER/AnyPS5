// The Metal driver's image cache with the guest write tracker (GuestImageWriteTracker.hpp): an
// uploaded surface proves its guest bytes unchanged after a new submission by the tracker instead
// of a hash, so a CPU store to it between submissions, even of one byte, must still make the next
// binding upload it again. Real watched guest pages, real write faults; no GPU.
#include "prx/libSceAgcDriver/Graphics/Metal/GuestImageWriteTracker.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
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
};

struct FakeTexture {
    Descriptor descriptor;
    std::size_t bytes;
    bool CanShareBacking(const Descriptor& other) const { return other.base == descriptor.base; }
    std::size_t GuestBytes() const { return bytes; }
};

using Cache = AgcDriver::Metal::GuestImageCache<FakeTexture>;

constexpr std::size_t Block = 64 * 1024;
constexpr std::size_t TextureBytes = 4 * Block;

// One binding of the surface at `host`: the cached texture, or a new one uploaded (Track before the
// upload reads the bytes, Insert once the work completed), as MetalShaderResources::texture does.
std::shared_ptr<FakeTexture> Bind(Cache& cache, std::byte* host, bool& uploaded) {
    const auto guest = reinterpret_cast<std::uint64_t>(host);
    if (auto hit = cache.Find(guest, host, Descriptor{guest})) {
        uploaded = false;
        cache.Insert(guest, host, hit->texture, false, 0, false, hit->tracked);
        return hit->texture;
    }
    uploaded = true;
    const auto tracked = cache.Track(host, TextureBytes);
    auto texture = std::make_shared<FakeTexture>(FakeTexture{{guest}, TextureBytes});
    cache.Insert(guest, host, texture, false, 0, false, tracked);
    return texture;
}

}

int main() {
    Check(AgcDriver::Metal::GuestImageWriteStamps::Available(), "the guest page write watch is available");
    const std::size_t mappedBytes = 24 * Block;
    void* mapped = mmap(nullptr, mappedBytes + Block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapped == MAP_FAILED) return 2;
    // 64 KiB aligned, as the tracker's blocks are: surfaces in separate blocks do not share stamps.
    auto* base = reinterpret_cast<std::byte*>((reinterpret_cast<std::uintptr_t>(mapped) + Block - 1) & ~std::uintptr_t{Block - 1});
    for (std::size_t i = 0; i < mappedBytes; ++i) base[i] = static_cast<std::byte>(i * 7u);
    GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(base, mappedBytes);

    Cache cache;
    cache.SetWriteTracker(AgcDriver::Metal::WatchedImageWriteTracker());
    auto* a = base;
    auto* b = base + 8 * Block;
    bool uploaded = false;

    // Submission 1 uploads both surfaces.
    const auto first = Bind(cache, a, uploaded);
    Check(uploaded, "submission 1 uploads surface A");
    Bind(cache, b, uploaded);
    Check(uploaded, "submission 1 uploads surface B");

    // Submission 2: nothing wrote either surface. Both are served from the cache and proved by the
    // tracker, with no byte of them hashed.
    cache.Expire(1);
    static_cast<void>(cache.TakeStatistics());
    Check(Bind(cache, a, uploaded) == first && !uploaded, "an unwritten surface is served from the cache in the next submission");
    Bind(cache, b, uploaded);
    Check(!uploaded, "an unwritten second surface is served too");
    auto statistics = cache.TakeStatistics();
    Check(statistics.trackedProofs == 2 && statistics.hashedBytes == 0, "the tracker proved both surfaces without hashing their bytes");

    // Between submissions the title stores one byte into A (a write fault on the watched page).
    // The next submission must upload A again; B, in other blocks, stays cached.
    a[3 * Block + 1234] = std::byte{0x5a};
    cache.Expire(1);
    const auto second = Bind(cache, a, uploaded);
    Check(uploaded && second != first, "a one-byte CPU store between submissions makes the next binding upload the surface again");
    Bind(cache, b, uploaded);
    Check(!uploaded, "a store to another surface's blocks keeps this one cached");
    statistics = cache.TakeStatistics();
    Check(statistics.trackedRejected == 1, "the tracker rejected exactly the written surface");

    // A surface the CPU wrote is hashed from now on (no write fault per page per frame), and the
    // hash still sees the next store.
    cache.Expire(1);
    Bind(cache, a, uploaded);
    statistics = cache.TakeStatistics();
    Check(!uploaded && statistics.hashedBytes == TextureBytes && statistics.trackedProofs == 0, "the written surface is proved by its hash");
    a[Block] = std::byte{0xa5};
    cache.Expire(1);
    Bind(cache, a, uploaded);
    Check(uploaded, "a store to a hashed surface still uploads it again");

    // A host-side store (a file read into the surface under a host-write guard, as libkernel
    // makes it) is seen too: the guarded pages are reported written.
    GuestWriteWatch::GuestWriteWatchBeginHostWrite_nid_postfix(b + 2 * Block, 4096);
    std::memset(b + 2 * Block, 0x11, 4096);
    GuestWriteWatch::GuestWriteWatchEndHostWrite_nid_postfix(b + 2 * Block, 4096);
    cache.Expire(1);
    Bind(cache, b, uploaded);
    Check(uploaded, "a guarded host write into a tracked surface uploads it again");

    // Two surfaces on the same pages (another view's entry, collected separately): one surface's
    // collect taking the written page must not hide the store from the other.
    auto* c = base + 12 * Block;
    Cache other;
    other.SetWriteTracker(AgcDriver::Metal::WatchedImageWriteTracker());
    Bind(cache, c, uploaded);
    Bind(other, c, uploaded);
    c[100] = std::byte{0x77};
    cache.Expire(1);
    other.Expire(1);
    Bind(cache, c, uploaded);
    Check(uploaded, "the first cache over shared pages sees the store");
    Bind(other, c, uploaded);
    Check(uploaded, "the second cache over the same pages still sees it after the first collected them");

    // Bulk stores (memcpy and memset, which Rosetta runs as native routines) over a tracked
    // surface, in the middle of its pages and over all of it.
    auto* d = base + 14 * Block;
    Bind(cache, d, uploaded);
    std::vector<std::byte> source(TextureBytes / 2, std::byte{0x3c});
    std::memcpy(d + 12345, source.data(), source.size());
    cache.Expire(1);
    Bind(cache, d, uploaded);
    Check(uploaded, "a memcpy into a tracked surface uploads it again");
    auto* e = base + 19 * Block;
    Bind(cache, e, uploaded);
    std::memset(e + 3 * Block + 100, 0x42, 64);
    cache.Expire(1);
    Bind(cache, e, uploaded);
    Check(uploaded, "a small memset into a tracked surface uploads it again");

    std::printf("%s: image cache write tracking (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
