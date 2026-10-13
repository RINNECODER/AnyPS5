#pragma once

#include "BindTrace.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

// Opt-in GPU queue profile (off by default). ANYPS5_METAL_GPU_TIMING=1 makes every CPU wait on
// a command buffer record, per kind of work (the scope the waiting thread is in), how many
// command buffers were waited on, the wall time spent waiting, and the GPU's own execution
// time (GPUEndTime - GPUStartTime). Every two seconds one JSON line goes to stderr:
//   {"event":"gpu_timing","window_ms":...,"dispatch":[n,wait_ms,gpu_ms],...}
// so a slow flip rate can be split into GPU execution and per-command-buffer round trips.
namespace AgcDriver::Metal::GpuTiming {

enum class Kind : std::uint8_t { Other, Dispatch, Draw, Readback, Upload, Count };

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_METAL_GPU_TIMING");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return enabled;
}

inline Kind& Current() {
    thread_local Kind kind = Kind::Other;
    return kind;
}

class Scope {
public:
    explicit Scope(Kind kind) : previous(Current()) { Current() = kind; }
    ~Scope() { Current() = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind previous;
};

struct Totals {
    std::uint64_t count = 0;
    double waitMs = 0;
    double gpuMs = 0;
};

struct State {
    std::mutex mutex;
    std::array<Totals, static_cast<std::size_t>(Kind::Count)> totals{};
    std::chrono::steady_clock::time_point windowStart = std::chrono::steady_clock::now();
    double busyMs = 0;
    double busyUntil = 0;
};

inline State& Shared() {
    static State state;
    return state;
}


// ANYPS5_METAL_GPU_TIMING=3: GPU time per command buffer, attributed to the work that created it
// (a dispatch's or draw's program address, an upload or copy-back with its byte size). Every
// four seconds the 16 most expensive tags go to stderr as one "gpu_top" JSON line.
inline bool ProfileTags() {
    static const bool on = [] {
        const char* value = std::getenv("ANYPS5_METAL_GPU_TIMING");
        return value != nullptr && value[0] == '3';
    }();
    return on;
}

// What the calling thread's next command buffers belong to: kind in the top 4 bits.
inline std::uint64_t& Tag() {
    thread_local std::uint64_t tag = 0;
    return tag;
}

class TagScope {
public:
    explicit TagScope(std::uint64_t tag) : previous(Tag()) { Tag() = tag; }
    ~TagScope() { Tag() = previous; }
    TagScope(const TagScope&) = delete;
    TagScope& operator=(const TagScope&) = delete;

private:
    std::uint64_t previous;
};

inline void RecordTag(std::uint64_t tag, double gpuSeconds) {
    struct Entry { std::uint64_t count = 0; double ms = 0; };
    static std::mutex mutex;
    static std::map<std::uint64_t, Entry> entries;
    static auto start = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex);
    auto& entry = entries[tag];
    ++entry.count;
    entry.ms += gpuSeconds > 0 ? gpuSeconds * 1000.0 : 0.0;
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::duration<double, std::milli>(now - start).count();
    if (window < 4000.0) return;
    std::vector<std::pair<std::uint64_t, Entry>> sorted(entries.begin(), entries.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double total = 0;
    for (const auto& item : sorted) total += item.second.ms;
    std::string line = "{\"schema_version\":1,\"event\":\"gpu_top\",\"window_ms\":" + std::to_string(static_cast<long>(window)) +
        ",\"total_ms\":" + std::to_string(static_cast<long>(total)) + ",\"top\":[";
    char item[96];
    for (std::size_t i = 0; i < sorted.size() && i < 16; ++i) {
        static constexpr const char* kinds[] = {"other", "dispatch", "draw", "readback", "upload"};
        const auto kind = static_cast<std::size_t>(sorted[i].first >> 60u);
        std::snprintf(item, sizeof item, "%s[\"%s\",\"0x%llx\",%llu,%.1f]", i ? "," : "", kind < 5 ? kinds[kind] : "?",
            static_cast<unsigned long long>(sorted[i].first & ((std::uint64_t{1} << 60u) - 1)),
            static_cast<unsigned long long>(sorted[i].second.count), sorted[i].second.ms);
        line += item;
    }
    std::fprintf(stderr, "%s]}\n", line.c_str());
    entries.clear();
    start = now;
}

inline std::uint64_t MakeTag(Kind kind, std::uint64_t value) {
    return (static_cast<std::uint64_t>(kind) << 60u) | (value & ((std::uint64_t{1} << 60u) - 1));
}

// One finished wait. `gpuSeconds` is the command buffer's GPUEndTime - GPUStartTime; with the
// command buffer's start and end (host seconds), the union of GPU intervals feeds "gpu_busy_ms"
// (command buffers committed without waiting overlap, so the per-kind sums can exceed it).
inline void Record(double waitMs, double gpuSeconds, double gpuStart = 0, double gpuEnd = 0);

// ANYPS5_METAL_GPU_TIMING=2 also puts the copy-backs of written images into command buffers of
// their own (enqueued right behind the work), so their GPU time shows as "readback".
inline bool SplitReadbacks() {
    static const bool split = [] {
        const char* value = std::getenv("ANYPS5_METAL_GPU_TIMING");
        return value != nullptr && value[0] == '2';
    }();
    return split;
}

inline void Record(double waitMs, double gpuSeconds, double gpuStart, double gpuEnd) {
    static constexpr const char* names[] = {"other", "dispatch", "draw", "readback", "upload"};
    if (BindTrace::Active()) BindTrace::Log("  gpu %s wait %.3f ms gpu %.3f ms", names[static_cast<std::size_t>(Current())], waitMs, gpuSeconds * 1000.0);
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    auto& totals = state.totals[static_cast<std::size_t>(Current())];
    ++totals.count;
    totals.waitMs += waitMs;
    totals.gpuMs += gpuSeconds > 0 ? gpuSeconds * 1000.0 : 0.0;
    if (gpuEnd > gpuStart && gpuStart > 0) {
        const auto from = std::max(gpuStart, state.busyUntil);
        if (gpuEnd > from) state.busyMs += (gpuEnd - from) * 1000.0;
        state.busyUntil = std::max(state.busyUntil, gpuEnd);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto window = std::chrono::duration<double, std::milli>(now - state.windowStart).count();
    if (window < 2000.0) return;
    char line[640];
    int used = std::snprintf(line, sizeof line, "{\"schema_version\":1,\"event\":\"gpu_timing\",\"window_ms\":%.0f", window);
    for (std::size_t i = 0; i < state.totals.size() && used > 0 && static_cast<std::size_t>(used) < sizeof line; ++i) {
        const auto& t = state.totals[i];
        used += std::snprintf(line + used, sizeof line - static_cast<std::size_t>(used), ",\"%s\":[%llu,%.1f,%.1f]", names[i],
            static_cast<unsigned long long>(t.count), t.waitMs, t.gpuMs);
    }
    std::fprintf(stderr, "%s,\"gpu_busy_ms\":%.1f}\n", line, state.busyMs);
    state.busyMs = 0;
    state.totals = {};
    state.windowStart = now;
}

}
