#pragma once

#include "GuestBytesFingerprint.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver::Metal {

// Native textures whose texels equal the guest bytes they were last uploaded from or read back
// into, so a later draw or dispatch that binds the same surface can skip the upload.
//
// Every draw and dispatch binds guest-memory images by uploading them (detile, then a blit into
// a new texture) and reads written ones back; a frame binds most surfaces several times, so the
// uploads were a large share of the GPU queue's time. An entry stays valid only while nothing
// writes its guest bytes, and the cache only learns about the driver's own writes directly:
// - within a stretch of draws and dispatches, every guest range the GPU may write (written
//   images, writable buffers, pages the shaders' BDA stores touched, DCC metadata) is
//   invalidated before the work that writes it runs, and stores the driver performs itself
//   (DMA, labels) drop the entries they overlap;
// - at every point where the guest may legitimately have written memory the GPU reads (a new
//   submission, a wait on memory or a rendering wait, a label or event write, a flip) the driver
//   calls Expire: entries stay, but each must prove its guest bytes unchanged before its next
//   use. An uploaded surface proves it with a hash of all its bytes (taken when it was uploaded);
//   one the GPU wrote, with a sample of its bytes taken once its copy-back completed (Seal),
//   which catches the guest clearing or rewriting it but not patching a few bytes; one whose DCC
//   keys say "clear" needs no proof (its texels are the clear value the keys name). Entries
//   still waiting for their copy-back are in flight and kept. Before this, every such point
//   dropped the whole cache, so every surface was uploaded again at least once per frame.
// - a mapping change or a failure calls Clear, which drops everything.
// Entries are keyed by guest address and host address (so a remapped range never hits) and
// invalidated by host range (so a write through another guest alias of the same memory counts).
// `Texture` provides `bool CanShareBacking(const Descriptor&) const` and `std::size_t GuestBytes() const`.

