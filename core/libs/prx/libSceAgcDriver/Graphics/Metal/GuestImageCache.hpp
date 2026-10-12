#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace AgcDriver::Metal {

// Native textures whose texels equal the guest bytes they were last uploaded from or read back
// into, so a later draw or dispatch that binds the same surface can skip the upload.
//
// Every draw and dispatch binds guest-memory images by uploading them (detile, then a blit into
// a new texture) and reads written ones back; a frame binds most surfaces several times, so the
// uploads were a large share of the GPU queue's time. An entry stays valid only while nothing
// writes its guest bytes, and the cache only ever learns about the driver's own writes, so:
// - the driver clears it at every point where the guest may legitimately have written memory
//   the GPU reads (a new submission, a wait on memory or a rendering wait, a label or event
//   write, a flip, a mapping change, a failed draw or dispatch), and
// - within a stretch of draws and dispatches, every guest range the GPU may write (written
//   images, writable buffers, pages the shaders' BDA stores touched, DCC metadata) is
//   invalidated before the work that writes it runs, and an image is re-entered only after its
//   copy-back completed.
// Entries are keyed by guest address and host address (so a remapped range never hits) and
// invalidated by host range (so a write through another guest alias of the same memory counts).
// `Texture` provides `bool CanShareBacking(const Descriptor&) const` and `std::size_t GuestBytes() const`.
template <typename Texture>
class GuestImageCache {
public:
    static constexpr std::size_t MaxEntries = 128;

    struct Hit {
        std::shared_ptr<Texture> texture;
        bool compare;
    };

    struct Statistics {
        std::uint64_t hits = 0;
        std::uint64_t misses = 0;
        std::uint64_t inserts = 0;
        std::uint64_t clears = 0;
        // Clears by the caller's reason code (the driver's ForgetReason), for profiling.
        std::uint64_t clearsBy[8]{};
    };

    // The cached texture for a surface at `guest` (backed by `host`) whose backing `descriptor`
    // can share, if any.
    template <typename Descriptor>
    std::optional<Hit> Find(std::uint64_t guest, const std::byte* host, const Descriptor& descriptor) {
        std::lock_guard lock(mutex);
        for (auto& entry : entries) {
            if (entry.guest != guest || entry.host != address(host) || !entry.texture->CanShareBacking(descriptor)) continue;
            entry.stamp = ++clock;
            ++statistics.hits;
            return Hit{entry.texture, entry.compare};
        }
        ++statistics.misses;
        return std::nullopt;
    }

    // Records that `texture`'s texels now equal the guest bytes at `guest`/`host`; replaces every
    // entry whose host bytes overlap them.
    void Insert(std::uint64_t guest, const std::byte* host, std::shared_ptr<Texture> texture, bool compare) {
        if (texture == nullptr || host == nullptr) return;
        const auto bytes = texture->GuestBytes();
        std::lock_guard lock(mutex);
        eraseOverlapping(address(host), bytes);
        if (entries.size() >= MaxEntries) {
            entries.erase(std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.stamp < b.stamp; }));
        }
        entries.push_back({guest, address(host), bytes, std::move(texture), compare, ++clock});
        ++statistics.inserts;
    }

    // Forgets every entry whose host bytes overlap [host, host + bytes).
    void Invalidate(const std::byte* host, std::size_t bytes) {
        if (host == nullptr || bytes == 0) return;
        std::lock_guard lock(mutex);
        eraseOverlapping(address(host), bytes);
    }

    void Clear(unsigned reason = 0) {
        std::lock_guard lock(mutex);
        if (entries.empty()) return;
        entries.clear();
        ++statistics.clears;
        ++statistics.clearsBy[reason % 8];
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }

    // The counters so far; resets them when `reset`.
    Statistics TakeStatistics(bool reset = true) {
        std::lock_guard lock(mutex);
        const auto result = statistics;
        if (reset) statistics = {};
        return result;
    }

private:
    struct Entry {
        std::uint64_t guest;
        std::uintptr_t host;
        std::size_t bytes;
        std::shared_ptr<Texture> texture;
        bool compare;
        std::uint64_t stamp;
    };

    static std::uintptr_t address(const std::byte* host) { return reinterpret_cast<std::uintptr_t>(host); }

    void eraseOverlapping(std::uintptr_t begin, std::size_t bytes) {
        const auto end = begin + bytes;
        std::erase_if(entries, [&](const Entry& entry) { return entry.host < end && begin < entry.host + entry.bytes; });
    }

    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::uint64_t clock = 0;
    Statistics statistics;
};

}
