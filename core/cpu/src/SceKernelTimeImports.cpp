#include <cpu/SceKernelTimeImports.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <sys/resource.h>
#include <time.h>

namespace Cpu {
namespace {

enum class Service {
    Usleep, Nanosleep, Sleep, ProcessTime, ProcessTimeCounter, ProcessTimeCounterFrequency,
    Gettimeofday, Gettimezone, ClockGettime, ClockGetres, UtcToLocaltime, LocaltimeToUtc,
    PosixUsleep, PosixNanosleep, PosixSleep, PosixGettimeofday, PosixClockGettime, PosixClockGetres
};

struct Row { const char* Name; Service Operation; };

// sce* rows are exported by the libkernel library only; the POSIX rows by both the
// libkernel and libScePosix libraries of the libkernel module.
constexpr std::array<Row, 12> kernelRows{{
    {"sceKernelUsleep", Service::Usleep}, {"sceKernelNanosleep", Service::Nanosleep},
    {"sceKernelSleep", Service::Sleep}, {"sceKernelGetProcessTime", Service::ProcessTime},
    {"sceKernelGetProcessTimeCounter", Service::ProcessTimeCounter},
    {"sceKernelGetProcessTimeCounterFrequency", Service::ProcessTimeCounterFrequency},
    {"sceKernelGettimeofday", Service::Gettimeofday}, {"sceKernelGettimezone", Service::Gettimezone},
    {"sceKernelClockGettime", Service::ClockGettime}, {"sceKernelClockGetres", Service::ClockGetres},
    {"sceKernelConvertUtcToLocaltime", Service::UtcToLocaltime},
    {"sceKernelConvertLocaltimeToUtc", Service::LocaltimeToUtc}}};
constexpr std::array<Row, 7> posixRows{{
    {"usleep", Service::PosixUsleep}, {"nanosleep", Service::PosixNanosleep},
    {"_nanosleep", Service::PosixNanosleep}, {"sleep", Service::PosixSleep},
    {"gettimeofday", Service::PosixGettimeofday}, {"clock_gettime", Service::PosixClockGettime},
    {"clock_getres", Service::PosixClockGetres}}};

// FreeBSD errno values; the sce* entry points return them as 0x8002xxxx.
constexpr int Eintr = 4, Efault = 14, Einval = 22;
constexpr std::int64_t sceError(int error) { return static_cast<std::int32_t>(0x80020000u | static_cast<std::uint32_t>(error)); }

constexpr std::uint64_t NanosPerSecond = 1000000000ull;
// Host slice for a sleep without a guest scheduler, so a stop request is seen promptly.
constexpr auto HostSleepSlice = std::chrono::milliseconds(5);
// Every consumer of one NID in one library shares a trampoline, so this bounds the
// distinct (NID, library) pairs; all share a single engine host gate.
constexpr std::size_t TrampolineCapacity = 64;

// PS4/PS5 (FreeBSD) clock ids.
enum class Clock { Realtime, Second, Monotonic, UserCpu, ProfCpu, ThreadCpu, ProcessCpu };
std::optional<Clock> clockFor(std::int32_t id) {
    switch (id) {
    case 0: case 9: case 10: return Clock::Realtime;            // REALTIME, _PRECISE, _FAST
    case 13: return Clock::Second;                               // SECOND
    case 4: case 5: case 7: case 8: case 11: case 12: return Clock::Monotonic; // MONOTONIC*, UPTIME*
    case 1: return Clock::UserCpu;                               // VIRTUAL
    case 2: return Clock::ProfCpu;                               // PROF
    case 14: return Clock::ThreadCpu;                            // THREAD_CPUTIME_ID
    case 15: return Clock::ProcessCpu;                           // PROCESS_CPUTIME_ID
    default: return std::nullopt;
    }
}

std::uint64_t hostClock(clockid_t id, bool resolution) {
    timespec value{};
    if ((resolution ? clock_getres(id, &value) : clock_gettime(id, &value)) != 0)
        throw std::runtime_error("Host clock is unavailable for a libkernel time service");
    return static_cast<std::uint64_t>(value.tv_sec) * NanosPerSecond + static_cast<std::uint64_t>(value.tv_nsec);
}

std::uint64_t steadyNanos() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::uint64_t cpuNanos(bool includeSystem) {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("Host process CPU time is unavailable");
    const auto micros = [](const timeval& value) {
        return static_cast<std::uint64_t>(value.tv_sec) * 1000000ull + static_cast<std::uint64_t>(value.tv_usec);
    };
    return (micros(usage.ru_utime) + (includeSystem ? micros(usage.ru_stime) : 0)) * 1000ull;
}

// Guest threads all run on the CPU owner thread, so the CPU-time clocks report host
// CPU time of the owner thread and process.
std::uint64_t clockNanos(Clock clock) {
    switch (clock) {
    case Clock::Realtime: return hostClock(CLOCK_REALTIME, false);
    case Clock::Second: return hostClock(CLOCK_REALTIME, false) / NanosPerSecond * NanosPerSecond;
    case Clock::Monotonic: return steadyNanos();
    case Clock::UserCpu: return cpuNanos(false);
    case Clock::ProfCpu: return cpuNanos(true);
    case Clock::ThreadCpu: return hostClock(CLOCK_THREAD_CPUTIME_ID, false);
    case Clock::ProcessCpu: return hostClock(CLOCK_PROCESS_CPUTIME_ID, false);
    }
    throw std::logic_error("Unknown libkernel clock");
}

std::uint64_t clockResolution(Clock clock) {
    switch (clock) {
    case Clock::Realtime: return hostClock(CLOCK_REALTIME, true);
    case Clock::Second: return NanosPerSecond;
    case Clock::Monotonic:
        return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::duration(1)).count()));
    case Clock::UserCpu: case Clock::ProfCpu: return 1000;   // getrusage microseconds
    case Clock::ThreadCpu: return hostClock(CLOCK_THREAD_CPUTIME_ID, true);
    case Clock::ProcessCpu: return hostClock(CLOCK_PROCESS_CPUTIME_ID, true);
    }
    throw std::logic_error("Unknown libkernel clock");
}

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

}

