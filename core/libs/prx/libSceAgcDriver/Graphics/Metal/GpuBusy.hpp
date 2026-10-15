#pragma once

#include <algorithm>
#include <atomic>
#include <mutex>

// GPU busy time for the frame replay (tools/agc-replay): while enabled, every command buffer
// MetalDevice creates adds its GPU interval, and overlapping intervals count once. Off by default.
// Callers outside the driver library use the AgcCapture wrappers (Execution/Metal/AgcFrameCapture.hpp),
// so the state below stays in the one image that records it.
namespace AgcDriver::Metal::GpuBusy {

struct State {
    std::atomic<bool> enabled{false};
    std::mutex mutex;
    double busyMs = 0;
    double busyUntil = 0;
};

inline State& Shared() {
    static State state;
    return state;
}

inline bool Enabled() { return Shared().enabled.load(std::memory_order_relaxed); }

inline void Record(double gpuStart, double gpuEnd) {
    auto& state = Shared();
    if (!state.enabled.load(std::memory_order_relaxed) || gpuStart <= 0 || gpuEnd <= gpuStart) return;
    std::lock_guard lock(state.mutex);
    const auto from = std::max(gpuStart, state.busyUntil);
    if (gpuEnd > from) state.busyMs += (gpuEnd - from) * 1000.0;
    state.busyUntil = std::max(state.busyUntil, gpuEnd);
}

inline double Take() {
    auto& state = Shared();
    std::lock_guard lock(state.mutex);
    const auto value = state.busyMs;
    state.busyMs = 0;
    return value;
}

}
