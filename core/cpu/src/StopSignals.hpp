#pragma once
// Private to anyps5_cpu_run (Main.cpp): clean process-stop plumbing.
#include <cpu/Cpu.hpp>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cxxabi.h>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unistd.h>

namespace StopSignal {

inline std::atomic<int> received{0};
inline int wake[2]{-1, -1};

inline void OnSignal(int signal) {
    const int saved = errno;
    int none = 0;
    received.compare_exchange_strong(none, signal);
    const char byte = 1;
    static_cast<void>(write(wake[1], &byte, 1));
    errno = saved;
}

inline const char* Name(int signal) {
    return signal == SIGINT ? "SIGINT" : signal == SIGTERM ? "SIGTERM" : "signal";
}

// SIGINT/SIGTERM become a cooperative guest stop request, so the normal stop
// path runs (CPU stop, graphics drain, window close, JSON event) instead of
// the default kill. The handler is one-shot (SA_RESETHAND): a second signal
// takes the default action, so a stop that hangs can still be forced.
// Machine::Run clears stop requests made before it starts, so the watcher
// repeats the request until this guard is destroyed (before the machine).
class Guard {
public:
    explicit Guard(Cpu::Machine& target) : machine(target) {
        if (wake[0] != -1) throw std::logic_error("Stop signal guard is already installed");
        if (pipe(wake) != 0) throw std::runtime_error("Cannot create stop signal pipe");
        fcntl(wake[0], F_SETFD, FD_CLOEXEC);
        fcntl(wake[1], F_SETFD, FD_CLOEXEC);
        fcntl(wake[1], F_SETFL, O_NONBLOCK);
        received.store(0);
        try { watcher = std::thread([this] { Watch(); }); }
        catch (...) { Close(); throw; }
        struct sigaction action {};
        action.sa_handler = OnSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND;
        if (sigaction(SIGINT, &action, &previousInterrupt) != 0 ||
            sigaction(SIGTERM, &action, &previousTerminate) != 0) {
            Restore();
            Stop();
            throw std::runtime_error("Cannot install SIGINT/SIGTERM stop handlers");
        }
        installed = true;
    }
    ~Guard() {
        if (installed) Restore();
        Stop();
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    void Watch() {
        char byte = 0;
        while (read(wake[0], &byte, 1) < 0 && errno == EINTR) {}
        std::unique_lock lock(mutex);
        while (!quit && received.load() != 0) {
            try { machine.RequestStop(); } catch (...) {}
            changed.wait_for(lock, std::chrono::milliseconds(20), [&] { return quit; });
        }
    }
    void Restore() {
        sigaction(SIGINT, &previousInterrupt, nullptr);
        sigaction(SIGTERM, &previousTerminate, nullptr);
    }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            quit = true;
        }
        changed.notify_all();
        const char byte = 0;
        static_cast<void>(write(wake[1], &byte, 1));
        if (watcher.joinable()) watcher.join();
        Close();
    }
    static void Close() {
        for (auto& fd : wake) {
            if (fd != -1) close(fd);
            fd = -1;
        }
    }

    Cpu::Machine& machine;
    std::thread watcher;
    std::mutex mutex;
    std::condition_variable changed;
    bool quit = false, installed = false;
    struct sigaction previousInterrupt {}, previousTerminate {};
};

// Readable name of the exception in flight, for exceptions that are not a
// std::exception (for example ProcessShutdown). Call only inside a catch.
inline std::string CurrentExceptionType() {
    const auto* type = abi::__cxa_current_exception_type();
    if (!type) return "unknown exception";
    int status = 0;
    std::unique_ptr<char, decltype(&std::free)> name(abi::__cxa_demangle(type->name(), nullptr, nullptr, &status), &std::free);
    return status == 0 && name ? name.get() : type->name();
}

}
