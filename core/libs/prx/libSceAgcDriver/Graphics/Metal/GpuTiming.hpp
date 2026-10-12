#pragma once

#include "BindTrace.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

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
