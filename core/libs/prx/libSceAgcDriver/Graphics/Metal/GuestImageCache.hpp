#pragma once

#include "GuestBytesFingerprint.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
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
//   use. An uploaded surface proves it with the guest write tracker when one is set (below), or
//   else with a hash of all its bytes (taken when it was uploaded); one the GPU wrote, with a
//   sample of its bytes taken once its copy-back completed (Seal), which catches the guest clearing or rewriting it but not patching a few bytes; one whose DCC
//   keys say "clear" needs no proof (its texels are the clear value the keys name). Entries
//   still waiting for their copy-back are in flight and kept. Before this, every such point
//   dropped the whole cache, so every surface was uploaded again at least once per frame.
// - a mapping change or a failure calls Clear, which drops everything.
// Write tracking (SetWriteTracker): the driver's tracker (GuestImageWriteTracker.hpp, over page
// write protection) learns which pages the CPU wrote since a range was last collected. An upload
// collects its surface's pages before the GPU reads them (Track) and the entry keeps the
// generation; after an Expire it proves itself by collecting again and asking whether any block
// of the surface was stamped since, which costs a walk of its page states instead of a hash of
// all its bytes, and is exact: any CPU store, even of one byte, fails it. A surface the CPU did
// write goes back to hashing for a while (its later uploads are not tracked), so a title streaming
// into a texture every frame does not take a write fault per page per frame. Ranges the tracker does not
// cover (aliased mappings, an unwatched process) are hashed as before.
// Entries are keyed by guest address and host address (so a remapped range never hits) and
// invalidated by host range (so a write through another guest alias of the same memory counts).
// `Texture` provides `bool CanShareBacking(const Descriptor&) const` and `std::size_t GuestBytes() const`.
// `collect` collects the CPU writes to a host range and returns the tracker generation that stamps
// them (0 when the range is not tracked); `unchanged` tells whether no block of the range was
// stamped after `generation`. Both run under the cache's lock. GuestImageWriteTracker.hpp has the
// driver's.
struct GuestImageWriteTracker {
    std::uint64_t (*collect)(const std::byte* host, std::size_t bytes) = nullptr;
    bool (*unchanged)(const std::byte* host, std::size_t bytes, std::uint64_t generation) = nullptr;
};

template <typename Texture>
class GuestImageCache {
public:
    static constexpr std::size_t MaxEntries = 128;

    struct Hit {
        std::shared_ptr<Texture> texture;
        bool compare;
        // The tracker generation the entry proves itself against (0: not tracked), for Insert.
        std::uint64_t tracked = 0;
    };

    using WriteTracker = GuestImageWriteTracker;

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
        // Proofs the write tracker answered, and tracked entries it rejected (a CPU store).
        std::uint64_t trackedProofs = 0;
        std::uint64_t trackedRejected = 0;
    };

    void SetWriteTracker(WriteTracker tracker) {
        std::lock_guard lock(mutex);
        writeTracker = tracker.collect != nullptr && tracker.unchanged != nullptr ? tracker : WriteTracker{};
    }

    // Before an upload reads the surface at `host`: collects its pages' CPU writes and returns the
    // generation to Insert it with (0: hash it instead, no tracker or a surface the CPU writes).
    std::uint64_t Track(const std::byte* host, std::size_t bytes) {
        if (host == nullptr || bytes == 0) return 0;
        std::lock_guard lock(mutex);
        if (writeTracker.collect == nullptr) return 0;
        const auto begin = address(host);
        std::erase_if(cpuWritten, [&](const CpuWritten& written) { return generation - written.generation > CpuWrittenExpiries; });
        for (const auto& written : cpuWritten)
            if (written.begin < begin + bytes && begin < written.end) return 0;
        return writeTracker.collect(host, bytes);
    }

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
            return Hit{entry->texture, entry->compare, entry->proof == Proof::Tracked ? entry->tracked : 0};
        }
        ++statistics.misses;
        return std::nullopt;
    }

    // Records that `texture`'s texels now equal the guest bytes at `guest`/`host` (or, with a
    // nonzero `content`, the clear value it stands for); replaces every entry whose host bytes
    // overlap them. `written`: the work that made it writes the surface, so its bytes match only
    // once its copy-back completed (Seal). `tracked`: the generation Track returned before the
    // upload read the bytes (or a Hit's), which proves an uploaded surface instead of a hash. An
    // uploaded surface already cached as this texture keeps its proof.
    void Insert(std::uint64_t guest, const std::byte* host, std::shared_ptr<Texture> texture, bool compare, std::uint64_t content = 0,
        bool written = false, std::uint64_t tracked = 0) {
        if (texture == nullptr || host == nullptr) return;
        const auto bytes = texture->GuestBytes();
        std::lock_guard lock(mutex);
        const auto proof = content != 0 ? Proof::Content : written ? Proof::Pending : tracked != 0 && writeTracker.collect != nullptr ? Proof::Tracked : Proof::Hash;
        for (auto& entry : entries) {
            if ((proof != Proof::Hash && proof != Proof::Tracked) || entry.texture != texture || entry.host != address(host) || entry.guest != guest ||
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
            fingerprint = GuestBytesFingerprint::Hash(host, bytes);
            statistics.hashedBytes += bytes;
        }
        entries.push_back({guest, address(host), bytes, std::move(texture), compare, ++clock, content, generation, generation, proof, fingerprint,
            proof == Proof::Tracked ? tracked : 0});
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
    enum class Proof : std::uint8_t { Content, Pending, Hash, Sample, Tracked };

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
        // Proof::Tracked: the tracker generation the bytes were collected at before the upload.
        std::uint64_t tracked;
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
            case Proof::Tracked: {
                ++statistics.trackedProofs;
                if (writeTracker.collect != nullptr && writeTracker.collect(bytes, entry.bytes) != 0 &&
                    writeTracker.unchanged(bytes, entry.bytes, entry.tracked)) return true;
                ++statistics.trackedRejected;
                // The CPU writes this surface: hash it from now on (a bounded list, oldest first).
                if (cpuWritten.size() >= MaxCpuWritten) cpuWritten.erase(cpuWritten.begin());
                cpuWritten.push_back({entry.host, entry.host + entry.bytes, generation});
                return false;
            }
        }
        return false;
    }

    static std::uintptr_t address(const std::byte* host) { return reinterpret_cast<std::uintptr_t>(host); }

    void eraseOverlapping(std::uintptr_t begin, std::size_t bytes) {
        const auto end = begin + bytes;
        std::erase_if(entries, [&](const Entry& entry) { return entry.host < end && begin < entry.host + entry.bytes; });
    }

    static constexpr std::size_t MaxCpuWritten = 64;
    // How long (in Expires, a few per submission) a written surface stays hashed before it is
    // tracked again: a surface written once (or marked written by a host write over its pages)
    // returns to tracking, one the title streams into is listed again by its next store.
    static constexpr std::uint64_t CpuWrittenExpiries = 256;
    struct CpuWritten {
        std::uintptr_t begin;
        std::uintptr_t end;
        std::uint64_t generation;
    };

    mutable std::mutex mutex;
    WriteTracker writeTracker;
    // Host ranges of tracked surfaces the CPU wrote: their uploads are hashed, not tracked.
    std::vector<CpuWritten> cpuWritten;
    std::vector<Entry> entries;
    std::uint64_t clock = 0;
    std::uint64_t generation = 1;
    Statistics statistics;
};

}
