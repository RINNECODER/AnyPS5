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
// They may **overlap**: the title binds one 128 MiB surface whole and its two 32 MiB sub-ranges
// inside it, and the same bytes as two format families, so an insert that dropped every entry it
// overlapped made those alternatives evict each other and re-upload every frame (REPORT6). An
// insert keeps them; it supersedes only the entries that describe the same surface (same guest
// address, content, byte count and geometry), and a write drops every entry whose bytes it
// overlaps (Invalidate), which is what keeps an overlapping entry from being served stale: an
// entry is re-proved (or is in the generation it was inserted in) before every use.
// The bound is MaxEntries and MaxBytes of guest bytes (an overlap counts once per entry), least
// recently used out first. A live-map frame binds about 350 surfaces; at the old bound of 128
// entries every frame re-uploaded about 300 of them (one compute or blit encoder each) although
// none had changed.
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

    enum class Cause : unsigned { Other, WrittenImage, WritableBuffer, DccMetadata, BdaWrittenPage, KnownStore, DriverStore, Count };

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
        std::uint64_t invalidated = 0;
        std::uint64_t invalidatedBy[8]{};
        std::uint64_t evicted = 0;
        std::uint64_t replaced = 0;
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
        auto found = entries.lower_bound(address(host));
        while (found != entries.end() && found->first == address(host)) {
            auto& entry = found->second;
            // Several entries may share a host base (a surface and its sub-ranges, two format
            // families of the same bytes): the one whose backing serves this descriptor answers.
            if (entry.guest != guest || entry.content != content || !entry.texture->CanShareBacking(descriptor)) {
                ++found;
                continue;
            }
            if (entry.generation != generation && !proves(entry)) {
                ++statistics.rejected;
                found = erase(found);
                continue;
            }
            if (entry.generation != generation) {
                ++statistics.verified;
                entry.generation = generation;
            }
            entry.stamp = ++clock;
            ++statistics.hits;
            return Hit{entry.texture, entry.compare};
        }
        ++statistics.misses;
        return std::nullopt;
    }

    // Re-proves, for a draw translation record that bound `texture` and read `guest` at `host`, the
    // same lazy proof an Expire defers: the record's own images only, not every entry. False when a
    // write dropped the entry, it now describes another guest range or texture, or its bytes
    // changed; the caller drops its record and re-prepares it.
    [[nodiscard]] bool Revalidate(const std::byte* host, std::uint64_t guest, std::uint64_t content, const Texture* texture) {
        if (host == nullptr || texture == nullptr) return false;
        std::lock_guard lock(mutex);
        auto found = entries.lower_bound(address(host));
        while (found != entries.end() && found->first == address(host)) {
            auto& entry = found->second;
            if (entry.guest != guest || entry.content != content || entry.texture.get() != texture) {
                ++found;
                continue;
            }
            if (entry.generation != generation) {
                if (!proves(entry)) {
                    ++statistics.rejected;
                    erase(found);
                    return false;
                }
                ++statistics.verified;
                entry.generation = generation;
            }
            entry.stamp = ++clock;
            return true;
        }
        return false;
    }

    // Records that `texture`'s texels now equal the guest bytes at `guest`/`host` (or, with a
    // nonzero `content`, the clear value it stands for). An entry that describes the same surface
    // (same guest address, content, byte count and geometry) is superseded; every other entry whose
    // host bytes overlap these is kept, because it describes a different surface of the same bytes
    // and the write that changed them dropped it already (Invalidate). `written`: the work that
    // made it writes the surface, so its bytes match only once its copy-back completed (Seal). An
    // uploaded surface already cached as this texture keeps its proof.
    void Insert(std::uint64_t guest, const std::byte* host, std::shared_ptr<Texture> texture, bool compare, std::uint64_t content = 0,
        bool written = false) {
        if (texture == nullptr || host == nullptr) return;
        const auto bytes = texture->GuestBytes();
        std::lock_guard lock(mutex);
        const bool useSample = written || (fastVerify && bytes > 4096);
        const auto proof = content != 0 ? Proof::Content : written ? Proof::Pending : useSample ? Proof::Sample : Proof::Hash;
        auto found = entries.lower_bound(address(host));
        while (found != entries.end() && found->first == address(host)) {
            auto& entry = found->second;
            if ((proof == Proof::Hash || proof == Proof::Sample) && entry.texture == texture && entry.guest == guest &&
                entry.content == content && entry.generation == generation) {
                entry.stamp = ++clock;
                return;
            }
            // The same surface re-uploaded (its bytes changed, or its entry was dropped): the new
            // entry supersedes it. Two format families of one surface cannot share a backing, so
            // both are kept.
            if (entry.guest == guest && entry.content == content && entry.bytes == bytes &&
                entry.texture->CanShareBackingIgnoringDcc(texture->Descriptor())) {
                found = erase(found);
                ++statistics.replaced;
                continue;
            }
            ++found;
        }
        while (!entries.empty() && (entries.size() >= MaxEntries || totalBytes + bytes > MaxBytes)) {
            const auto victim = std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second.stamp < b.second.stamp; });
            erase(victim);
            ++statistics.evicted;
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
        auto found = entries.lower_bound(address(host));
        while (found != entries.end() && found->first == address(host)) {
            auto& entry = found->second;
            if (entry.texture.get() != texture) {
                ++found;
                continue;
            }
            if (entry.proof != Proof::Pending && entry.proof != Proof::Sample) return;
            entry.proof = Proof::Sample;
            entry.fingerprint = GuestBytesFingerprint::Sample(host, entry.bytes);
            return;
        }
    }

    void Invalidate(const std::byte* host, std::size_t bytes, Cause cause = Cause::Other) {
        if (host == nullptr || bytes == 0) return;
        std::lock_guard lock(mutex);
        const auto dropped = eraseOverlapping(address(host), bytes);
        if (dropped == 0) return;
        statistics.invalidated += dropped;
        statistics.invalidatedBy[static_cast<unsigned>(cause) % 8] += dropped;
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

    // The Expire generation: a record that read a surface with no entry here is valid only while
    // this is unchanged (it has no proof of its own to re-run).
    [[nodiscard]] std::uint64_t Generation() const {
        std::lock_guard lock(mutex);
        return generation;
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

    using Entries = std::multimap<std::uintptr_t, Entry>;

    typename Entries::iterator erase(typename Entries::iterator entry) {
        totalBytes -= entry->second.bytes;
        return entries.erase(entry);
    }

    // Entries may overlap, so an entry starting before `begin` can still reach into the range and
    // a walk back from `lower_bound` is not enough: every entry starting before `end` is examined.
    std::size_t eraseOverlapping(std::uintptr_t begin, std::size_t bytes) {
        const auto end = begin + bytes;
        std::size_t dropped = 0;
        for (auto entry = entries.begin(); entry != entries.end() && entry->first < end;) {
            const auto next = std::next(entry);
            if (begin < entry->second.host + entry->second.bytes) {
                erase(entry);
                ++dropped;
            }
            entry = next;
        }
        return dropped;
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
