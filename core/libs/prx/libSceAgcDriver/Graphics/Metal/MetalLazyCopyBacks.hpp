#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALLAZYCOPYBACKS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALLAZYCOPYBACKS_HPP

#include "MetalTexture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

// Render-target and storage-image copy-backs that run only when someone needs the guest bytes.
//
// Every draw or dispatch that wrote a guest surface used to copy the texture back into guest
// memory right behind itself (a blit to a linear buffer and a retile into the no-copy alias),
// about 400 copy-backs and 900 encoders per frame on the live map, and the GPU spent more time in
// the transitions around those blits than in the draws. Most of those bytes were never read:
// the next draw that binds the surface gets the texture itself from the image cache.
//
// A deferred work (committed without waiting) now records its copy-back here instead. The
// texture is the newer copy of those guest bytes until the copy-back is encoded, which happens
// GPU-side, into the open command buffer (one compute pass per surface, TextureRetileFromTexture,
// many surfaces per encoder), at the first of these points. Nothing here protects pages: there is
// no fault on a guest CPU read, so the rule is that the bytes are materialized before anyone
// outside the GPU work that follows could observe them.
// 1. Before any guest-visible signal: a label or memory store the driver performs (RELEASE_MEM,
//    WRITE_DATA, EVENT_WRITE_EOP/EOS, deferred or not), an end-of-pipe interrupt, the retirement of
//    a submission, a flip. The guest CPU learns that GPU work finished only through these, so
//    every byte the work left is in guest memory before the signal lands (the copy-back is
//    committed ahead of the signal on the same timeline, which runs signals after the work).
// 2. Before every timeline drain (MetalDriver CompletePriorGpuWorkAndCopyBack): the driver drains
//    before it reads or stores guest memory on the CPU itself (DMA_DATA and the other CPU-executed
//    packets, waits, conditions, predicates, indirect arguments, sample dumps, captures, a
//    suspend, a mapping change), so those see the bytes.
// 3. Before the driver reads guest bytes on the CPU for a draw or dispatch (the CPU-read hook:
//    index scans, DCC keys, staged or unaligned surfaces): the overlapping copy-backs only.
// 4. Before GPU work reads the bytes through guest memory instead of the texture: a buffer
//    binding (guest compute or a vertex fetch reading a render target as data), or an upload of a
//    texture other than the recorded one (another format or view of the surface): the overlapping
//    copy-backs only, encoded ahead of the reader in the same command buffer.
// 5. Before a copy-back of another texture over overlapping bytes is recorded, so recorded
//    entries stay disjoint and their order does not matter.
// Two records of one texture into the same bytes merge (their levels and layers widen).
// Not matched by address: a shader that reads a surface's guest memory through a raw (BDA) pointer
// instead of a descriptor sees the bytes only after the next point of 1 or 2. On the guest GPU such
// a read needs a cache flush first (ACQUIRE_MEM, EVENT_WRITE); those are not points here, because
// titles issue one or two per draw, which would copy back after nearly every draw again.
// Synchronous work (DrawSynchronously, a rerun inside a drain) copies back eagerly as before.
// ANYPS5_METAL_EAGER_COPYBACK=1 restores one copy-back per work.
class MetalLazyCopyBacks {
public:
    // Why bytes were materialized, for ANYPS5_METAL_GPU_TIMING statistics.
    enum class Cause : unsigned { Drain, Signal, Retire, CpuRead, GpuRead, Overlap, Mapping, Count };

