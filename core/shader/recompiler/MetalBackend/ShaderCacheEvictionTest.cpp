// The shader cache size cap: least-recently-used eviction down to 90% of the cap, stale version
// directories removed, a concurrent reader tolerated, and the cap read from the environment.

#include "../ShaderCacheEviction.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
namespace Ev = ShaderRecompiler::CacheEviction;

namespace {

int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void Make(const fs::path& path, std::size_t size, std::chrono::minutes age) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
    fs::last_write_time(path, fs::file_time_type::clock::now() - age);
    if (path.parent_path() != path.parent_path().parent_path()) fs::last_write_time(path.parent_path(), fs::file_time_type::clock::now() - age);
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / ("anyps5-evict-" + std::to_string(::getpid()));
    fs::remove_all(root);

    // Cap respected, oldest go first, down to 90%.
    for (int index = 0; index < 10; ++index) Make(root / "v" / ("e" + std::to_string(index)), 100, std::chrono::minutes(60 * 10 - index));  // e0 oldest
    require(Ev::TotalBytes(Ev::Scan(root)) == 1000, "scan counts every entry");
    require(Ev::Enforce(root, 0) == 1000 && fs::exists(root / "v" / "e0"), "cap 0 is unlimited");
    require(Ev::Enforce(root, 1000) == 1000, "at the cap nothing is evicted");
    const auto left = Ev::Enforce(root, 950);  // over: target 855 -> keep 8 entries (800)
    require(left == 800, "evicts down to under 90% of the cap");
    require(!fs::exists(root / "v" / "e0") && !fs::exists(root / "v" / "e1") && fs::exists(root / "v" / "e2") && fs::exists(root / "v" / "e9"),
            "least recently used entries go first");

    // A hit refreshes recency: e2 (the oldest left) survives the next eviction after Touch.
    Ev::Touch(root / "v" / "e2");
    Ev::Enforce(root, 700);  // target 630 -> keep 6
    require(fs::exists(root / "v" / "e2") && !fs::exists(root / "v" / "e3") && !fs::exists(root / "v" / "e4"), "a touched entry outlives older untouched ones");
    // Touch is rate limited: a recent entry keeps its mtime.
    Make(root / "v" / "fresh", 10, std::chrono::minutes(5));
    const auto before = fs::last_write_time(root / "v" / "fresh");
    Ev::Touch(root / "v" / "fresh");
    require(fs::last_write_time(root / "v" / "fresh") == before, "touch is limited to once per interval");
    Ev::Touch(root / "v" / "missing");  // tolerated

    // Stale temporaries are evicted; young ones and markers are not counted.
    Make(root / "v" / "dead.tmp.1", 50, std::chrono::minutes(60 * 5));
    Make(root / "v" / "live.tmp.2", 50, std::chrono::minutes(0));
    Make(root / "v" / "last-used", 5, std::chrono::minutes(0));
    require(Ev::TotalBytes(Ev::Scan(root)) == 600 + 10 + 50, "young temporaries and markers are skipped");

    // Stale versions.
    fs::remove_all(root);
    Make(root / "0123456789abcdef" / "x", 10, std::chrono::minutes(60 * 48));
    Make(root / "fedcba9876543210" / "x", 10, std::chrono::minutes(60 * 48));  // current
    Make(root / "msl-old" / "x", 10, std::chrono::minutes(60 * 48));
    Make(root / "msl-new" / "x", 10, std::chrono::minutes(60 * 48));  // current
    Make(root / "00000000000000aa" / "x", 10, std::chrono::minutes(0));  // another build running now
    Make(root / "notes" / "x", 10, std::chrono::minutes(60 * 48));  // not a version directory
    const auto hexVersion = [](const std::string& name) { return name.size() == 16; };
    const auto mslVersion = [](const std::string& name) { return name.rfind("msl-", 0) == 0; };
    require(Ev::RemoveStaleVersions(root, root / "fedcba9876543210", hexVersion) == 1, "one old source version removed");
    require(Ev::RemoveStaleVersions(root, root / "msl-new", mslVersion) == 1, "one old MSL version removed");
    require(!fs::exists(root / "0123456789abcdef") && !fs::exists(root / "msl-old"), "old versions removed");
    require(fs::exists(root / "msl-new") && fs::exists(root / "fedcba9876543210") && fs::exists(root / "00000000000000aa") && fs::exists(root / "notes"), "current, recently used and unrelated directories stay");

    // A reader racing the eviction sees a hit or a clean miss, never a crash.
    fs::remove_all(root);
    for (int index = 0; index < 200; ++index) Make(root / "v" / ("r" + std::to_string(index)), 100, std::chrono::minutes(60 * 300 - index));
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0}, misses{0};
    std::thread reader([&] {
        while (!stop) {
            for (int index = 0; index < 200 && !stop; ++index) {
                std::ifstream file(root / "v" / ("r" + std::to_string(index)), std::ios::binary);
                ++reads;
                if (!file) { ++misses; continue; }
                std::string content((std::istreambuf_iterator<char>(file)), {});
                require(content.empty() || content.size() == 100, "a reader sees a whole entry or nothing");
                Ev::Touch(root / "v" / ("r" + std::to_string(index)));
            }
        }
    });
    for (int round = 0; round < 20; ++round) Ev::Enforce(root, 1000 + static_cast<std::uint64_t>(round));
    stop = true;
    reader.join();
    require(Ev::TotalBytes(Ev::Scan(root)) <= 900 && reads > 0, "eviction under a concurrent reader reaches the target");

    // The environment: default 1 GiB, MB override, 0 unlimited, junk ignored.
    ::unsetenv("ANYPS5_SHADER_CACHE_MAX_MB");
    require(Ev::MaxBytes() == 1024ull * 1024 * 1024, "default cap is 1 GiB");
    ::setenv("ANYPS5_SHADER_CACHE_MAX_MB", "64", 1);
    require(Ev::MaxBytes() == 64ull * 1024 * 1024, "the environment sets the cap in MiB");
    ::setenv("ANYPS5_SHADER_CACHE_MAX_MB", "0", 1);
    require(Ev::MaxBytes() == 0, "0 is unlimited");
    ::setenv("ANYPS5_SHADER_CACHE_MAX_MB", "lots", 1);
    require(Ev::MaxBytes() == 1024ull * 1024 * 1024, "junk falls back to the default");

    // The budget enforces on write once past the cap.
    fs::remove_all(root);
    ::setenv("ANYPS5_SHADER_CACHE_MAX_MB", "1", 1);
    Ev::Budget budget;
    for (int index = 0; index < 30; ++index) {
        Make(root / "v" / ("b" + std::to_string(index)), 100 * 1024, std::chrono::minutes(60 * 100 - index));
        budget.NoteWrite(root, 100 * 1024);
    }
    require(Ev::TotalBytes(Ev::Scan(root)) <= 1024 * 1024, "writes through the budget keep the directory under the cap");
    require(fs::exists(root / "v" / "b29"), "the newest write survives");

    fs::remove_all(root);
    if (failures == 0) std::puts("PASS");
    return failures == 0 ? 0 : 1;
}