inline void HashHist(std::size_t bytes) {
    static std::uint64_t n[12]{}, b[12]{}; static std::uint64_t calls = 0;
    int k = 0; while (k < 11 && (std::size_t{64} << (k * 1)) * 1024 <= bytes) ++k;
    ++n[k]; b[k] += bytes;
    if (++calls % 1000 == 0) { std::fprintf(stderr, "[hashhist]"); for (int i = 0; i < 12; ++i) std::fprintf(stderr, " <%zuK:%llu/%lluMiB", (std::size_t{64} << i), (unsigned long long)n[i], (unsigned long long)(b[i] >> 20)); std::fprintf(stderr, "\n"); }
}
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
        // Entries that proved their bytes after an Expire, ones that failed, and bytes hashed.
        std::uint64_t verified = 0;
        std::uint64_t rejected = 0;
        std::uint64_t hashedBytes = 0;
    };

    // The cached texture for a surface at `guest` (backed by `host`) whose backing `descriptor`
    // can share, if any.
    // `content` distinguishes what the texels stand for: 0 for the guest bytes themselves, another
    // value for a surface whose DCC keys say "clear" (the texels are the clear value, whatever the
    // bytes underneath hold).
    template <typename Descriptor>
    std::optional<Hit> Find(std::uint64_t guest, const std::byte* host, const Descriptor& descriptor, std::uint64_t content = 0) {
        std::lock_guard lock(mutex);
        for (auto entry = entries.begin(); entry != entries.end(); ++entry) {
            if (entry->guest != guest || entry->host != address(host) || entry->content != content || !entry->texture->CanShareBacking(descriptor)) continue;
            if (entry->generation != generation) {
                if (!proves(*entry)) {
                    ++statistics.rejected;
                    entries.erase(entry);
                    break;
                }
                ++statistics.verified;
                entry->generation = generation;
            }
            entry->stamp = ++clock;
            ++statistics.hits;
            return Hit{entry->texture, entry->compare};
        }
        ++statistics.misses;
        return std::nullopt;
    }

    // Records that `texture`'s texels now equal the guest bytes at `guest`/`host` (or, with a
    // nonzero `content`, the clear value it stands for); replaces every entry whose host bytes
    // overlap them. `written`: the work that made it writes the surface, so its bytes match only
    // once its copy-back completed (Seal). An uploaded surface already cached as this texture
    // keeps its proof.
    void Insert(std::uint64_t guest, const std::byte* host, std::shared_ptr<Texture> texture, bool compare, std::uint64_t content = 0,
        bool written = false) {
        if (texture == nullptr || host == nullptr) return;
        const auto bytes = texture->GuestBytes();
        std::lock_guard lock(mutex);
        const auto proof = content != 0 ? Proof::Content : written ? Proof::Pending : Proof::Hash;
        for (auto& entry : entries) {
            if (proof != Proof::Hash || entry.texture != texture || entry.host != address(host) || entry.guest != guest ||
                entry.content != content || entry.generation != generation) continue;
            entry.stamp = ++clock;
            return;
        }
        eraseOverlapping(address(host), bytes);
        if (entries.size() >= MaxEntries) {
            entries.erase(std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.stamp < b.stamp; }));
        }
        std::uint64_t fingerprint = 0;
        if (proof == Proof::Hash) {
            HashHist(bytes); fingerprint = GuestBytesFingerprint::Hash(host, bytes);
            statistics.hashedBytes += bytes;
        }
        entries.push_back({guest, address(host), bytes, std::move(texture), compare, ++clock, content, generation, generation, proof, fingerprint});
        ++statistics.inserts;
    }

    // The copy-back of a surface Insert recorded as written completed: its guest bytes now equal
    // `texture`. Takes the sample that proves them after a later Expire.
    void Seal(const std::byte* host, const Texture* texture) {
        if (host == nullptr || texture == nullptr) return;
        std::lock_guard lock(mutex);
        for (auto& entry : entries) {
            // A later write of the same texture may still be in flight; completions run in order,
            // so the last one's sample stands.
            if (entry.host != address(host) || entry.texture.get() != texture || (entry.proof != Proof::Pending && entry.proof != Proof::Sample)) continue;
            entry.proof = Proof::Sample;
            entry.fingerprint = GuestBytesFingerprint::Sample(host, entry.bytes);
        }
    }

    // Forgets every entry whose host bytes overlap [host, host + bytes).
    void Invalidate(const std::byte* host, std::size_t bytes) {
        if (host == nullptr || bytes == 0) return;
        std::lock_guard lock(mutex);
        eraseOverlapping(address(host), bytes);
    }

    // Drops every entry (a mapping change, a failure).
    void Clear(unsigned reason = 0) {
        std::lock_guard lock(mutex);
        if (entries.empty()) return;
        entries.clear();
        ++statistics.clears;
        ++statistics.clearsBy[reason % 8];
    }

    // The guest may have written any guest memory since: every entry must prove its bytes
    // unchanged before its next use (see above).
    void Expire(unsigned reason = 0) {
        std::lock_guard lock(mutex);
        if (entries.empty()) return;
        ++generation;
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
    enum class Proof : std::uint8_t { Content, Pending, Hash, Sample };

    struct Entry {
        std::uint64_t guest;
        std::uintptr_t host;
        std::size_t bytes;
        std::shared_ptr<Texture> texture;
        bool compare;
        std::uint64_t stamp;
        std::uint64_t content;
        // The Expire generation the entry was last valid in, and how it proves itself in a later one.
        std::uint64_t generation;
        std::uint64_t inserted;
        Proof proof;
        std::uint64_t fingerprint;
    };

    bool proves(const Entry& entry) {
        const auto* bytes = reinterpret_cast<const std::byte*>(entry.host);
        switch (entry.proof) {
            case Proof::Content:
                return true;
            case Proof::Pending:
                // In flight: the guest cannot have written it yet. A copy-back that never sealed
                // (failed work) stops being trusted after a few expiries.
                return generation - entry.inserted <= 8;
            case Proof::Hash:
                statistics.hashedBytes += entry.bytes; HashHist(entry.bytes);
                return GuestBytesFingerprint::Hash(bytes, entry.bytes) == entry.fingerprint;
            case Proof::Sample:
                return GuestBytesFingerprint::Sample(bytes, entry.bytes) == entry.fingerprint;
        }
        return false;
    }

    static std::uintptr_t address(const std::byte* host) { return reinterpret_cast<std::uintptr_t>(host); }

    void eraseOverlapping(std::uintptr_t begin, std::size_t bytes) {
        const auto end = begin + bytes;
        std::erase_if(entries, [&](const Entry& entry) { return entry.host < end && begin < entry.host + entry.bytes; });
    }

    mutable std::mutex mutex;
    std::vector<Entry> entries;
    std::uint64_t clock = 0;
    std::uint64_t generation = 1;
    Statistics statistics;
};

}
