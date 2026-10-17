#pragma once

#include "GuestBytesFingerprint.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
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
// Entries never overlap in host bytes (an insert replaces the ones it overlaps), so they are kept
// ordered by host address: a lookup or an invalidation touches only the entries at its range.
// The bound is MaxEntries and MaxBytes of guest bytes, least recently used out first. A live-map
// frame binds about 350 surfaces; at the old bound of 128 entries every frame re-uploaded about
// 300 of them (one compute or blit encoder each) although none had changed.
// `Texture` provides `bool CanShareBacking(const Descriptor&) const` and `std::size_t GuestBytes() const`.
template <typename Texture>
class GuestImageCache {
public:
    static constexpr std::size_t MaxEntries = 1024;
    static constexpr std::size_t MaxBytes = std::size_t{1} << 30;

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

    // Fast verification mode: uses Proof::Sample (512 distributed words + endpoints, ~4 KiB)
    // for textures > 4 KiB instead of re-hashing hundreds of megabytes on every expire generation.
    void SetFastVerify(bool on) { fastVerify = on; }
    [[nodiscard]] bool FastVerify() const { return fastVerify; }

    // The cached texture for a surface at `guest` (backed by `host`) whose backing `descriptor`
    // can share, if any.
    // `content` distinguishes what the texels stand for: 0 for the guest bytes themselves, another
    // value for a surface whose DCC keys say "clear" (the texels are the clear value, whatever the
    // bytes underneath hold).
    template <typename Descriptor>
    std::optional<Hit> Find(std::uint64_t guest, const std::byte* host, const Descriptor& descriptor, std::uint64_t content = 0) {
        std::lock_guard lock(mutex);
        if (auto found = entries.find(address(host)); found != entries.end()) {
            auto& entry = found->second;
            if (entry.guest == guest && entry.content == content && entry.texture->CanShareBacking(descriptor)) {
                if (entry.generation != generation) {
                    if (!proves(entry)) {
                        ++statistics.rejected;
                        erase(found);
                        ++statistics.misses;
                        return std::nullopt;
                    }
                    ++statistics.verified;
                    entry.generation = generation;
                }
                entry.stamp = ++clock;
                ++statistics.hits;
                return Hit{entry.texture, entry.compare};
            }
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
        const bool useSample = written || (fastVerify && bytes > 4096);
        const auto proof = content != 0 ? Proof::Content : written ? Proof::Pending : useSample ? Proof::Sample : Proof::Hash;
        if (auto found = entries.find(address(host)); found != entries.end()) {
            auto& entry = found->second;
            if ((proof == Proof::Hash || proof == Proof::Sample) && entry.texture == texture && entry.guest == guest && entry.content == content && entry.generation == generation) {
                entry.stamp = ++clock;
                return;
            }
        }
        eraseOverlapping(address(host), bytes);
        if (auto same = entries.find(address(host)); same != entries.end()) erase(same);
        while (!entries.empty() && (entries.size() >= MaxEntries || totalBytes + bytes > MaxBytes)) {
            erase(std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second.stamp < b.second.stamp; }));
        }
        std::uint64_t fingerprint = 0;
        if (proof == Proof::Hash) {
            fingerprint = GuestBytesFingerprint::Hash(host, bytes);
            statistics.hashedBytes += bytes;
        } else if (proof == Proof::Sample) {
            fingerprint = GuestBytesFingerprint::Sample(host, bytes);
        }
        entries.emplace(address(host), Entry{guest, address(host), bytes, std::move(texture), compare, ++clock, content, generation, generation, proof, fingerprint});
        totalBytes += bytes;
        ++statistics.inserts;
    }

    // The copy-back of a surface Insert recorded as written completed: its guest bytes now equal
    // `texture`. Takes the sample that proves them after a later Expire.
    void Seal(const std::byte* host, const Texture* texture) {
        if (host == nullptr || texture == nullptr) return;
        std::lock_guard lock(mutex);
        // A later write of the same texture may still be in flight; completions run in order, so
        // the last one's sample stands.
        auto found = entries.find(address(host));
        if (found == entries.end()) return;
        auto& entry = found->second;
        if (entry.texture.get() != texture || (entry.proof != Proof::Pending && entry.proof != Proof::Sample)) return;
        entry.proof = Proof::Sample;
        entry.fingerprint = GuestBytesFingerprint::Sample(host, entry.bytes);
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
        totalBytes = 0;
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

    // Guest bytes the entries cover.
    [[nodiscard]] std::size_t Bytes() const {
        std::lock_guard lock(mutex);
        return totalBytes;
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
                statistics.hashedBytes += entry.bytes;
                return GuestBytesFingerprint::Hash(bytes, entry.bytes) == entry.fingerprint;
            case Proof::Sample:
                return GuestBytesFingerprint::Sample(bytes, entry.bytes) == entry.fingerprint;
        }
        return false;
    }

    static std::uintptr_t address(const std::byte* host) { return reinterpret_cast<std::uintptr_t>(host); }

    using Entries = std::map<std::uintptr_t, Entry>;

    void erase(typename Entries::iterator entry) {
        totalBytes -= entry->second.bytes;
        entries.erase(entry);
    }

    void eraseOverlapping(std::uintptr_t begin, std::size_t bytes) {
        const auto end = begin + bytes;
        // Entries are disjoint: only the last one starting before `begin` can reach into the range.
        auto entry = entries.lower_bound(begin);
        if (entry != entries.begin()) {
            const auto before = std::prev(entry);
            if (begin < before->second.host + before->second.bytes) entry = before;
        }
        while (entry != entries.end() && entry->second.host < end) {
            const auto next = std::next(entry);
            if (begin < entry->second.host + entry->second.bytes) erase(entry);
            entry = next;
        }
    }

    mutable std::mutex mutex;
    Entries entries;
    std::size_t totalBytes = 0;
    std::uint64_t clock = 0;
    std::uint64_t generation = 1;
    bool fastVerify = false;
    Statistics statistics;
};

}
