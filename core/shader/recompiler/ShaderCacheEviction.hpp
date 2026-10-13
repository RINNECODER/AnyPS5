#ifndef CORE_SHADER_RECOMPILER_SHADERCACHEEVICTION_HPP
#define CORE_SHADER_RECOMPILER_SHADERCACHEEVICTION_HPP

// Size cap and least-recently-used eviction for the persistent shader caches under
// ShaderCacheDirectory(). Last use is the file's mtime: a hit refreshes it at most once an hour per
// file. Entries are written by write-to-temp + rename and read whole, so removing a file under a
// concurrent reader only turns that read into a miss.
//
// ANYPS5_SHADER_CACHE_MAX_MB: cap for the whole directory in MiB (default 1024, 0 = unlimited).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace ShaderRecompiler::CacheEviction {

inline constexpr std::uint64_t DefaultMaxMegabytes = 1024;
inline constexpr std::chrono::hours TouchInterval{1};
inline constexpr std::chrono::hours TemporaryGrace{1};

// Bytes allowed; 0 means unlimited.
inline std::uint64_t MaxBytes() {
    std::uint64_t megabytes = DefaultMaxMegabytes;
    const char* value = std::getenv("ANYPS5_SHADER_CACHE_MAX_MB");
    if (value != nullptr && *value != '\0') {
        char* end = nullptr;
        const auto parsed = std::strtoull(value, &end, 10);
        if (end != value && *end == '\0') megabytes = parsed;
    }
    return megabytes * 1024ull * 1024ull;
}

// Records a use of an entry: refreshes its mtime unless that is already within the interval.
inline void Touch(const std::filesystem::path& file, std::chrono::hours interval = TouchInterval) {
    std::error_code error;
    const auto used = std::filesystem::last_write_time(file, error);
    if (error) return;
    const auto now = std::filesystem::file_time_type::clock::now();
    if (now - used < interval) return;
    std::filesystem::last_write_time(file, now, error);
}

struct Entry {
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::filesystem::file_time_type used;
};

// Every cache entry below root. The last-used markers are not entries; a young temporary belongs
// to a live writer and is left alone, an old one to a dead writer and is evicted like any stale file.
inline std::vector<Entry> Scan(const std::filesystem::path& root) {
    std::vector<Entry> entries;
    std::error_code error;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (!it->is_regular_file(entryError) || entryError) continue;
        const auto name = it->path().filename().string();
        if (name == "last-used") continue;
        const auto size = it->file_size(entryError);
        const auto used = std::filesystem::last_write_time(it->path(), entryError);
        if (entryError) continue;
        if (name.find(".tmp.") != std::string::npos && now - used <= TemporaryGrace) continue;
        entries.push_back({it->path(), size, used});
    }
    return entries;
}

inline std::uint64_t TotalBytes(const std::vector<Entry>& entries) {
    std::uint64_t total = 0;
    for (const auto& entry : entries) total += entry.size;
    return total;
}

// When the entries exceed maxBytes, removes the least recently used until at most 90% of maxBytes
// remain. Returns the bytes left. maxBytes 0 is unlimited.
inline std::uint64_t Enforce(const std::filesystem::path& root, std::uint64_t maxBytes) {
    auto entries = Scan(root);
    auto total = TotalBytes(entries);
    if (maxBytes == 0 || total <= maxBytes) return total;
    const auto target = maxBytes / 10 * 9;
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const auto& entry : entries) {
        if (total <= target) break;
        std::error_code error;
        // Another process may have removed it already; the bytes are gone either way.
        std::filesystem::remove(entry.path, error);
        total -= std::min(total, entry.size);
    }
    return total;
}

// Removes the version directories of root that isVersionDirectory(name) accepts, other than keep,
// and that were not used within grace (a build that is running refreshes its marker). Returns how many.
template <class Predicate>
inline std::size_t RemoveStaleVersions(const std::filesystem::path& root, const std::filesystem::path& keep, Predicate isVersionDirectory,
                                       std::chrono::hours grace = TemporaryGrace) {
    std::size_t removed = 0;
    std::error_code error;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        std::error_code entryError;
        if (it->path() == keep || !it->is_directory(entryError) || !isVersionDirectory(it->path().filename().string())) continue;
        auto used = std::filesystem::last_write_time(it->path() / "last-used", entryError);
        if (entryError) {
            entryError.clear();
            used = std::filesystem::last_write_time(it->path(), entryError);
        }
        if (entryError || now - used < grace) continue;
        std::filesystem::remove_all(it->path(), entryError);
        if (!entryError) ++removed;
    }
    return removed;
}

// Marks a version directory as in use, for RemoveStaleVersions in other builds.
inline void MarkInUse(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto stamp = directory / "last-used";
    if (!std::filesystem::exists(stamp, error)) std::ofstream(stamp, std::ios::binary | std::ios::trunc).close();
    std::filesystem::last_write_time(stamp, std::filesystem::file_time_type::clock::now(), error);
}

// A process-wide byte estimate of one cache root, so a write does not rescan the tree: Startup (or
// the first write) scans and enforces; later writes add their bytes and enforce again past the cap.
class Budget {
public:
    void Startup(const std::filesystem::path& root) {
        const auto cap = MaxBytes();
        if (cap == 0) return;
        std::lock_guard lock(mutex);
        scanned = true;
        total = Enforce(root, cap);
    }
    void NoteWrite(const std::filesystem::path& root, std::uint64_t bytes) {
        const auto cap = MaxBytes();
        if (cap == 0) return;
        std::lock_guard lock(mutex);
        if (!scanned) {
            scanned = true;
            total = Enforce(root, cap);
        }
        total += bytes;
        if (total > cap) total = Enforce(root, cap);
    }

private:
    std::mutex mutex;
    bool scanned = false;
    std::uint64_t total = 0;
};

inline Budget& SharedBudget() {
    static Budget budget;
    return budget;
}

}

#endif