struct SceKernelTimeImports::Impl : std::enable_shared_from_this<SceKernelTimeImports::Impl> {
    using Key = std::pair<std::string, std::string>;
    struct Sleeper { GuestThreadHandle Thread; std::chrono::steady_clock::time_point Deadline; std::uint64_t Remaining; };
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    std::function<bool()> stopRequested;
    std::optional<GuestThreads::WaitDomain> waits;
    SceHostTrampolines trampolines;
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::map<std::string, std::pair<Service, bool>> services;
    std::map<Key, std::uint64_t> gates;
    std::map<std::uint64_t, Sleeper> sleepers;
    std::uint64_t nextKey = 1;

    static std::uint64_t checkedBase(Machine& guest, std::uint64_t base) {
        for (const auto& mapping : guest.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("SCE kernel time gate page is already mapped");
        return base;
    }

    Impl(Machine& guest, const std::shared_ptr<GuestThreads>& scheduler, std::function<bool()> stop, std::uint64_t gateBase)
        : machine(guest), threads(scheduler), stopRequested(std::move(stop)),
          trampolines(guest, checkedBase(guest, gateBase), TrampolineCapacity) {
        for (const auto& row : kernelRows) services.emplace(Nid::ComputeNid(row.Name, "libkernel"), std::pair{row.Operation, false});
        for (const auto& row : posixRows) services.emplace(Nid::ComputeNid(row.Name, "libkernel"), std::pair{row.Operation, true});
    }

    bool accessible(std::uint64_t address, std::size_t size, Permission permission) const {
        if (!address || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
        try { machine.CheckAccess(address, size, permission); return true; }
        catch (const std::runtime_error&) { return false; }
    }
    template<class T> void store(std::uint64_t address, const T& value) {
        machine.Write(address, std::as_bytes(std::span(&value, 1)));
    }
    template<class T> T load(std::uint64_t address) const {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    // Writes a {seconds, fraction} pair, the layout of both timespec and timeval.
    void storePair(std::uint64_t address, std::int64_t seconds, std::int64_t fraction) {
        store(address, std::array<std::int64_t, 2>{seconds, fraction});
    }

    // POSIX convention: -1 with the calling guest thread's errno set. Single-image runs
    // have no guest thread runtime and so no errno storage; they get -1 alone.
    std::int64_t posixError(int error) {
        if (const auto scheduler = threads.lock()) {
            const auto address = scheduler->ActiveErrnoAddress();
            if (accessible(address, 4, Permission::Write)) store(address, static_cast<std::int32_t>(error));
        }
        return -1;
    }

    std::int64_t clockGet(std::int32_t id, std::uint64_t output, bool resolution, bool posix) {
        const auto fail = [&](int error) { return posix ? posixError(error) : sceError(error); };
        const auto clock = clockFor(id);
        if (!clock) return fail(Einval);
        if (!accessible(output, 16, Permission::Write)) return fail(Efault);
        const auto nanos = resolution ? clockResolution(*clock) : clockNanos(*clock);
        storePair(output, static_cast<std::int64_t>(nanos / NanosPerSecond), static_cast<std::int64_t>(nanos % NanosPerSecond));
        return 0;
    }

    std::int64_t timeOfDay(std::uint64_t tv, std::uint64_t tz, bool posix) {
        const auto fail = [&](int error) { return posix ? posixError(error) : sceError(error); };
        // FreeBSD gettimeofday accepts a null tv; sceKernelGettimeofday does not.
        if ((tv || !posix) && !accessible(tv, 16, Permission::Write)) return fail(Efault);
        if (tz && !accessible(tz, 8, Permission::Write)) return fail(Efault);
        if (tv) {
            const auto micros = hostClock(CLOCK_REALTIME, false) / 1000;
            storePair(tv, static_cast<std::int64_t>(micros / 1000000), static_cast<std::int64_t>(micros % 1000000));
        }
        if (tz) store(tz, std::array<std::int32_t, 2>{0, 0});
        return 0;
    }

    // Parks the calling guest thread until its deadline; returns the provisional RAX
    // (the scheduler supplies the real one), or the interrupted status of a host sleep
    // that a stop request ended early: fail(EINTR), with the unslept time in *remaining.
    template<class Fail> std::int64_t sleep(std::uint64_t nanos, std::uint64_t remaining, Fail fail) {
        const auto scheduler = threads.lock();
        if (!nanos) {
            // A zero sleep still gives other runnable guest threads a turn.
            if (scheduler) { machine.Set(Register::Rax, 0); scheduler->YieldFromHostCall(); }
            if (remaining) storePair(remaining, 0, 0);
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto room = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::time_point::max() - now).count());
        const auto deadline = nanos >= room ? std::chrono::steady_clock::time_point::max()
            : now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(nanos));
        if (!scheduler) {
            for (auto current = now; current < deadline; current = std::chrono::steady_clock::now()) {
                if (stopRequested && stopRequested()) {
                    const auto left = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - current).count());
                    if (remaining) storePair(remaining, static_cast<std::int64_t>(left / NanosPerSecond),
                                             static_cast<std::int64_t>(left % NanosPerSecond));
                    return fail(Eintr);
                }
                std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(deadline - current, HostSleepSlice));
            }
            if (remaining) storePair(remaining, 0, 0);
            return 0;
        }
        if (!waits) throw std::logic_error("SCE kernel time sleep has no guest wait domain");
        if (nextKey == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("SCE kernel time sleep identity exhausted");
        const auto key = nextKey++;
        sleepers.emplace(key, Sleeper{waits->ActiveThread(), deadline, remaining});
        try { waits->BlockFromHostCall(key); }
        catch (...) { sleepers.erase(key); throw; }
        return 0; // the scheduler supplies the real return when the sleep completes
    }

    std::int64_t nanosleep(std::uint64_t request, std::uint64_t remaining, bool posix) {
        const auto fail = [&](int error) { return posix ? posixError(error) : sceError(error); };
        if (!accessible(request, 16, Permission::Read)) return fail(Efault);
        if (remaining && !accessible(remaining, 16, Permission::Write)) return fail(Efault);
        const auto [seconds, nanos] = load<std::array<std::int64_t, 2>>(request);
        if (nanos < 0 || nanos >= static_cast<std::int64_t>(NanosPerSecond)) return fail(Einval);
        // FreeBSD kern_nanosleep: a negative interval has already elapsed.
        if (seconds < 0) {
            if (remaining) storePair(remaining, 0, 0);
            return 0;
        }
        const auto total = static_cast<std::uint64_t>(seconds) > (std::numeric_limits<std::uint64_t>::max() - NanosPerSecond) / NanosPerSecond
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(seconds) * NanosPerSecond + static_cast<std::uint64_t>(nanos);
        return sleep(total, remaining, fail);
    }

    std::int64_t invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        const auto second = guest.Get(Register::Rsi);
        const auto third = guest.Get(Register::Rdx);
        const auto fourth = guest.Get(Register::Rcx);
        const auto clockId = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(first));
        const auto sce = [](int error) { return sceError(error); };
        const auto posix = [this](int error) { return posixError(error); };
        const auto useconds = static_cast<std::uint64_t>(static_cast<std::uint32_t>(first)) * 1000ull;
        const auto seconds = static_cast<std::uint64_t>(static_cast<std::uint32_t>(first)) * NanosPerSecond;
        switch (service) {
        case Service::Usleep: return sleep(useconds, 0, sce);
        case Service::PosixUsleep: return sleep(useconds, 0, posix);
        case Service::Sleep: return sleep(seconds, 0, sce);
        // POSIX sleep returns the unslept seconds: the whole request when a stop interrupts it.
        case Service::PosixSleep: return sleep(seconds, 0, [&](int) { return static_cast<std::int64_t>(first & 0xffffffffu); });
        case Service::Nanosleep: return nanosleep(first, second, false);
        case Service::PosixNanosleep: return nanosleep(first, second, true);
        case Service::ProcessTime:
            return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
        case Service::ProcessTimeCounter:
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        case Service::ProcessTimeCounterFrequency: return static_cast<std::int64_t>(NanosPerSecond);
        case Service::Gettimeofday: return timeOfDay(first, 0, false);
        case Service::PosixGettimeofday: return timeOfDay(first, second, true);
        case Service::Gettimezone:
            if (!accessible(first, 8, Permission::Write)) return sceError(Efault);
            store(first, std::array<std::int32_t, 2>{0, 0});
            return 0;
        case Service::ClockGettime: return clockGet(clockId, second, false, false);
        case Service::ClockGetres: return clockGet(clockId, second, true, false);
        case Service::PosixClockGettime: return clockGet(clockId, second, false, true);
        case Service::PosixClockGetres: return clockGet(clockId, second, true, true);
        case Service::UtcToLocaltime: {
            // (time_t utc, time_t* local, {time_t, u32 west, u32 dst}* timesec, u64* dst_sec)
            if ((second && !accessible(second, 8, Permission::Write)) ||
                (third && !accessible(third, 16, Permission::Write)) ||
                (fourth && !accessible(fourth, 8, Permission::Write))) return sceError(Efault);
            const auto utc = std::bit_cast<std::int64_t>(first);
            if (second) store(second, utc);
            if (third) { store(third, utc); store(third + 8, std::array<std::uint32_t, 2>{0, 0}); }
            if (fourth) store(fourth, std::uint64_t{0});
            return 0;
        }
        case Service::LocaltimeToUtc: {
            // (time_t local, reserved, time_t* utc, timezone* tz, int* dst_sec)
            const auto dst = guest.Get(Register::R8);
            if ((third && !accessible(third, 8, Permission::Write)) ||
                (fourth && !accessible(fourth, 8, Permission::Write)) ||
                (dst && !accessible(dst, 4, Permission::Write))) return sceError(Efault);
            if (third) store(third, std::bit_cast<std::int64_t>(first));
            if (fourth) store(fourth, std::array<std::int32_t, 2>{0, 0});
            if (dst) store(dst, std::int32_t{0});
            return 0;
        }
        }
        throw std::logic_error("Unknown SCE kernel time operation");
    }

    // Owner pump: wakes every sleeper whose deadline has passed. True while any
    // sleeper is still waiting, so an otherwise idle scheduler keeps turning.
    bool pump() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = sleepers.begin(); it != sleepers.end();) {
            const auto& sleeper = it->second;
            if (!waits->IsWaiting(sleeper.Thread, it->first)) { it = sleepers.erase(it); continue; }
            if (sleeper.Deadline > now) { ++it; continue; }
            const auto remaining = sleeper.Remaining;
            const auto completion = [state = weak_from_this(), remaining]() -> std::uint32_t {
                const auto context = state.lock();
                if (!context) throw std::runtime_error("SCE kernel time provider has expired");
                // Another guest thread may have unmapped it during the sleep.
                if (remaining && context->accessible(remaining, 16, Permission::Write)) context->storePair(remaining, 0, 0);
                return 0;
            };
            if (!waits->Wake(sleeper.Thread, it->first, completion)) { ++it; continue; }
            it = sleepers.erase(it);
        }
        return !sleepers.empty();
    }
    void stopped(GuestThreadHandle thread) {
        std::erase_if(sleepers, [&](const auto& item) { return item.second.Thread == thread; });
    }
};

