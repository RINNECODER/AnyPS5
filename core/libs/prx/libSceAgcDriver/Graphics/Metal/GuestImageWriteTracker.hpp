#pragma once

#include "GuestImageCache.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace AgcDriver::Metal {

// GuestImageCache's write tracker for the Metal driver, over libc's page write watch
// (GuestWriteWatch.hpp: the title's direct-memory mappings are registered when mapped; a collect
// reports the pages written since that page was last collected and write-protects them again, and
// a store to a protected page takes one fault that marks it written). The Metal build has no
// GuestMemory write tracker (that one is the Vulkan driver's), so this keeps the stamps itself:
// every collect takes a new generation and stamps the 64 KiB blocks of the pages it found written,
// and a range is unchanged since generation G while no block of it carries a newer stamp. The
// stamps make collects of overlapping ranges safe: a page one surface's collect takes is still
// seen by every other surface on it. Host addresses: the watch covers the process's own mappings.
class GuestImageWriteStamps {
public:
    static GuestImageWriteStamps& Shared() {
        static GuestImageWriteStamps stamps;
        return stamps;
    }

    static bool Available() { return GuestWriteWatch::GuestWriteWatchAvailable_nid_postfix(); }

    // 0 when the range is not (wholly) watched.
    std::uint64_t Collect(const std::byte* host, std::size_t bytes) {
        const auto begin = reinterpret_cast<std::uintptr_t>(host);
        if (bytes == 0 || begin + bytes < begin || begin + bytes > Limit) return 0;
        std::lock_guard lock(mutex);
        const auto stamp = ++generation;
        Pass pass{*this, stamp};
        if (!GuestWriteWatch::GuestWriteWatchCollect_nid_postfix(begin, bytes, &written, &pass) || pass.failed) return 0;
        return stamp;
    }

    // Whether no collect since `stamp` found a written page in [host, host + bytes).
    bool Unchanged(const std::byte* host, std::size_t bytes, std::uint64_t stamp) {
        const auto begin = reinterpret_cast<std::uintptr_t>(host);
        if (stamp == 0 || bytes == 0 || begin + bytes < begin || begin + bytes > Limit) return false;
        std::lock_guard lock(mutex);
        for (auto block = begin / BlockBytes; block <= (begin + bytes - 1) / BlockBytes; ++block) {
            const auto& leaf = leaves[block / LeafBlocks];
            if (leaf != nullptr && (*leaf)[block % LeafBlocks] > stamp) return false;
        }
        return true;
    }

private:
    static constexpr std::uintptr_t BlockBytes = 64 * 1024;
    static constexpr std::size_t LeafBlocks = std::size_t{1} << 16;
    static constexpr std::size_t LeafCount = std::size_t{1} << 15;
    static constexpr std::uintptr_t Limit = std::uintptr_t{LeafCount} * LeafBlocks * BlockBytes;
    using Leaf = std::array<std::uint64_t, LeafBlocks>;

    struct Pass {
        GuestImageWriteStamps& stamps;
        std::uint64_t stamp;
        bool failed = false;
    };

    // Runs outside the watch's lock, inside ours.
    static void written(void* context, std::uintptr_t begin, std::uintptr_t end) {
        auto& pass = *static_cast<Pass*>(context);
        if (end <= begin || end > Limit) {
            pass.failed = true;
            return;
        }
        for (auto block = begin / BlockBytes; block <= (end - 1) / BlockBytes; ++block) {
            auto& leaf = pass.stamps.leaves[block / LeafBlocks];
            if (leaf == nullptr) leaf = std::make_unique<Leaf>();
            (*leaf)[block % LeafBlocks] = pass.stamp;
        }
    }

    std::mutex mutex;
    std::uint64_t generation = 0;
    std::array<std::unique_ptr<Leaf>, LeafCount> leaves;
};

inline GuestImageWriteTracker WatchedImageWriteTracker() {
    return {
        [](const std::byte* host, std::size_t bytes) { return GuestImageWriteStamps::Shared().Collect(host, bytes); },
        [](const std::byte* host, std::size_t bytes, std::uint64_t generation) {
            return GuestImageWriteStamps::Shared().Unchanged(host, bytes, generation);
        },
    };
}

}
