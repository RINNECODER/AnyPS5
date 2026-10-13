#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// Reports GPU work that does not complete (fork-only, always on, a few relaxed stores per draw,
// dispatch and wait). A command buffer that never completes leaves the queue worker blocked in
// waitUntilCompleted with every guest thread waiting behind it, and nothing in the log says which
// work it was. The packet executor notes the draw or dispatch it runs (Note); a watcher thread,
// started by the first wait, prints that work once one wait has lasted 10, 30, 60, ... seconds.
// ANYPS5_METAL_STALL_DUMP=<file> also writes the stuck dispatch's Metal source there (debug aid).
namespace AgcDriver::Metal::GpuStallWatch {

struct Work {
    std::atomic<std::uint64_t> program{0};
    std::atomic<std::uint32_t> kind{0};  // 0 other, 1 dispatch, 2 draw
    std::atomic<std::uint64_t> size{0};  // dispatch groups (x | y << 21 | z << 42) or draw count
    std::atomic<std::int64_t> waitStartedMs{0};
    std::mutex sourceMutex;
    const std::string* source = nullptr;  // set by SourceScope
    std::string detail;                   // SetDetail: what the stuck work read (dump only)
};

inline Work& Current() {
    static Work work;
    return work;
}

inline const char* DumpPath() {
    static const char* path = std::getenv("ANYPS5_METAL_STALL_DUMP");
    return path != nullptr && *path != '\0' ? path : nullptr;
}

inline std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void Note(std::uint32_t kind, std::uint64_t program, std::uint64_t size) {
    auto& work = Current();
    work.kind.store(kind, std::memory_order_relaxed);
    work.program.store(program, std::memory_order_relaxed);
    work.size.store(size, std::memory_order_relaxed);
}

// Text written after the source in the dump (bindings and their first words); kept until the
// enclosing SourceScope ends.
inline void SetDetail(std::string detail) {
    if (DumpPath() == nullptr) return;
    auto& work = Current();
    std::lock_guard lock(work.sourceMutex);
    work.detail = std::move(detail);
}

// Attaches the Metal source of the noted work for the dump; SourceScope detaches it before the
// source goes away.
class SourceScope {
public:
    explicit SourceScope(const std::string& source) { set(&source); }
    ~SourceScope() {
        set(nullptr);
        SetDetail({});
    }
    SourceScope(const SourceScope&) = delete;
    SourceScope& operator=(const SourceScope&) = delete;

private:
    static void set(const std::string* source) {
        if (DumpPath() == nullptr) return;
        auto& work = Current();
        std::lock_guard lock(work.sourceMutex);
        work.source = source;
    }
};

// Seconds after which a still-running wait is reported: 10, 30, 60, then every 60.
constexpr std::int64_t NextReport(std::int64_t reportedSeconds) {
    return reportedSeconds < 10 ? 10 : reportedSeconds < 30 ? 30 : reportedSeconds < 60 ? 60 : reportedSeconds + 60;
}

inline void Report(Work& work, std::int64_t seconds, bool first) {
    const auto kind = work.kind.load(std::memory_order_relaxed);
    const auto size = work.size.load(std::memory_order_relaxed);
    const auto program = static_cast<unsigned long long>(work.program.load(std::memory_order_relaxed));
    if (kind == 1) {
        std::fprintf(stderr, "[metal] GPU work still running after %llds: dispatch cs 0x%llx groups %llux%llux%llu\n",
            static_cast<long long>(seconds), program, static_cast<unsigned long long>(size & 0x1fffff),
            static_cast<unsigned long long>((size >> 21) & 0x1fffff), static_cast<unsigned long long>(size >> 42));
    } else if (kind == 2) {
        std::fprintf(stderr, "[metal] GPU work still running after %llds: draw vs 0x%llx count %llu\n",
            static_cast<long long>(seconds), program, static_cast<unsigned long long>(size));
    } else {
        std::fprintf(stderr, "[metal] GPU work still running after %llds (not a draw or dispatch)\n", static_cast<long long>(seconds));
    }
    if (!first || DumpPath() == nullptr) return;
    std::lock_guard lock(work.sourceMutex);
    if (work.source == nullptr || work.waitStartedMs.load(std::memory_order_relaxed) == 0) return;
    if (std::FILE* file = std::fopen(DumpPath(), "wb")) {
        std::fwrite(work.source->data(), 1, work.source->size(), file);
        std::fwrite(work.detail.data(), 1, work.detail.size(), file);
        std::fclose(file);
        std::fprintf(stderr, "[metal] wrote the stuck work's Metal source to %s\n", DumpPath());
    }
}

inline void Watch() {
    std::int64_t started = 0;
    std::int64_t reported = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto& work = Current();
        const auto now = work.waitStartedMs.load(std::memory_order_relaxed);
        if (now != started) {
            started = now;
            reported = 0;
        }
        if (started == 0) continue;
        const auto seconds = (NowMs() - started) / 1000;
        if (seconds < NextReport(reported)) continue;
        Report(work, seconds, reported == 0);
        reported = seconds;
    }
}

// Brackets one blocking wait for a command buffer.
class Scope {
public:
    Scope() {
        static std::once_flag started;
        std::call_once(started, [] { std::thread(Watch).detach(); });
        Current().waitStartedMs.store(NowMs(), std::memory_order_relaxed);
    }
    ~Scope() {
        auto& work = Current();
        if (DumpPath() != nullptr) {
            std::lock_guard lock(work.sourceMutex);
            work.waitStartedMs.store(0, std::memory_order_relaxed);
        } else {
            work.waitStartedMs.store(0, std::memory_order_relaxed);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

}
