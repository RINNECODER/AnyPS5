#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Opt-in binding trace for following one image through a frame (off by default).
// ANYPS5_METAL_BIND_TRACE=<first flip>[:<flips>] logs, for every draw and dispatch from
// that presented flip on (2 flips unless given), the colour/depth targets, the storage
// and sampled images (address, format, size, layers, samples, and whether the binding is a
// resident GPU surface or guest memory) and every skip, as "[bind] f<flip> ..." lines on
// stderr. Guest-memory images also report how many of 64 probed texels are non-zero, which
// shows a target that was rendered on the GPU but is sampled from stale guest memory.
namespace AgcDriver::Metal::BindTrace {

struct Settings {
    bool enabled = false;
    std::uint64_t first = 0;
    std::uint64_t count = 2;
};

inline Settings ParseSettings(const char* value) {
    Settings settings;
    if (value == nullptr || *value == '\0') return settings;
    char* end = nullptr;
    const auto first = std::strtoull(value, &end, 10);
    if (end == value || (*end != '\0' && *end != ':')) return settings;
    settings.first = first;
    if (*end == ':') {
        const char* countText = end + 1;
        const auto count = std::strtoull(countText, &end, 10);
        if (end == countText || *end != '\0' || count == 0) return settings;
        settings.count = count;
    }
    settings.enabled = true;
    return settings;
}

inline const Settings& Configured() {
    static const Settings settings = ParseSettings(std::getenv("ANYPS5_METAL_BIND_TRACE"));
    return settings;
}

inline std::atomic<std::uint64_t>& PresentedFlips() {
    static std::atomic<std::uint64_t> flips{0};
    return flips;
}

// Called once per presented flip.
inline void Flip() {
    if (Configured().enabled) PresentedFlips().fetch_add(1, std::memory_order_relaxed);
}

inline bool Active() {
    const auto& settings = Configured();
    if (!settings.enabled) return false;
    const auto flip = PresentedFlips().load(std::memory_order_relaxed);
    return flip >= settings.first && flip - settings.first < settings.count;
}

__attribute__((format(printf, 1, 2))) inline void Log(const char* format, ...) {
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    std::fprintf(stderr, "[bind] f%llu %s\n",
        static_cast<unsigned long long>(PresentedFlips().load(std::memory_order_relaxed)), text);
}

}
