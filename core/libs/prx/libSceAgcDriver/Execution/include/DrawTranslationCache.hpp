#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver {

// Per-draw translation cache (the Metal draw path's counterpart of the Vulkan driver's DrawLookup).
// An entry holds what a draw derived from its inputs (the recompiled stages), under two checks:
//  - the key: every word of the draw's register-derived inputs (shader identity, user data, stage
//    and pixel info, draw parameters the recompile reads), compared exactly, not only by hash;
//  - the contents: the guest bytes the resource capture read (descriptors, SRT tables and every
//    other dword it followed), read again and compared on each lookup.
// A lookup hits only when both match, so a changed descriptor or constant misses. Each key keeps a
// few variants (most recently hit first), for draws whose descriptors alternate between frames.
template<typename Payload>
class DrawTranslationCache {
public:
    struct Region {
        std::uint64_t address;
        std::vector<std::byte> bytes;
    };
    // Reads guest bytes at `address` into `destination`; false when they cannot be read (unmapped).
    using Reader = bool (*)(std::uint64_t address, std::span<std::byte> destination);
    struct Counters {
        std::uint64_t lookups = 0, hits = 0, absent = 0, contentMisses = 0, inserts = 0, clears = 0;
    };

    explicit DrawTranslationCache(std::size_t maxKeys = 8192, std::size_t variantsPerKey = 4)
        : maxKeys(std::max<std::size_t>(1, maxKeys)), variantsPerKey(std::max<std::size_t>(1, variantsPerKey)) {}

    static std::uint64_t Hash(std::span<const std::uint64_t> key) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const auto word : key) {
            hash ^= word;
            hash *= 0x100000001b3ull;
            hash ^= hash >> 29u;
        }
        return hash;
    }

    // The payload of the variant whose key and guest contents both match now, or null.
    std::shared_ptr<const Payload> Find(std::span<const std::uint64_t> key, Reader read) {
        const auto hash = Hash(key);
        std::vector<std::shared_ptr<const Entry>> candidates;
        {
            std::lock_guard lock(mutex);
            ++counters.lookups;
            const auto found = entries.find(hash);
            if (found != entries.end()) candidates = found->second;
        }
        bool sameKey = false;
        for (std::size_t rank = 0; rank < candidates.size(); ++rank) {
            const auto& entry = *candidates[rank];
            if (!std::equal(entry.key.begin(), entry.key.end(), key.begin(), key.end())) continue;
            sameKey = true;
            if (!ContentsMatch(entry.regions, read)) continue;
            std::lock_guard lock(mutex);
            ++counters.hits;
            if (rank != 0) {
                const auto found = entries.find(hash);
                if (found != entries.end()) {
                    auto& variants = found->second;
                    const auto at = std::find(variants.begin(), variants.end(), candidates[rank]);
                    if (at != variants.end()) std::rotate(variants.begin(), at, at + 1);
                }
            }
            return entry.payload;
        }
        std::lock_guard lock(mutex);
        ++(sameKey ? counters.contentMisses : counters.absent);
        return nullptr;
    }

    // Stores `payload` for `key` with the guest bytes it was derived from (regions may be in any
    // order and adjacent; they are sorted and merged).
    void Insert(std::span<const std::uint64_t> key, std::vector<Region> regions, std::shared_ptr<const Payload> payload) {
        auto entry = std::make_shared<Entry>();
        entry->key.assign(key.begin(), key.end());
        entry->regions = Merge(std::move(regions));
        entry->payload = std::move(payload);
        const auto hash = Hash(key);
        std::lock_guard lock(mutex);
        ++counters.inserts;
        if (entries.size() >= maxKeys && entries.find(hash) == entries.end()) {
            entries.clear();
            ++counters.clears;
        }
        auto& variants = entries[hash];
        variants.insert(variants.begin(), std::move(entry));
        if (variants.size() > variantsPerKey) variants.resize(variantsPerKey);
    }

    void Clear() {
        std::lock_guard lock(mutex);
        entries.clear();
    }

    [[nodiscard]] Counters Snapshot() const {
        std::lock_guard lock(mutex);
        return counters;
    }

private:
    struct Entry {
        std::vector<std::uint64_t> key;
        std::vector<Region> regions;
        std::shared_ptr<const Payload> payload;
    };

    static std::vector<Region> Merge(std::vector<Region> regions) {
        std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.address < b.address; });
        std::vector<Region> merged;
        for (auto& region : regions) {
            if (region.bytes.empty()) continue;
            if (!merged.empty()) {
                auto& last = merged.back();
                const auto end = last.address + last.bytes.size();
                if (region.address <= end) {
                    // Overlapping reads of the same bytes: keep the union.
                    const auto skip = static_cast<std::size_t>(end - region.address);
                    if (skip < region.bytes.size()) last.bytes.insert(last.bytes.end(), region.bytes.begin() + static_cast<std::ptrdiff_t>(skip), region.bytes.end());
                    continue;
                }
            }
            merged.push_back(std::move(region));
        }
        return merged;
    }

    static bool ContentsMatch(const std::vector<Region>& regions, Reader read) {
        thread_local std::vector<std::byte> current;
        for (const auto& region : regions) {
            current.resize(region.bytes.size());
            if (!read(region.address, current)) return false;
            if (std::memcmp(current.data(), region.bytes.data(), region.bytes.size()) != 0) return false;
        }
        return true;
    }

    const std::size_t maxKeys;
    const std::size_t variantsPerKey;
    mutable std::mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<std::shared_ptr<const Entry>>> entries;
    Counters counters;
};

}
