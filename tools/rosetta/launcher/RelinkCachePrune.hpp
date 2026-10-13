// Keeps the relink cache (<root>/<eboot sha256>/...) from growing without bound when a game updates:
// at most `keep` most recently used title entries stay (ANYPS5_ROSETTA_CACHE_KEEP, default 2).
// Recency is the entry directory's mtime, which the launcher refreshes on every launch. The entry in
// use and any entry whose <sha>.lock or <sha>.inuse is held by a live launcher are never removed.
#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace Aps5Launcher {

inline constexpr int kDefaultRelinkCacheKeep = 2;

// ANYPS5_ROSETTA_CACHE_KEEP: how many title entries to keep; 0 or less disables pruning.
inline int RelinkCacheKeep() {
    const char* value = std::getenv("ANYPS5_ROSETTA_CACHE_KEEP");
    if (value == nullptr || *value == '\0') return kDefaultRelinkCacheKeep;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value && *end == '\0' ? static_cast<int>(std::max(parsed, 0L)) : kDefaultRelinkCacheKeep;
}

inline bool IsTitleEntryName(const std::string& name) {
    return name.size() == 64 && name.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// A shared lock on <root>/<sha>.inuse for the life of the launcher marks the entry as running.
// Returns the descriptor (leave it open), or -1.
inline int MarkRelinkEntryInUse(const std::filesystem::path& root, const std::string& sha) {
    const int fd = ::open((root / (sha + ".inuse")).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd >= 0 && ::flock(fd, LOCK_SH | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Refreshes the entry's recency.
inline void TouchRelinkEntry(const std::filesystem::path& root, const std::string& sha) {
    ::utimensat(AT_FDCWD, (root / sha).c_str(), nullptr, 0);
}

namespace detail {
// Holds an exclusive non-blocking flock on a file next to the entry, or none.
struct EntryLock {
    int fd = -1;
    bool held = false;
    EntryLock(const std::filesystem::path& file) {
        fd = ::open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        held = fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    }
    ~EntryLock() {
        if (fd >= 0) ::close(fd);
    }
    EntryLock(const EntryLock&) = delete;
    EntryLock& operator=(const EntryLock&) = delete;
};
}  // namespace detail

// Removes all but the `keep` most recently used title entries of root, never `inUse` nor an entry
// locked by another launcher. Returns the removed entry names.
inline std::vector<std::string> PruneRelinkCache(const std::filesystem::path& root, int keep, const std::string& inUse) {
    std::vector<std::string> removed;
    if (keep <= 0) return removed;
    struct Candidate {
        std::string name;
        std::filesystem::file_time_type used;
    };
    std::vector<Candidate> entries;
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        std::error_code entryError;
        const std::string name = it->path().filename().string();
        if (!IsTitleEntryName(name) || !it->is_directory(entryError)) continue;
        const auto used = std::filesystem::last_write_time(it->path(), entryError);
        if (!entryError) entries.push_back({name, used});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Candidate& a, const Candidate& b) { return a.used > b.used; });
    int kept = 0;
    for (const auto& entry : entries) {
        // The entry in use always stays, and counts toward the keep.
        if (kept < keep || entry.name == inUse) {
            ++kept;
            continue;
        }
        detail::EntryLock preparing(root / (entry.name + ".lock"));
        detail::EntryLock running(root / (entry.name + ".inuse"));
        if (!preparing.held || !running.held) {
            ++kept;
            continue;
        }
        std::error_code removeError;
        std::filesystem::remove_all(root / entry.name, removeError);
        for (const char* suffix : {".relink-failed.log"}) std::filesystem::remove(root / (entry.name + suffix), removeError);
        std::filesystem::remove(root / (entry.name + ".lock"), removeError);
        std::filesystem::remove(root / (entry.name + ".inuse"), removeError);
        removed.push_back(entry.name);
    }
    return removed;
}

}  // namespace Aps5Launcher
