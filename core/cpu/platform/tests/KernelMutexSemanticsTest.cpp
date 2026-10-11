#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <chrono>
#include <ctime>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

// Issue #280 (audit PLAT-04/05/06/07/15/23). A linked synthetic guest drives the
// target provider through real PLT calls and GuestThreads. Each mode observes one
// libthr-compatible contract through guest-visible return codes, slot words and
// exclusion of a second guest thread; no provider internals are inspected.
namespace {
constexpr std::uint64_t Bias = 0x1000000;
constexpr std::uint64_t Perm = 0x80020001, Deadlock = 0x8002000b, Busy = 0x80020010, Invalid = 0x80020016,
                        TimedOut = 0x8002003c;
constexpr std::uint64_t Done = 0x53454d414e544943ULL, Payload = 0x1020304050607080ULL;
using State = std::array<std::uint64_t, 128>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports threadImports{machine, threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    Cpu::Platform::TargetKernelMutexes mutexes{machine, threads};
    std::unique_ptr<Cpu::SceModules> graph;
    std::uint64_t receiptAddress = 0;
    Session(const char* path, unsigned mode) {
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
                "Mutex semantics fixture requires native Modern QEMU TCG");
        lifecycle.SetProcessExitHandler([runtime = std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
            const auto owner = runtime.lock();
            require(bool(owner), "Mutex semantics process-exit scheduler expired");
            owner->ProcessExitFromHostCall(code);
        });
        const std::array hosts{Cpu::SceHostModule{"libkernel.prx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}}};
        graph = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{path, Bias},
            std::span<const Cpu::SceModuleFile>{}, hosts,
            [&](const Cpu::SceImport& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                if (const auto gate = mutexes.Resolve(import, type)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = mutexes.ResolveCondition(import, type, 0)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = threadImports.Resolve(import, type)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = lifecycle.Resolve(import)) return Cpu::SceResolvedImport{*gate, type};
                return std::nullopt;
            });
        for (const auto& item : graph->Modules()[0].Image.Exports) if (item.Type == 1 && item.Size == sizeof(State)) {
            require(!receiptAddress, "Ambiguous semantics receipt"); receiptAddress = Bias + item.Value;
        }
        require(receiptAddress && receipt() == State{}, "Loading ran guest code or lost the semantics receipt");
        put(1, mode);
        Cpu::SetupSceEntry(machine, graph->Main(), {"public-mutex-semantics"}, graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry, graph->InitialStack(), graph->Tls(), graph->ThreadTlsFactory()});
        graph->SetExecutor(threads->ModuleExecutor());
        graph->InitializeDependencies();
    }
    ~Session() { threads->Withdraw(); }
    State receipt() const { State s{}; machine.Read(receiptAddress, std::as_writable_bytes(std::span(s))); return s; }
    void put(unsigned index, std::uint64_t value) {
        machine.Write(receiptAddress + 8 * index, std::as_bytes(std::span(&value, 1)));
    }
    State run() {
        require(graph->RunMain(3000000, 30000) == Cpu::StopReason::Exit && machine.ExitCode() == 0,
                "Semantics guest did not finish");
        const auto s = receipt();
        require(s[0] == Done && !s[4] && !s[6], "Semantics guest status or protected payload differs");
        return s;
    }
};
bool token(std::uint64_t value) { return value > 2; }

