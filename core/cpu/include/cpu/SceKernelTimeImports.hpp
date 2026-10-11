#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;
class GuestThreads;

// libkernel time services: process time and its counter, real-time, monotonic and
// CPU-time clocks, gettimeofday, UTC conversion and sleeps, with their POSIX
// aliases (exported by the libkernel module in both its libkernel and libScePosix
// libraries). The console clock is kept in UTC, as in core/libs libkernel Time.
//
// With a guest scheduler a sleep parks only the calling guest thread: the others
// keep running, the scheduler's owner pump wakes the sleeper at its deadline, and a
// stop request or shutdown cancels it like any other blocked call. Without one (a
// single-image run) the host sleeps in short slices and gives up early once
// stopRequested reports true.
class SceKernelTimeImports {
public:
    SceKernelTimeImports(Machine& machine, std::shared_ptr<GuestThreads> threads,
                         std::function<bool()> stopRequested = {},
                         std::uint64_t gateBase = 0x7ffdee000000);
    ~SceKernelTimeImports();
    SceKernelTimeImports(const SceKernelTimeImports&) = delete;
    SceKernelTimeImports& operator=(const SceKernelTimeImports&) = delete;
    // nullopt for every NID this provider does not implement. A known NID in the
    // wrong module/library scope or version is rejected.
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