    static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("ANYPS5_METAL_EAGER_COPYBACK");
            return value == nullptr || value[0] != '1';
        }();
        return enabled;
    }

    // A copy-back that was encoded: once its command buffer completed, `host` holds `texture`.
    struct Materialized {
        std::shared_ptr<MetalTexture> texture;
        const std::byte* host;
        std::uint64_t guestAddress;
        std::size_t bytes;
    };

    struct Statistics {
        std::uint64_t recorded = 0;
        std::uint64_t merged = 0;
        std::uint64_t encoded = 0;
        std::uint64_t fused = 0;
        std::uint64_t points = 0;
        std::array<std::uint64_t, static_cast<std::size_t>(Cause::Count)> by{};
    };

    // Records the copy-back of `part` of `texture` into its guest surface, `guest` + `offset` (the
    // no-copy alias, in place), whose canonical host bytes are `host` at `guestAddress`. False when
    // it cannot be deferred (the caller copies back now). A copy-back of another texture over
    // overlapping bytes is encoded into `commands` first and returned in `encodedFirst`.
    bool Record(id<MTLCommandBuffer> commands, const std::shared_ptr<MetalTexture>& texture, id<MTLBuffer> guest, std::size_t offset,
        TextureSubresources part, std::span<std::byte> host, std::uint64_t guestAddress, std::vector<Materialized>& encodedFirst) {
        if (commands == nil || guest == nil || texture == nullptr || host.empty() || !texture->UploadsInPlace(guest, offset)) return false;
        std::lock_guard lock(mutex);
        ++statistics.recorded;
        const auto aliasBegin = reinterpret_cast<std::uintptr_t>(guest.contents) + offset;
        for (auto& entry : entries) {
            if (entry.texture != texture || entry.guest != guest || entry.offset != offset || entry.host != host.data()) continue;
            entry.part.levelBegin = std::min(entry.part.levelBegin, part.levelBegin);
            entry.part.levelEnd = std::max(entry.part.levelEnd, part.levelEnd);
            entry.part.layerBegin = std::min(entry.part.layerBegin, part.layerBegin);
            entry.part.layerEnd = std::max(entry.part.layerEnd, part.layerEnd);
            ++statistics.merged;
            return true;
        }
        const auto hostBegin = reinterpret_cast<std::uintptr_t>(host.data());
        encodeMatching(commands, Cause::Overlap, encodedFirst, [&](const Entry& entry) {
            return entry.overlaps(hostBegin, hostBegin + host.size()) || entry.overlaps(aliasBegin, aliasBegin + host.size());
        });
        entries.push_back({texture, guest, offset, part, host.data(), host.size(), aliasBegin, guestAddress});
        return true;
    }

    // Whether a recorded copy-back's bytes overlap [begin, begin + bytes), matched at the canonical
    // host address and at the alias address.
    [[nodiscard]] bool Overlaps(const void* begin, std::size_t bytes) const {
        if (begin == nullptr || bytes == 0) return false;
        std::lock_guard lock(mutex);
        const auto from = reinterpret_cast<std::uintptr_t>(begin);
        return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.overlaps(from, from + bytes); });
    }

    // Encodes into `commands` the recorded copy-backs whose bytes overlap [begin, begin + bytes),
    // except those of `keep` (a binding of that very texture, which holds the newer bytes).
    std::vector<Materialized> EncodeOverlapping(id<MTLCommandBuffer> commands, const void* begin, std::size_t bytes, Cause cause,
        const MetalTexture* keep = nullptr) {
        std::vector<Materialized> done;
        if (begin == nullptr || bytes == 0) return done;
        std::lock_guard lock(mutex);
        const auto from = reinterpret_cast<std::uintptr_t>(begin);
        encodeMatching(commands, cause, done, [&](const Entry& entry) {
            return entry.texture.get() != keep && entry.overlaps(from, from + bytes);
        });
        return done;
    }

    // Encodes every recorded copy-back into `commands`.
    std::vector<Materialized> EncodeAll(id<MTLCommandBuffer> commands, Cause cause) {
        std::vector<Materialized> done;
        std::lock_guard lock(mutex);
        encodeMatching(commands, cause, done, [](const Entry&) { return true; });
        return done;
    }

    // Drops every record without encoding it (shutdown, a failed device).
    void Discard() {
        std::lock_guard lock(mutex);
        entries.clear();
    }

    [[nodiscard]] bool Empty() const {
        std::lock_guard lock(mutex);
        return entries.empty();
    }
    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }

    Statistics TakeStatistics(bool reset = true) {
        std::lock_guard lock(mutex);
        const auto result = statistics;
        if (reset) statistics = {};
        return result;
    }

private:
    struct Entry {
        std::shared_ptr<MetalTexture> texture;
        id<MTLBuffer> guest;
        std::size_t offset;
        TextureSubresources part;
        std::byte* host;
        std::size_t bytes;
        std::uintptr_t alias;
        std::uint64_t guestAddress;

        [[nodiscard]] bool overlaps(std::uintptr_t begin, std::uintptr_t end) const {
            const auto hostBegin = reinterpret_cast<std::uintptr_t>(host);
            return (hostBegin < end && begin < hostBegin + bytes) || (alias < end && begin < alias + bytes);
        }
    };

    template <typename Match>
    void encodeMatching(id<MTLCommandBuffer> commands, Cause cause, std::vector<Materialized>& done, Match&& match) {
        std::vector<Entry> taken;
        for (std::size_t i = 0; i < entries.size();) {
            if (!match(entries[i])) {
                ++i;
                continue;
            }
            taken.push_back(std::move(entries[i]));
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
        }
        if (taken.empty()) return;
        if (commands == nil) throw MetalGpuExecutionError("Metal lazy copy-back has no command buffer to encode into");
        ++statistics.points;
        statistics.by[static_cast<std::size_t>(cause)] += taken.size();
        // Entries are disjoint, so their order does not matter: the fused ones share one encoder.
        id<MTLComputeCommandEncoder> encoder = nil;
        std::vector<Entry*> blit;
        try {
            for (auto& entry : taken) {
                if (!entry.texture->FusesTransfer(false)) {
                    blit.push_back(&entry);
                    continue;
                }
                if (encoder == nil) {
                    encoder = MetalEncoders::Compute(commands);
                    if (encoder == nil) throw MetalGpuExecutionError("Metal lazy copy-back encoder creation failed");
                    encoder.label = @"lazy copy-back";
                }
                if (!entry.texture->EncodeReadback(encoder, entry.guest, entry.offset, entry.part)) {
                    throw MetalGpuExecutionError("Metal lazy copy-back is no longer in place");
                }
                ++statistics.fused;
            }
        } catch (...) {
            if (encoder != nil) [encoder endEncoding];
            throw;
        }
        if (encoder != nil) [encoder endEncoding];
        for (auto* entry : blit) {
            if (!entry->texture->EncodeReadback(commands, entry->guest, entry->offset, entry->part)) {
                throw MetalGpuExecutionError("Metal lazy copy-back is no longer in place");
            }
        }
        statistics.encoded += taken.size();
        for (auto& entry : taken) done.push_back({std::move(entry.texture), entry.host, entry.guestAddress, entry.bytes});
    }

    mutable std::mutex mutex;
    std::vector<Entry> entries;
    Statistics statistics;
};

}

#endif