void staticInitializers(const char* path) {
    const auto s = Session(path, 1).run();
    require(s[10] == 0 && token(s[11]) && s[12] == 2, "Zero static mutex lock/destroy differs");
    require(s[13] == 0 && token(s[14]) && s[14] != s[11],
            "A zeroed mutex at a destroyed object's address was not lazily initialized (PLAT-05)");
    require(s[15] == 0 && token(s[16]) && s[17] == Deadlock, "Adaptive static initializer 1 differs");
    require(s[18] == Invalid && s[19] == Invalid, "Destroyed mutex sentinel 2 was admitted");
    require(s[20] == 0 && token(s[21]) && s[22] == 1, "Zero static condition signal/destroy differs");
    require(s[23] == 0 && token(s[24]) && s[24] != s[21],
            "A zeroed condition at a destroyed object's address was not lazily initialized (PLAT-05)");
    require(s[25] == Invalid, "Destroyed condition sentinel 1 was admitted");
}
void initOverwrites(const char* path) {
    const auto s = Session(path, 2).run();
    require(s[10] == 0, "Mutex attribute re-init returned an error instead of overwriting (PLAT-04)");
    require(token(s[11]) && s[12] == Deadlock, "Re-initialized attribute kept the stale recursive type");
    require(s[13] == 0 && token(s[14]) && s[14] != s[11], "Mutex re-init did not overwrite the handle (PLAT-04)");
    require(token(s[15]) && s[16] == 0 && token(s[17]) && s[17] != s[15],
            "Condition re-init did not overwrite the handle (PLAT-04)");
    require(s[18] == 0, "Condition attribute re-init returned an error instead of overwriting (PLAT-04)");
}
void handleIdentity(const char* path) {
    const auto s = Session(path, 3).run();
    require(s[10] == 0 && s[11] == Busy, "A copied mutex handle was not the same mutex (PLAT-06)");
    require(s[40] == Busy && s[12] == 1 && s[41] == 1 && s[13] == 0 && s[14] == 0x2801 && s[15] == Payload + 1,
            "Copied-handle ownership did not exclude another guest thread");
    require(s[16] == 0 && s[17] == 0, "POSIX lock through a copied handle differs");
    require(s[18] == 0 && s[43] == 0 && s[44] == Busy && s[19] == 0x2802,
            "Condition wait/signal through copied mutex and condition handles differs");
    require(s[20] == Invalid && s[21] == Invalid, "A forged handle was admitted");
    require(s[22] == 1 && token(s[23]) && s[24] == Invalid,
            "Destroy through a copied condition handle did not end the shared object");
}
void ownerExit(const char* path) {
    const auto s = Session(path, 4).run();
    require(s[40] == 0 && s[10] == 0x2803, "Exiting owner did not acquire the mutex");
    require(s[11] == Busy && s[12] == Perm && s[13] == Busy && s[14] == Perm && s[15] == 1,
            "Non-robust owner exit did not leave the mutex owned (PLAT-07)");
    require(s[41] == 1 && s[16] == 1 && !s[43] && token(s[17]), "A later locker acquired a mutex its exited owner held");
}
void destroyAfterBroadcast(const char* path) {
    const auto s = Session(path, 5).run();
    require(s[10] == 0 && s[11] == 0 && s[12] == 1,
            "Condition destroy after broadcast returned busy (PLAT-15)");
    require(s[13] == 0 && token(s[14]) && s[14] != s[30] && s[15] == 0,
            "Reusing a destroyed condition's memory failed or a waiter returned before reacquiring");
    require(s[16] == 0x2810 && s[17] == 0x2811 && s[18] == 0, "Broadcast waiters did not finish cleanly");
    require(s[19] == 0 && s[20] == 0 && s[21] == 0x2810, "Destroy after signal-one differs (PLAT-15)");
    require(s[42] == 0 && s[43] == 0 && s[44] == Busy && s[45] == Busy && s[46] == 3,
            "Woken waiters did not return 0 owning the mutex");
}
void badAttribute(const char* path) {
    const auto s = Session(path, 6).run();
    require(s[10] == Invalid && s[11] == 0x5eed, "A zero mutex attribute was silently treated as default (PLAT-23)");
    require(s[12] == 0 && s[13] == Invalid && s[14] == 0x5eed, "A destroyed mutex attribute was admitted (PLAT-23)");
    require(s[15] == Invalid && s[16] == Invalid && s[17] == 0x5eed, "A stale or forged mutex attribute was admitted");
    require(s[18] == Invalid && s[19] == 0x5eed, "A zero condition attribute was admitted");
    require(s[20] == 0, "A live mutex attribute was refused");
}
void timedLock(const char* path) {
    Session session(path, 7);
    const auto start = std::chrono::steady_clock::now();
    const auto s = session.run();
    require(s[10] == 0 && s[11] == Deadlock, "Uncontended or self timed lock differs");
    require(s[40] == 1 && s[12] == TimedOut && s[13] == TimedOut && s[14] == 0x2820,
            "Timed lock held by another thread did not expire with SCE ETIMEDOUT");
    // The rest of this guest finishes in a few milliseconds; only a real 300 ms
    // expiry makes the run this long.
    require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(300),
            "Timed lock expired before its relative deadline");
    require(s[15] == 1 && s[43] == 0 && s[44] == Busy && s[16] == 0x2821,
            "Timed lock waiter acquired early or was not granted on release");
    require(s[17] == 0 && token(s[18]), "Timed lock did not lazily initialize a static mutex");
}
void conditionClock(const char* path) {
    Session session(path, 8);
    timespec monotonic{};
    require(clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0, "Host monotonic clock unavailable");
    session.put(100, static_cast<std::uint64_t>(monotonic.tv_sec));
    session.put(101, static_cast<std::uint64_t>(monotonic.tv_nsec));
    const auto s = session.run();
    require(s[10] == Invalid && s[11] == 22 && s[19] == Invalid && s[12] == 0 && s[18] == 0,
            "Condition attribute setclock results differ");
    require(s[40] == 1 && s[13] == 1 && s[41] == 0 && s[14] == 0x2830,
            "Monotonic-clock timed wait expired against the realtime clock or missed its signal");
    require(s[15] == 60 && s[16] == 60 && s[17] == Busy,
            "Expired monotonic or realtime absolute deadline did not time out with the mutex reacquired");
}
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: KernelMutexSemanticsTest packaged.elf\n"; return 1; }
    // Each audit item runs in its own guest so one regression cannot hide another.
    const std::array<std::pair<const char*, void (*)(const char*)>, 8> cases{{
        {"static initializers (PLAT-05)", staticInitializers}, {"init overwrites (PLAT-04)", initOverwrites},
        {"handle identity (PLAT-06)", handleIdentity}, {"owner exit (PLAT-07)", ownerExit},
        {"destroy after broadcast (PLAT-15)", destroyAfterBroadcast}, {"bad attribute (PLAT-23)", badAttribute},
        {"scePthreadMutexTimedlock", timedLock}, {"condattr setclock", conditionClock}}};
    unsigned failed = 0;
    for (const auto& [context, check] : cases) {
        try { check(argv[1]); }
        catch (const std::exception& error) { std::cerr << "FAIL " << context << ": " << error.what() << '\n'; ++failed; }
    }
    if (failed) return 1;
    std::cout << "PASS mutex/condition lazy static init, init overwrite, handle identity, non-robust owner exit, destroy after broadcast, bad attributes, timed lock and condition clocks\n";
}
