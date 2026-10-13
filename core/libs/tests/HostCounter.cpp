#include "prx/libkernel/Time/include/HostCounter.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

static void Require(bool value, const char* name) {
    if (!value) {
        std::fprintf(stderr, "Host counter check failed: %s\n", name);
        std::abort();
    }
}

int main() {
    const auto frequency = HostCounter::Frequency();
    Require(frequency != 0, "frequency");
    const auto begin = std::chrono::steady_clock::now();
    const auto ticks = HostCounter::Read();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto elapsed = HostCounter::Read() - ticks;
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    const long double ratio = static_cast<long double>(elapsed) * 1000000000.0L /
        static_cast<long double>(frequency) / static_cast<long double>(nanos);
    Require(ratio > 0.95L && ratio < 1.05L, "frequency matches elapsed time");
    std::atomic<std::uint64_t> previous{HostCounter::Read()};
    std::atomic<unsigned> turn{0};
    std::array<std::thread, 4> workers;
    for (unsigned index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            for (unsigned iteration = index; iteration < 4096; iteration += 4) {
                while (turn.load(std::memory_order_acquire) != iteration) std::this_thread::yield();
                const auto value = HostCounter::Read();
                Require(value >= previous.load(std::memory_order_relaxed), "monotonic across threads");
                previous.store(value, std::memory_order_relaxed);
                turn.store(iteration + 1, std::memory_order_release);
            }
        });
    }
    for (auto& worker : workers) worker.join();
}
