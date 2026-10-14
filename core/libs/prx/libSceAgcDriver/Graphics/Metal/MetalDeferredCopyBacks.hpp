#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEFERREDCOPYBACKS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEFERREDCOPYBACKS_HPP

#include "MetalTexture.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace AgcDriver::Metal {

// Lazy, coalesced render-target copy-backs within one batched command buffer.
//
// Every draw that renders into a guest surface copies the texture back into guest memory (a
// retile blit into the no-copy alias of those bytes) right behind itself. A run of draws into the
// same target, which is most of a frame, copied the same surface back once per draw, though
// only the last copy survives. The copy-back of a color attachment is instead recorded here for
// the open batch (MetalDevice::SetBatching) and encoded once, behind the last draw that wrote
// it, at the first of:
// - the batch committing (MetalDevice::Flush, a Wait on it): everything the CPU or the guest can
//   observe (completions, Seal, labels, flips, CPU reads) only runs after the batch completes,
//   so it sees exactly the bytes the eager copy-backs left;
// - a cache flush or pipeline barrier packet (EVENT_WRITE, ACQUIRE_MEM, the barrier the driver
//   ignores otherwise): GPU readers after it, including raw (BDA) loads of the surface's memory,
//   see the copy-back as before. A shader that reads a render target's memory without such a
//   barrier is not coherent on the guest GPU either;
// - later work in the batch binding guest bytes that overlap a recorded surface through guest
//   memory (a buffer, an upload of a texture other than the recorded one): encoded first, so the
//   binding reads what the eager copy-back left;
// - a copy-back of another texture overlapping it being recorded: encoded first, so recorded
//   surfaces never overlap and their order does not matter.
// Two records of the same texture into the same bytes merge (their levels and layers widen).
// The texture stays authoritative in between: later draws that bind the same surface get it from
// the image cache (GuestImageCache) and never read the guest bytes.
// ANYPS5_METAL_EAGER_COPYBACK=1 restores one copy-back per work.
//
// Not thread-safe: the owner serializes every use (the driver's gpuMutex), as for batching.
class MetalDeferredCopyBacks {
public:
    static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("ANYPS5_METAL_EAGER_COPYBACK");
            return value == nullptr || value[0] != '1';
        }();
        return enabled;
    }

    // Records the copy-back of `part` of `texture` into `guest` + `offset` (in place) for
    // `commands`, the open batch. `host` is the surface's guest bytes at their canonical host
    // address (the borrowed range; an alias buffer may map them elsewhere), which readers are
    // matched against. False when it cannot be deferred (the caller encodes it now).
    bool Defer(id<MTLCommandBuffer> commands, const std::shared_ptr<MetalTexture>& texture, id<MTLBuffer> guest, std::size_t offset,
        TextureSubresources part, std::span<const std::byte> host = {}) {
        if (commands == nil || guest == nil || !texture || !texture->UploadsInPlace(guest, offset)) return false;
        if (!entries.empty() && batch != commands) {
            // The batch these were recorded for committed without Flush: a driver bug. The new
            // command buffer runs after it, so encoding them first still orders them before any
            // later reader; only that batch's own completions may have seen stale bytes.
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) std::fprintf(stderr, "[agc] deferred copy-backs outlived their command buffer; encoding them late\n");
            EncodeAll(commands);
        }
        batch = commands;
        const auto begin = host.empty() ? reinterpret_cast<std::uintptr_t>(guest.contents) + offset : reinterpret_cast<std::uintptr_t>(host.data());
        const auto end = begin + (host.empty() ? texture->GuestBytes() : host.size());
        ++requested;
        for (auto& entry : entries) {
            if (entry.texture == texture && entry.guest == guest && entry.offset == offset) {
                entry.part.levelBegin = std::min(entry.part.levelBegin, part.levelBegin);
                entry.part.levelEnd = std::max(entry.part.levelEnd, part.levelEnd);
                entry.part.layerBegin = std::min(entry.part.layerBegin, part.layerBegin);
                entry.part.layerEnd = std::max(entry.part.layerEnd, part.layerEnd);
                return true;
            }
        }
        encodeOverlapping(begin, end, nullptr);
        // Called through a pointer, so the owner (MetalDevice) needs no MetalTexture symbols.
        constexpr auto encodeReadback = [](const MetalTexture& surface, id<MTLCommandBuffer> into, id<MTLBuffer> bytes, std::size_t at,
                                            TextureSubresources levels) { return surface.EncodeReadback(into, bytes, at, levels); };
        entries.push_back({texture, guest, offset, part, begin, end, +encodeReadback});
        return true;
    }

    // Encodes, into the open batch, the recorded copy-backs whose guest bytes overlap host
    // [begin, end), except that of `keep` (a binding of that very texture, which holds them).
    void EncodeOverlapping(const void* begin, std::size_t bytes, const MetalTexture* keep = nullptr) {
        if (entries.empty() || begin == nullptr) return;
        const auto from = reinterpret_cast<std::uintptr_t>(begin);
        encodeOverlapping(from, from + bytes, keep);
    }

    // Encodes every recorded copy-back into `commands` (the open batch, or with split readback
    // timing a command buffer enqueued right behind it).
    void EncodeAll(id<MTLCommandBuffer> commands) {
        auto taken = std::move(entries);
        entries.clear();
        batch = nil;
        for (auto& entry : taken) encode(commands, entry);
    }

    // Encodes every recorded copy-back into the batch they were recorded for (a barrier).
    void EncodeAll() {
        if (!entries.empty()) EncodeAll(batch);
    }

    [[nodiscard]] bool Empty() const { return entries.empty(); }
    // Copy-backs recorded, and the ones actually encoded (the rest merged into a later one).
    [[nodiscard]] std::uint64_t Requested() const { return requested; }
    [[nodiscard]] std::uint64_t Encoded() const { return encoded; }

private:
    struct Entry {
        std::shared_ptr<MetalTexture> texture;
        id<MTLBuffer> guest;
        std::size_t offset;
        TextureSubresources part;
        std::uintptr_t begin;
        std::uintptr_t end;
        bool (*encodeReadback)(const MetalTexture&, id<MTLCommandBuffer>, id<MTLBuffer>, std::size_t, TextureSubresources);
    };

    void encodeOverlapping(std::uintptr_t begin, std::uintptr_t end, const MetalTexture* keep) {
        if (entries.empty()) return;
        for (std::size_t i = 0; i < entries.size();) {
            auto& entry = entries[i];
            if (entry.texture.get() == keep || entry.end <= begin || end <= entry.begin) {
                ++i;
                continue;
            }
            auto taken = std::move(entry);
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            encode(batch, taken);
        }
    }

    void encode(id<MTLCommandBuffer> commands, Entry& entry) {
        ++encoded;
        if (!entry.encodeReadback(*entry.texture, commands, entry.guest, entry.offset, entry.part)) {
            throw MetalGpuExecutionError("Metal deferred copy-back is no longer in place");
        }
    }

    id<MTLCommandBuffer> batch = nil;
    std::vector<Entry> entries;
    std::uint64_t requested = 0;
    std::uint64_t encoded = 0;
};

}

#endif