SceKernelTimeImports::SceKernelTimeImports(Machine& machine, std::shared_ptr<GuestThreads> threads,
                                           std::function<bool()> stopRequested, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, threads, std::move(stopRequested), gateBase)) {
    if (!threads) return;
    impl->waits.emplace(threads->CreateWaitDomain(machine, [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle thread) {
        if (const auto state = weak.lock()) state->stopped(thread);
    }));
    impl->waits->SetOwnerPump([weak = std::weak_ptr<Impl>(impl)] {
        const auto state = weak.lock();
        return state && state->pump();
    });
}

SceKernelTimeImports::~SceKernelTimeImports() = default;

std::optional<std::uint64_t> SceKernelTimeImports::Resolve(const SceImport& import) {
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) return std::nullopt;
    const auto [operation, posix] = service->second;
    if (import.ModuleName != "libkernel" || import.LibraryVersion != 1 || import.ModuleMajor != 1 ||
        import.ModuleMinor != 1 || (import.LibraryName != "libkernel" && !(posix && import.LibraryName == "libScePosix")))
        throw std::runtime_error("Unsupported SCE kernel time import scope/version: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), operation, qualified = identity(import)](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE kernel time import runtime has expired");
        try {
            const auto result = context->invoke(guest, operation);
            guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
        } catch (const std::exception& error) {
            throw std::runtime_error("SCE kernel time import " + qualified + ": " + error.what());
        }
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
