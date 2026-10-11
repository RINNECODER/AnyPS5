#include "KernelThreadExtras.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <cpu/SceModules.hpp>
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>

namespace Cpu::Platform {
namespace {
enum class Op {
    RwInit, RwDestroy, RwWrlock, RwRdlock, RwTrywrlock, RwTryrdlock, RwTimedwrlock, RwTimedrdlock, RwUnlock,
    Once, KeyCreate, KeyDelete, Setspecific, Getspecific,
    SyncWait, SyncWait32, SyncWait64, SyncWake,
    ModuleFromAddr, ModuleForUnwind, DebugRaise, DebugRaiseRelease, DebugCppInfo,
    LoadStart, StopUnload, Dlsym, Trinity, Uuid, AtexitInc, AtexitDec,
    PosixBegin,
    // libScePosix rows that return -1 and store errno (the rows above return errno directly).
    Getpid, SemInit, SemPost, SemWait, SemDestroy, SemTimedwait,
    Getrusage, Signal, Sigprocmask, PhdrMatch,
    Coredump
};
constexpr unsigned errnoReturnRows = 12, coredumpRows = 13;
// POSIX names of the same objects return positive errno values.
constexpr std::array<Op, 22> posixOps{Op::RwInit, Op::RwDestroy, Op::RwWrlock, Op::RwRdlock, Op::RwTrywrlock,
    Op::RwTryrdlock, Op::RwUnlock, Op::Once, Op::KeyCreate, Op::KeyDelete, Op::Setspecific, Op::Getspecific,
    Op::Getpid, Op::SemInit, Op::SemPost, Op::SemWait, Op::SemDestroy, Op::SemTimedwait,
    Op::Getrusage, Op::Signal, Op::Sigprocmask, Op::PhdrMatch};
constexpr std::array<KernelThreadExtraImport, 65> inventory{{
    {"6ULAa0fq4jA", "scePthreadRwlockInit"}, {"BB+kb08Tl9A", "scePthreadRwlockDestroy"},
    {"mqdNorrB+gI", "scePthreadRwlockWrlock"}, {"Ox9i0c7L5w0", "scePthreadRwlockRdlock"},
    {"bIHoZCTomsI", "scePthreadRwlockTrywrlock"}, {"XD3mDeybCnk", "scePthreadRwlockTryrdlock"},
    {"adh--6nIqTk", "scePthreadRwlockTimedwrlock"}, {"iPtZRWICjrM", "scePthreadRwlockTimedrdlock"},
    {"+L98PIbGttk", "scePthreadRwlockUnlock"},
    {"14bOACANTBo", "scePthreadOnce"},
    {"geDaqgH9lTg", "scePthreadKeyCreate"}, {"PrdHuuDekhY", "scePthreadKeyDelete"},
    {"+BzXYkqYeLE", "scePthreadSetspecific"}, {"eoht7mQOCmo", "scePthreadGetspecific"},
    {"Hc4CaR6JBL0", "sceKernelSyncOnAddressWait"}, {"B2n8aDorSH4", "sceKernelSyncOnAddressWait32"},
    {"PZQhiiLXRFs", "sceKernelSyncOnAddressWait64"}, {"q2y-wDIVWZA", "sceKernelSyncOnAddressWake"},
    {"f7KBOafysXo", "sceKernelGetModuleInfoFromAddr"}, {"RpQJJVKTiFM", "sceKernelGetModuleInfoForUnwind"},
    {"OMDRKKAZ8I4", "sceKernelDebugRaiseException"}, {"zE-wXIZjLoM", "sceKernelDebugRaiseExceptionOnReleaseMode"},
    {"cfwBSQyr5Ys", "sceKernelDebugWriteCppExceptionInfo"},
    {"wzvqT4UqKX8", "sceKernelLoadStartModule"}, {"QKd0qM58Qes", "sceKernelStopUnloadModule"},
    {"LwG8g3niqwA", "sceKernelDlsym"}, {"tU5e3f9gSiU", "sceKernelIsTrinityMode"},
    {"Xjoosiw+XPI", "sceKernelUuidCreate"}, {"Tz4RNUCBbGI", "_sceKernelRtldThreadAtexitIncrement"},
    {"8OnWXlgQlvo", "_sceKernelRtldThreadAtexitDecrement"},
    {"ytQULN-nhL4", "pthread_rwlock_init"}, {"1471ajPzxh0", "pthread_rwlock_destroy"},
    {"sIlRvQqsN2Y", "pthread_rwlock_wrlock"}, {"iGjsr1WAtI0", "pthread_rwlock_rdlock"},
    {"XhWHn6P5R7U", "pthread_rwlock_trywrlock"}, {"SFxTMOfuCkE", "pthread_rwlock_tryrdlock"},
    {"EgmLo6EWgso", "pthread_rwlock_unlock"}, {"Z4QosVuAsA0", "pthread_once"},
    {"mqULNdimTn0", "pthread_key_create"}, {"6BpEZuDT7YI", "pthread_key_delete"},
    {"WrOLvHU0yQM", "pthread_setspecific"}, {"0-KXaS70xy4", "pthread_getspecific"},
    {"HoLVWNanBBc", "getpid"}, {"pDuPEf3m4fI", "sem_init"}, {"IKP8typ0QUk", "sem_post"},
    {"YCV5dGGBcCo", "sem_wait"}, {"cDW233RAwWo", "sem_destroy"}, {"w5IHyvahg-o", "sem_timedwait"},
    {"hHlZQUnlxSM", "getrusage"}, {"VADc3MNQ3cM", "signal"}, {"6xVpy0Fdq+I", "_sigprocmask"},
    {"Fjc4-n1+y2g", "__elf_phdr_match_addr"},
    // libSceCoredump: a host crash is never turned into a guest core dump, so the handler is
    // recorded by nothing and every call succeeds, exactly as upstream's stand-ins return 0.
    {"dei8oUx6DbU", "sceCoredumpDebugTextOut"}, {"8zLSfEfW5AU", "sceCoredumpRegisterCoredumpHandler"},
    {"fFkhOgztiCA", "sceCoredumpUnregisterCoredumpHandler"}, {"kK0DUW1Ukgc", "sceCoredumpGetStopInfoCpu"},
    {"Jrs7UUkGOFo", "sceCoredumpGetStopInfoGpu_Agc"}, {"ShChva57wIM", "sceCoredumpUnnamedShChva57wIM"},
    {"5nc2gdLNsok", "sceCoredumpAttachUserFile"}, {"Uxqkdta7wEg", "sceCoredumpSetUserDataType"},
    {"Dbbkj6YHWdo", "sceCoredumpWriteUserData"}, {"MEJ7tc7ThwM", "sceCoredumpAttachMemoryRegionAsUserFile"},
    {"32KQRUK13kI", "sceCoredumpWriteUserString"}, {"DoKHmUw1yiQ", "sceCoredumpAttachUserMemoryFile"},
    {"+YX0z-GUSNw", "sceCoredumpAttachMemoryRegion"}}};
static_assert(inventory.size() == static_cast<std::size_t>(Op::PosixBegin) + posixOps.size() + coredumpRows);
constexpr KernelThreadExtraImport sysmoduleUnwind{"4fU5yvOkVG4", "sceSysmoduleGetModuleInfoForUnwind"};
constexpr unsigned coredumpFirst = inventory.size() - coredumpRows;
std::uint64_t posixResult(std::uint64_t code) {
    return code >= 0x80020000u && code < 0x80020100u ? code - 0x80020000u : code;
}

enum GuestErrno : std::uint32_t { Perm = 1, NoEntry = 2, Search = 3, Deadlock = 11, Fault = 14, Busy = 16, Invalid = 22, Again = 35, TimedOut = 60, Overflow = 84 };
constexpr std::uint32_t error(GuestErrno value) { return 0x80020000u + value; }
constexpr std::uint64_t destroyed = 1;
constexpr unsigned keyCount = 256, destructorIterations = 4;
constexpr std::uint64_t codeOffset = 0x200000;
// Once and exit-hook guest routines (x86-64), with movabs placeholders patched to gates.
constexpr std::array<std::uint8_t, 107> stubCode{
    0x53,0x41,0x54,0x41,0x55,0x48,0x89,0xfb,0x49,0x89,0xf4,0x48,0xb8,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,
    0xff,0xd0,0x48,0x85,0xc0,0x75,0x16,0x41,0xff,0xd4,0x48,0x89,0xdf,0x48,0xb8,0x22,0x22,0x22,0x22,0x22,0x22,
    0x22,0x22,0xff,0xd0,0x31,0xc0,0xeb,0x08,0x48,0x83,0xf8,0x01,0x75,0x02,0x31,0xc0,0x41,0x5d,0x41,0x5c,0x5b,
    0xc3,0x53,0x48,0xb8,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0xff,0xd0,0x48,0x85,0xc0,0x74,0x04,0xff,0xd0,
    0xeb,0xeb,0x5b,0x48,0xb8,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0x44,0xff,0xd0,0x48,0x85,0xc0,0x74,0x02,0xff,
    0xe0,0xc3};
constexpr std::size_t atOnceBegin = 0x0d, atOnceEnd = 0x24, atExitEntry = 0x40, atExitNext = 0x43, atExitDtors = 0x59;
std::atomic<std::uint64_t> nextToken{0xa007000000000003ULL};

bool span(Machine& m, std::uint64_t p, std::size_t size, Permission permission) {
    if (!p || p > std::numeric_limits<std::uint64_t>::max() - size) return false;
    try { m.CheckAccess(p, size, permission); } catch (const std::exception&) { return false; }
    return true;
}
template <class T> T load(Machine& m, std::uint64_t p) {
    T v{}; m.Read(p, std::as_writable_bytes(std::span(&v, 1))); return v;
}
template <class T> void store(Machine& m, std::uint64_t p, T v) { m.Write(p, std::as_bytes(std::span(&v, 1))); }
using Clock = std::chrono::steady_clock;
Clock::time_point after(std::uint64_t microseconds) {
    const auto now = Clock::now();
    const auto limit = std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - now).count();
    if (microseconds >= static_cast<std::uint64_t>(limit)) return Clock::time_point::max();
    return now + std::chrono::microseconds(microseconds);
}
}

std::span<const KernelThreadExtraImport> KernelThreadExtraInventory() { return inventory; }

struct KernelThreadExtras::Impl {
    struct Rwlock {
        std::uint64_t token;
        GuestThreadHandle writer = 0;
        std::map<GuestThreadHandle, unsigned> readers;
        struct Waiter { GuestThreadHandle id; bool write; };
        std::deque<Waiter> waiters;
    };
    struct OnceState { std::uint64_t token; GuestThreadHandle owner; std::deque<GuestThreadHandle> waiters; };
    struct AddressQueue { std::uint64_t token; std::deque<GuestThreadHandle> waiters; };
    enum class Kind { Rwlock, Address, Sem };
    struct Timed { Kind kind; std::uint64_t key; std::uint64_t object; Clock::time_point deadline; std::uint64_t errnoCell = 0; };
    struct Sem { std::uint64_t token; std::uint32_t value; std::deque<GuestThreadHandle> waiters; };
    struct Key { bool allocated = false; std::uint64_t destructor = 0; std::uint64_t generation = 0; };
    struct Value { std::uint64_t value = 0, generation = 0; };
    struct ExitCursor { unsigned round = 0, index = 0; bool called = false; };

    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    std::uint64_t base;
    SceHostTrampolines trampolines;
    std::optional<GuestThreads::WaitDomain> waits;
    std::map<std::uint64_t, Rwlock> rwlocks;
    std::map<std::uint64_t, OnceState> onces;         // keyed by guest control address
    std::map<std::uint64_t, AddressQueue> addresses;  // keyed by guest address
    std::map<std::uint64_t, Sem> sems;                // POSIX semaphores keyed by guest address
    std::array<std::uint64_t, 33> signalHandlers{};   // dispositions are recorded; signals are never delivered
    std::uint32_t blockedSignals = 0;
    std::map<std::int32_t, unsigned> atexitRefs;      // thread-atexit references per module index
    std::map<GuestThreadHandle, Timed> timed;
    std::array<Key, keyCount> keys{};
    std::map<GuestThreadHandle, std::map<unsigned, Value>> values;
    std::map<GuestThreadHandle, ExitCursor> exits;
    std::map<std::string, std::uint64_t> gates;
    std::uint64_t onceStub = 0, posixOnceStub = 0;
    static constexpr std::uint64_t posixOnceOffset = 0x800;
    ModuleSource modules;
    bool codeMapped = false;

    Impl(Machine& m, const std::shared_ptr<GuestThreads>& scheduler, std::uint64_t b)
        : machine(m), threads(scheduler), base(checked(m, scheduler, b)),
          trampolines(m, b, SceHostTrampolines::DefaultCapacity, "Kernel thread extras") {}
    ~Impl() {
        if (waits) waits->Withdraw();
        trampolines.Release();
        if (codeMapped) machine.Unmap(base + codeOffset, 4096);
    }
    static std::uint64_t checked(Machine& m, const std::shared_ptr<GuestThreads>& scheduler, std::uint64_t b) {
        if (!scheduler) throw std::invalid_argument("Kernel thread extras need a guest scheduler");
        if (!b || (b & 4095) || b >= 0x7ffffffff000 - codeOffset)
            throw std::invalid_argument("Invalid kernel thread extras gate base");
        for (const auto& mapping : m.Mappings())
            if (mapping.Address < b + codeOffset + 4096 && b < mapping.Address + mapping.Size)
                throw std::invalid_argument("Kernel thread extras range already mapped");
        return b;
    }
    std::shared_ptr<GuestThreads> runtime() const {
        auto value = threads.lock();
        if (!value) throw std::runtime_error("Kernel thread extras guest scheduler expired");
        return value;
    }
    GuestThreadHandle self() const { return runtime()->ActiveThread(); }
    std::uint64_t token() { return nextToken.fetch_add(1); }

    void install(const std::weak_ptr<Impl>& weak) {
        auto gate = [&](auto handler) {
            return trampolines.Add([weak, handler](Machine& m) {
                const auto state = weak.lock();
                if (!state) throw std::runtime_error("Kernel thread extras provider expired");
                handler(*state, m);
            });
        };
        const auto begin = gate([](Impl& s, Machine& m) { m.Set(Register::Rax, s.onceBegin(m.Get(Register::Rdi), m.Get(Register::Rsi))); });
        const auto posixBegin = gate([](Impl& s, Machine& m) {
            m.Set(Register::Rax, posixResult(s.onceBegin(m.Get(Register::Rdi), m.Get(Register::Rsi))));
        });
        const auto end = gate([](Impl& s, Machine& m) { m.Set(Register::Rax, s.onceEnd(m.Get(Register::Rdi))); });
        const auto next = gate([](Impl& s, Machine& m) { s.nextDestructor(m); });
        const auto dtors = gate([](Impl& s, Machine& m) {
            m.Set(Register::Rdi, 0); m.Set(Register::Rsi, 0); m.Set(Register::Rdx, 0);
            m.Set(Register::Rax, s.runtime()->ThreadDtors());
        });
        auto code = stubCode;
        const auto patch = [&](std::size_t offset, std::uint64_t value) { std::memcpy(code.data() + offset, &value, 8); };
        patch(atOnceBegin, begin); patch(atOnceEnd, end); patch(atExitNext, next); patch(atExitDtors, dtors);
        const auto page = base + codeOffset;
        machine.Map(page, 4096, Permission::Read | Permission::Write);
        codeMapped = true;
        machine.Write(page, std::as_bytes(std::span(code)));
        // The POSIX once copy differs only in its begin gate (errno results).
        auto posix = stubCode;
        std::memcpy(posix.data() + atOnceBegin, &posixBegin, 8);
        std::memcpy(posix.data() + atOnceEnd, &end, 8);
        machine.Write(page + posixOnceOffset, std::as_bytes(std::span(posix).first(atExitEntry)));
        machine.Protect(page, 4096, Permission::Read | Permission::Execute);
        onceStub = page;
        posixOnceStub = page + posixOnceOffset;
        runtime()->SetThreadExitHook(page + atExitEntry);
    }

    // ---- shared wait plumbing -------------------------------------------------
    void block(GuestThreadHandle id, std::uint64_t key, std::optional<Clock::time_point> deadline,
               Kind kind, std::uint64_t object, std::uint64_t errnoCell = 0) {
        if (deadline) timed.insert_or_assign(id, Timed{kind, key, object, *deadline, errnoCell});
        try { waits->BlockFromHostCall(key); }
        catch (...) { timed.erase(id); throw; }
    }
    void wake(GuestThreadHandle id, std::uint64_t key, std::uint32_t result) {
        timed.erase(id);
        if (waits->IsWaiting(id, key)) waits->Wake(id, key, result);
    }
    bool pump() {
        if (timed.empty()) return false;
        const auto now = Clock::now();
        std::vector<std::pair<GuestThreadHandle, Timed>> expired;
        for (const auto& [id, wait] : timed) if (wait.deadline <= now) expired.emplace_back(id, wait);
        for (const auto& [id, wait] : expired) {
            timed.erase(id);
            if (wait.kind == Kind::Rwlock) {
                const auto found = rwlocks.find(wait.object);
                if (found == rwlocks.end()) continue;
                auto& lock = found->second;
                const auto erased = std::erase_if(lock.waiters, [&](const auto& w) { return w.id == id; });
                if (erased && waits->IsWaiting(id, wait.key)) waits->Wake(id, wait.key, error(TimedOut));
                grant(lock);
            } else if (wait.kind == Kind::Sem) {
                const auto found = sems.find(wait.object);
                if (found == sems.end()) continue;
                if (std::erase(found->second.waiters, id) && waits->IsWaiting(id, wait.key))
                    waits->Wake(id, wait.key, std::function<std::uint32_t()>([this, cell = wait.errnoCell] {
                        if (cell && span(machine, cell, 4, Permission::Write)) store<std::int32_t>(machine, cell, TimedOut);
                        return 0xffffffffu; }));
            } else {
                const auto found = addresses.find(wait.object);
                if (found == addresses.end()) continue;
                if (std::erase(found->second.waiters, id) && waits->IsWaiting(id, wait.key))
                    waits->Wake(id, wait.key, error(TimedOut));
                if (found->second.waiters.empty()) addresses.erase(found);
            }
        }
        return !timed.empty();
    }
    void stopped(GuestThreadHandle id) {
        timed.erase(id);
        for (auto& [token, lock] : rwlocks) {
            std::erase_if(lock.waiters, [&](const auto& w) { return w.id == id; });
            // A finished reader no longer blocks writers; a finished writer keeps
            // the lock, as a non-robust owner does.
            if (lock.readers.erase(id)) grant(lock);
        }
        for (auto& [address, sem] : sems) std::erase(sem.waiters, id);
        for (auto it = addresses.begin(); it != addresses.end();) {
            std::erase(it->second.waiters, id);
            it = it->second.waiters.empty() ? addresses.erase(it) : std::next(it);
        }
        for (auto it = onces.begin(); it != onces.end();) {
            auto& state = it->second;
            std::erase(state.waiters, id);
            if (state.owner != id) { ++it; continue; }
            // The initializer never finished: hand the control to the next caller.
            if (state.waiters.empty()) {
                if (span(machine, it->first, 4, Permission::Write)) store<std::int32_t>(machine, it->first, 0);
                it = onces.erase(it);
                continue;
            }
            const auto next = state.waiters.front();
            state.waiters.pop_front();
            state.owner = next;
            wake(next, state.token, 0);
            ++it;
        }
        values.erase(id);
        exits.erase(id);
    }

    // ---- rwlock ----------------------------------------------------------------
    std::uint32_t rwInit(std::uint64_t slot) {
        if (!span(machine, slot, 8, Permission::Write)) return error(Invalid);
        const auto value = token();
        rwlocks.emplace(value, Rwlock{value, 0, {}, {}});
        store<std::uint64_t>(machine, slot, value);
        return 0;
    }
    // Resolves the guest word; the zero static initializer is created lazily.
    std::pair<std::uint32_t, Rwlock*> rwResolve(std::uint64_t slot) {
        if (!span(machine, slot, 8, Permission::Read)) return {error(Invalid), nullptr};
        const auto word = load<std::uint64_t>(machine, slot);
        if (word == 0) {
            if (!span(machine, slot, 8, Permission::Write)) return {error(Invalid), nullptr};
            const auto value = token();
            auto& lock = rwlocks.emplace(value, Rwlock{value, 0, {}, {}}).first->second;
            store<std::uint64_t>(machine, slot, value);
            return {0, &lock};
        }
        const auto found = rwlocks.find(word);
        if (word == destroyed || found == rwlocks.end()) return {error(Invalid), nullptr};
        return {0, &found->second};
    }
    std::uint32_t rwDestroy(std::uint64_t slot) {
        if (!span(machine, slot, 8, Permission::Write)) return error(Invalid);
        const auto word = load<std::uint64_t>(machine, slot);
        if (word == 0) { store<std::uint64_t>(machine, slot, destroyed); return 0; }
        const auto found = rwlocks.find(word);
        if (found == rwlocks.end()) return error(Invalid);
        if (found->second.writer || !found->second.readers.empty() || !found->second.waiters.empty()) return error(Busy);
        rwlocks.erase(found);
        store<std::uint64_t>(machine, slot, destroyed);
        return 0;
    }
    // Writer preference: a new reader waits behind a queued writer unless it
    // already holds a read lock (recursive read must not deadlock).
    static bool readable(const Rwlock& lock, GuestThreadHandle id) {
        if (lock.writer) return false;
        const bool writerQueued = std::any_of(lock.waiters.begin(), lock.waiters.end(), [](const auto& w) { return w.write; });
        return !writerQueued || lock.readers.contains(id);
    }
    std::uint32_t rwLock(std::uint64_t slot, bool write, bool tryOnly, std::optional<Clock::time_point> deadline) {
        const auto [status, lock] = rwResolve(slot);
        if (!lock) return status;
        const auto id = self();
        if (lock->writer == id) return tryOnly ? error(Busy) : error(Deadlock);
        if (write) {
            if (lock->readers.contains(id)) return tryOnly ? error(Busy) : error(Deadlock);
            if (!lock->writer && lock->readers.empty()) { lock->writer = id; return 0; }
        } else if (readable(*lock, id)) {
            auto& depth = lock->readers[id];
            if (depth == std::numeric_limits<unsigned>::max()) return error(Again);
            ++depth;
            return 0;
        }
        if (tryOnly) return error(Busy);
        if (deadline && *deadline <= Clock::now()) return error(TimedOut);
        lock->waiters.push_back({id, write});
        try { block(id, lock->token, deadline, Kind::Rwlock, lock->token); }
        catch (...) { lock->waiters.pop_back(); throw; }
        return 0; // completed by grant() or expiry
    }
    void grant(Rwlock& lock) {
        while (!lock.waiters.empty() && !lock.writer) {
            const auto head = lock.waiters.front();
            if (head.write) {
                if (!lock.readers.empty()) return;
                lock.waiters.pop_front();
                lock.writer = head.id;
                wake(head.id, lock.token, 0);
                return;
            }
            // Admit every queued reader ahead of the first queued writer.
            lock.waiters.pop_front();
            ++lock.readers[head.id];
            wake(head.id, lock.token, 0);
        }
    }
    std::uint32_t rwUnlock(std::uint64_t slot) {
        const auto [status, lock] = rwResolve(slot);
        if (!lock) return status;
        const auto id = self();
        if (lock->writer == id) lock->writer = 0;
        else if (const auto reader = lock->readers.find(id); reader != lock->readers.end()) {
            if (--reader->second == 0) lock->readers.erase(reader);
        } else return error(Perm);
        grant(*lock);
        return 0;
    }

    // ---- once ------------------------------------------------------------------
    // Returns 0 when the caller must run the initializer, 1 when it is done,
    // otherwise an SCE error. Concurrent callers park until onceEnd.
    std::uint32_t onceBegin(std::uint64_t control, std::uint64_t routine) {
        if (!routine || !span(machine, control, 4, Permission::Read | Permission::Write)) return error(Invalid);
        const auto state = load<std::int32_t>(machine, control);
        if (state == 1) return 1;
        if (state != 0 && state != 2 && state != 3) return error(Invalid);
        const auto id = self();
        const auto found = onces.find(control);
        if (found == onces.end()) {
            onces.emplace(control, OnceState{token(), id, {}});
            store<std::int32_t>(machine, control, 2);
            return 0;
        }
        if (found->second.owner == id) return error(Deadlock);
        found->second.waiters.push_back(id);
        store<std::int32_t>(machine, control, 3);
        try { block(id, found->second.token, std::nullopt, Kind::Address, 0); }
        catch (...) { found->second.waiters.pop_back(); throw; }
        return 0;
    }
    std::uint32_t onceEnd(std::uint64_t control) {
        if (!span(machine, control, 4, Permission::Write)) return error(Invalid);
        store<std::int32_t>(machine, control, 1);
        const auto found = onces.find(control);
        if (found == onces.end()) return 0;
        const auto state = std::move(found->second);
        onces.erase(found);
        for (const auto id : state.waiters) wake(id, state.token, 1);
        return 0;
    }

    // ---- TSD -------------------------------------------------------------------
    std::uint32_t keyCreate(std::uint64_t output, std::uint64_t destructor) {
        if (!span(machine, output, 4, Permission::Write)) return error(Invalid);
        for (unsigned i = 0; i < keyCount; ++i) {
            if (keys[i].allocated) continue;
            keys[i].allocated = true;
            keys[i].destructor = destructor;
            ++keys[i].generation;
            store<std::int32_t>(machine, output, static_cast<std::int32_t>(i + 1));
            return 0;
        }
        return error(Again);
    }
    std::optional<unsigned> keyIndex(std::uint64_t key) const {
        const auto value = static_cast<std::int32_t>(key);
        if (value < 1 || value > static_cast<std::int32_t>(keyCount) || !keys[value - 1].allocated) return std::nullopt;
        return static_cast<unsigned>(value - 1);
    }
    std::uint32_t keyDelete(std::uint64_t key) {
        const auto index = keyIndex(key);
        if (!index) return error(Invalid);
        keys[*index].allocated = false;
        keys[*index].destructor = 0;
        ++keys[*index].generation;
        return 0;
    }
    std::uint32_t setSpecific(std::uint64_t key, std::uint64_t value) {
        const auto index = keyIndex(key);
        if (!index) return error(Invalid);
        values[self()][*index] = Value{value, keys[*index].generation};
        return 0;
    }
    std::uint64_t getSpecific(std::uint64_t key) {
        const auto index = keyIndex(key);
        if (!index) return 0;
        const auto thread = values.find(self());
        if (thread == values.end()) return 0;
        const auto value = thread->second.find(*index);
        if (value == thread->second.end() || value->second.generation != keys[*index].generation) return 0;
        return value->second.value;
    }
    // FreeBSD _thread_cleanupspecific: up to PTHREAD_DESTRUCTOR_ITERATIONS rounds;
    // each non-null value with a destructor is cleared before its destructor runs.
    void nextDestructor(Machine& m) {
        const auto id = self();
        auto& cursor = exits[id];
        auto thread = values.find(id);
        for (;;) {
            if (thread != values.end()) {
                for (; cursor.index < keyCount; ++cursor.index) {
                    const auto& key = keys[cursor.index];
                    if (!key.allocated || !key.destructor) continue;
                    const auto value = thread->second.find(cursor.index);
                    if (value == thread->second.end() || !value->second.value ||
                        value->second.generation != key.generation) continue;
                    const auto argument = value->second.value;
                    thread->second.erase(value);
                    ++cursor.index;
                    cursor.called = true;
                    m.Set(Register::Rdi, argument);
                    m.Set(Register::Rax, key.destructor);
                    return;
                }
            }
            if (!cursor.called || ++cursor.round >= destructorIterations) break;
            cursor.index = 0;
            cursor.called = false;
        }
        values.erase(id);
        exits.erase(id);
        m.Set(Register::Rax, 0);
    }

    // ---- SyncOnAddress ---------------------------------------------------------
    std::uint32_t syncWait(std::uint64_t address, std::uint64_t expected, std::uint64_t timeout, unsigned width) {
        if (!address || address % width || !span(machine, address, width, Permission::Read)) return error(Invalid);
        const auto current = width == 8 ? load<std::uint64_t>(machine, address) : load<std::uint32_t>(machine, address);
        const auto wanted = width == 8 ? expected : static_cast<std::uint32_t>(expected);
        if (current != wanted) return 0;
        std::optional<Clock::time_point> deadline;
        if (timeout) {
            if (!span(machine, timeout, 4, Permission::Read)) return error(Fault);
            const auto microseconds = load<std::uint32_t>(machine, timeout);
            if (!microseconds) return error(TimedOut);
            deadline = after(microseconds);
        }
        auto [entry, created] = addresses.try_emplace(address, AddressQueue{token(), {}});
        const auto id = self();
        entry->second.waiters.push_back(id);
        try { block(id, entry->second.token, deadline, Kind::Address, address); }
        catch (...) {
            entry->second.waiters.pop_back();
            if (entry->second.waiters.empty()) addresses.erase(entry);
            throw;
        }
        return 0;
    }
    std::uint32_t syncWake(std::uint64_t address, std::int32_t count) {
        if (!address || count < 0) return error(Invalid);
        const auto found = addresses.find(address);
        if (found == addresses.end()) return 0;
        auto& queue = found->second;
        for (; count > 0 && !queue.waiters.empty(); --count) {
            const auto id = queue.waiters.front();
            queue.waiters.pop_front();
            wake(id, queue.token, 0);
        }
        if (queue.waiters.empty()) addresses.erase(found);
        return 0;
    }

    // ---- module information ----------------------------------------------------
    struct Located {
        const SceModuleRecord* record; std::int32_t index;
        std::uint64_t ehHeader = 0, ehHeaderSize = 0, ehFrame = 0, ehFrameSize = 0;
    };
    static bool loadable(const SceSegment& s) { return s.Type == 1 || s.Type == 0x61000010; }
    std::optional<Located> locate(std::uint64_t address) {
        if (!modules) return std::nullopt;
        const auto list = modules();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& record = list[i];
            for (const auto& segment : record.Image.Segments) {
                if (!loadable(segment)) continue;
                const auto start = segment.Address + record.LoadBias;
                if (address < start || address - start >= segment.MemorySize) continue;
                Located found{&record, static_cast<std::int32_t>(i)};
                frames(found);
                return found;
            }
        }
        return std::nullopt;
    }
    std::uint64_t encoded(std::uint64_t at, std::uint8_t encoding, std::uint64_t& cursor) {
        std::uint64_t value = 0;
        switch (encoding & 0x0f) {
        case 0x00: value = load<std::uint64_t>(machine, at); cursor = at + 8; break;
        case 0x03: value = load<std::uint32_t>(machine, at); cursor = at + 4; break;
        case 0x0b: value = static_cast<std::uint64_t>(static_cast<std::int64_t>(load<std::int32_t>(machine, at))); cursor = at + 4; break;
        case 0x04: value = load<std::uint64_t>(machine, at); cursor = at + 8; break;
        case 0x0c: value = load<std::uint64_t>(machine, at); cursor = at + 8; break;
        default: return 0;
        }
        if ((encoding & 0x70) == 0x10) value += at;
        return value;
    }
    void frames(Located& found) {
        const auto& record = *found.record;
        for (const auto& segment : record.Image.Segments) {
            if (segment.Type != 0x6474e550) continue;
            found.ehHeader = segment.Address + record.LoadBias;
            found.ehHeaderSize = segment.MemorySize;
        }
        if (!found.ehHeader || !span(machine, found.ehHeader, 8, Permission::Read)) return;
        const auto version = load<std::uint8_t>(machine, found.ehHeader);
        const auto encoding = load<std::uint8_t>(machine, found.ehHeader + 1);
        if (version != 1 || encoding == 0xff) return;
        std::uint64_t cursor = 0;
        found.ehFrame = encoded(found.ehHeader + 4, encoding, cursor);
        // Walk CIE/FDE records to the zero terminator inside the containing segment.
        std::uint64_t end = 0;
        for (const auto& segment : record.Image.Segments) {
            const auto start = segment.Address + record.LoadBias;
            if (loadable(segment) && found.ehFrame >= start && found.ehFrame - start < segment.MemorySize)
                end = start + segment.MemorySize;
        }
        if (!end) { found.ehFrame = 0; return; }
        auto at = found.ehFrame;
        while (at + 4 <= end && span(machine, at, 4, Permission::Read)) {
            std::uint64_t length = load<std::uint32_t>(machine, at);
            at += 4;
            if (!length) break;
            if (length == 0xffffffff) {
                if (at + 8 > end) break;
                length = load<std::uint64_t>(machine, at);
                at += 8;
            }
            if (length > end - at) { at = end; break; }
            at += length;
        }
        found.ehFrameSize = std::min(at, end) - found.ehFrame;
    }
    static std::string name(const SceModuleRecord& record) {
        auto value = record.Image.OriginalFilename ? *record.Image.OriginalFilename : record.Image.Path.filename().string();
        if (value.size() > 255) value.resize(255);
        return value;
    }
    void writeName(std::uint64_t output, const std::string& value) {
        std::array<char, 256> buffer{};
        std::memcpy(buffer.data(), value.data(), value.size());
        machine.Write(output, std::as_bytes(std::span(buffer)));
    }
    std::uint32_t moduleFromAddr(std::uint64_t address, std::uint64_t output) {
        constexpr std::uint64_t size = 0x1a8;
        if (!output) return error(Fault);
        if (!span(machine, output, size, Permission::Read | Permission::Write)) return error(Fault);
        if (load<std::uint64_t>(machine, output) < size) return error(Invalid);
        const auto found = locate(address);
        if (!found) return error(Search);
        const auto& record = *found->record;
        std::array<std::byte, size> info{};
        const auto put = [&](std::size_t offset, auto value) { std::memcpy(info.data() + offset, &value, sizeof(value)); };
        put(0x0, size);
        put(0x108, found->index);
        put(0x10c, static_cast<std::uint32_t>(record.TlsModuleId));
        if (record.Image.Tls) {
            put(0x110, record.Image.Tls->Address + record.LoadBias);
            put(0x118, static_cast<std::uint32_t>(record.Image.Tls->FileSize));
            put(0x11c, static_cast<std::uint32_t>(record.Image.Tls->MemorySize));
            put(0x124, static_cast<std::uint32_t>(record.Image.Tls->Alignment));
        }
        put(0x128, record.Init);
        put(0x130, record.Fini);
        put(0x148, found->ehHeader);
        put(0x150, found->ehFrame);
        put(0x158, static_cast<std::uint32_t>(found->ehHeaderSize));
        put(0x15c, static_cast<std::uint32_t>(found->ehFrameSize));
        std::uint32_t count = 0;
        for (const auto& segment : record.Image.Segments) {
            if (!loadable(segment) || count == 4) continue;
            const std::int32_t prot = ((segment.Flags & 4) ? 1 : 0) | ((segment.Flags & 2) ? 2 : 0) | ((segment.Flags & 1) ? 4 : 0);
            put(0x160 + 16 * count, segment.Address + record.LoadBias);
            put(0x168 + 16 * count, static_cast<std::uint32_t>(segment.MemorySize));
            put(0x16c + 16 * count, prot);
            ++count;
        }
        put(0x1a0, count);
        put(0x1a4, std::uint32_t{1});
        machine.Write(output, info);
        writeName(output + 8, name(record));
        return 0;
    }
    std::uint32_t moduleForUnwind(std::uint64_t address, std::uint64_t output) {
        constexpr std::uint64_t size = 0x130;
        if (!output || !span(machine, output, size, Permission::Write)) return error(Fault);
        const auto found = locate(address);
        if (!found) return error(Search);
        const auto& record = *found->record;
        std::array<std::byte, size> info{};
        const auto put = [&](std::size_t offset, std::uint64_t value) { std::memcpy(info.data() + offset, &value, 8); };
        put(0x0, size);
        put(0x108, found->ehHeader);
        put(0x110, found->ehFrame);
        put(0x118, found->ehFrameSize);
        for (const auto& segment : record.Image.Segments) {
            if (!loadable(segment)) continue;
            put(0x120, segment.Address + record.LoadBias);
            put(0x128, segment.MemorySize);
            break;
        }
        machine.Write(output, info);
        writeName(output + 8, name(record));
        return 0;
    }

    // ---- module loading --------------------------------------------------------
    // The module graph is fixed before the guest runs: every module the title can reach was
    // loaded and initialised by SceModules. Loading by path therefore resolves an already
    // present module (matched by file stem, so ".prx" and ".sprx" spellings agree) and an
    // unknown module reports the same ENOENT upstream returns; unloading keeps the image
    // resident and its fini runs at process teardown.
    std::optional<std::string> cstring(std::uint64_t pointer, std::size_t limit = 1024) {
        if (!pointer) return std::nullopt;
        std::string value;
        for (std::size_t i = 0; i < limit; ++i) {
            char c = 0;
            try { machine.Read(pointer + i, std::as_writable_bytes(std::span(&c, 1))); }
            catch (const std::exception&) { return std::nullopt; }
            if (!c) return value;
            value.push_back(c);
        }
        return std::nullopt;
    }
    static std::string stem(std::string value) {
        if (const auto slash = value.find_last_of('/'); slash != std::string::npos) value.erase(0, slash + 1);
        for (const std::string_view extension : {".sprx", ".prx"})
            if (value.size() > extension.size() && value.compare(value.size() - extension.size(), extension.size(), extension) == 0) {
                value.resize(value.size() - extension.size());
                break;
            }
        return value;
    }
    std::optional<std::int32_t> findModule(const std::string& path) {
        const auto wanted = stem(path);
        if (wanted.empty() || !modules) return std::nullopt;
        const auto list = modules();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const auto& image = list[i].Image;
            if (stem(image.Path.filename().string()) == wanted ||
                (image.OriginalFilename && stem(*image.OriginalFilename) == wanted))
                return static_cast<std::int32_t>(i);
        }
        return std::nullopt;
    }
    bool moduleHandle(std::int32_t handle) const { return modules && handle >= 0 && static_cast<std::size_t>(handle) < modules().size(); }
    std::uint32_t writeResult(std::uint64_t result) {
        if (!result) return 0;
        if (!span(machine, result, 4, Permission::Write)) return error(Fault);
        store<std::int32_t>(machine, result, 0);
        return 0;
    }
    std::uint64_t loadStart(std::uint64_t path, std::uint64_t result) {
        const auto name = cstring(path);
        if (!name) return error(Fault);
        if (const auto status = writeResult(result)) return status;
        const auto found = findModule(*name);
        if (!found) {
            // A title-loaded module absent from the --sce-module graph: name it, since the
            // title usually continues and fails much later on the missing handle.
            std::cerr << "{\"schema_version\":1,\"event\":\"module_load_failed\",\"path\":\"";
            for (const unsigned char c : *name) {
                if (c == '"' || c == '\\') std::cerr << '\\' << c;
                else if (c < 0x20) std::cerr << "\\u00" << "0123456789abcdef"[c >> 4] << "0123456789abcdef"[c & 15];
                else std::cerr << c;
            }
            std::cerr << "\",\"error\":" << error(NoEntry) << "}\n";
        }
        return found ? static_cast<std::uint32_t>(*found) : error(NoEntry);
    }
    std::uint32_t stopUnload(std::int32_t handle, std::uint64_t result) {
        if (const auto status = writeResult(result)) return status;
        return moduleHandle(handle) ? 0 : error(Search);
    }
    std::uint32_t dlsym(std::int32_t handle, std::uint64_t symbol, std::uint64_t output) {
        const auto name = cstring(symbol, 512);
        if (!name || !output || !span(machine, output, 8, Permission::Write)) return error(Fault);
        if (!moduleHandle(handle)) return error(Search);
        const auto nid = Nid::ComputeNid(*name, "libkernel");
        const auto list = modules();
        const auto& record = list[static_cast<std::size_t>(handle)];
        for (const auto& item : record.Image.Exports) {
            if ((item.Type != 1 && item.Type != 2) || (item.Identity.Nid != nid && item.Identity.Nid != *name)) continue;
            store<std::uint64_t>(machine, output, item.Value + record.LoadBias);
            return 0;
        }
        return error(Search);
    }
    std::uint32_t uuidCreate(std::uint64_t output) {
        if (!output || !span(machine, output, 16, Permission::Write)) return error(Invalid);
        static thread_local std::random_device device;
        std::array<std::uint32_t, 4> value{};
        for (auto& word : value) word = device();
        value[1] = (value[1] & 0x0fffffffu) | 0x40000000u;   // RFC 4122 version 4 and variant bits
        value[2] = (value[2] & 0xffffff3fu) | 0x80u;
        machine.Write(output, std::as_bytes(std::span(value)));
        return 0;
    }
    std::optional<std::int32_t> containing(std::uint64_t address) {
        if (!modules) return std::nullopt;
        const auto list = modules();
        for (std::size_t i = 0; i < list.size(); ++i)
            for (const auto& segment : list[i].Image.Segments) {
                if (!loadable(segment)) continue;
                const auto start = segment.Address + list[i].LoadBias;
                if (address >= start && address - start < segment.MemorySize) return static_cast<std::int32_t>(i);
            }
        return std::nullopt;
    }
    // Images never unload, so the per-image thread-atexit reference only needs balanced counting.
    std::uint32_t atexitReference(std::uint64_t address, bool increment) {
        const auto index = containing(address);
        if (!index) return error(Search);
        auto& count = atexitRefs[*index];
        if (increment) { ++count; return 0; }
        if (!count) return error(Invalid);
        --count;
        return 0;
    }

    // ---- POSIX semaphores (-1 and errno) ---------------------------------------
    std::uint64_t fail(GuestErrno code) {
        if (const auto cell = runtime()->ActiveErrnoAddress(); cell && span(machine, cell, 4, Permission::Write))
            store<std::int32_t>(machine, cell, static_cast<std::int32_t>(code));
        return 0xffffffffffffffffULL;
    }
    // Host crashes and kill() are not turned into guest signals, so handlers are only recorded
    // (SIG_DFL = 0 until set) and the process-wide mask is bookkeeping a guest can read back.
    std::uint64_t signalSet(std::uint64_t number, std::uint64_t handler) {
        const auto sig = static_cast<std::int32_t>(number);
        if (sig < 1 || sig > 32 || sig == 9 || sig == 17 || handler == 0xffffffffffffffffULL) return fail(Invalid);
        const auto previous = signalHandlers[static_cast<std::size_t>(sig)];
        signalHandlers[static_cast<std::size_t>(sig)] = handler;
        return previous;
    }
    std::uint64_t sigprocmask(std::int32_t how, std::uint64_t set, std::uint64_t previous) {
        if (previous) {
            if (!span(machine, previous, 16, Permission::Write)) return fail(Fault);
            std::array<std::uint32_t, 4> old{blockedSignals, 0, 0, 0};
            machine.Write(previous, std::as_bytes(std::span(old)));
        }
        if (!set) return 0;
        if (!span(machine, set, 4, Permission::Read)) return fail(Fault);
        const auto bits = load<std::uint32_t>(machine, set);
        switch (how) {
        case 1: blockedSignals |= bits; return 0;
        case 2: blockedSignals &= ~bits; return 0;
        case 3: blockedSignals = bits; return 0;
        default: return fail(Invalid);
        }
    }
    std::uint64_t getrusage(std::int32_t who, std::uint64_t output) {
        if (who != 0 && who != 1) return fail(Invalid);
        constexpr std::size_t size = 144;   // two timevals and fourteen longs, as FreeBSD lays out rusage
        if (!output || !span(machine, output, size, Permission::Write)) return fail(Fault);
        static_assert(sizeof(::rusage) == size, "host rusage layout differs from the guest layout");
        ::rusage host{};
        ::getrusage(RUSAGE_SELF, &host);   // the guest has one process; a thread query reports it
        machine.Write(output, std::as_bytes(std::span(&host, 1)));
        return 0;
    }
    // Reports whether an executable PT_LOAD of the dl_phdr_info at `info` contains `address`.
    std::uint64_t phdrMatch(std::uint64_t info, std::uint64_t address) {
        if (!info || !span(machine, info, 26, Permission::Read)) return 0;
        const auto bias = load<std::uint64_t>(machine, info), headers = load<std::uint64_t>(machine, info + 16);
        const auto count = load<std::uint16_t>(machine, info + 24);
        for (std::uint16_t i = 0; i < count; ++i) {
            const auto at = headers + 56ULL * i;
            if (!span(machine, at, 56, Permission::Read)) return 0;
            if (load<std::uint32_t>(machine, at) != 1 || !(load<std::uint32_t>(machine, at + 4) & 1)) continue;
            const auto begin = bias + load<std::uint64_t>(machine, at + 16), size = load<std::uint64_t>(machine, at + 40);
            if (begin <= address && address + 8 < begin + size) return 1;
        }
        return 0;
    }
    std::uint64_t semInit(std::uint64_t address, std::uint32_t value) {
        if (!address || !span(machine, address, 4, Permission::Write)) return fail(Fault);
        if (value > 0x7fffffffu) return fail(Invalid);
        if (const auto found = sems.find(address); found != sems.end() && !found->second.waiters.empty()) return fail(Busy);
        sems.insert_or_assign(address, Sem{token(), value, {}});
        return 0;
    }
    std::uint64_t semDestroy(std::uint64_t address) {
        const auto found = sems.find(address);
        if (found == sems.end()) return fail(Invalid);
        if (!found->second.waiters.empty()) return fail(Busy);
        sems.erase(found);
        return 0;
    }
    std::uint64_t semPost(std::uint64_t address) {
        const auto found = sems.find(address);
        if (found == sems.end()) return fail(Invalid);
        auto& sem = found->second;
        if (!sem.waiters.empty()) {   // hand the unit straight to the oldest waiter
            const auto id = sem.waiters.front();
            sem.waiters.pop_front();
            wake(id, sem.token, 0);
            return 0;
        }
        if (sem.value == 0x7fffffffu) return fail(Overflow);
        ++sem.value;
        return 0;
    }
    std::uint64_t semWait(std::uint64_t address, std::optional<Clock::time_point> deadline) {
        const auto found = sems.find(address);
        if (found == sems.end()) return fail(Invalid);
        auto& sem = found->second;
        if (sem.value) { --sem.value; return 0; }
        if (deadline && *deadline <= Clock::now()) return fail(TimedOut);
        const auto id = self();
        sem.waiters.push_back(id);
        try { block(id, sem.token, deadline, Kind::Sem, address, runtime()->ActiveErrnoAddress()); }
        catch (...) {
            sem.waiters.pop_back();
            throw;
        }
        return 0;
    }
    std::uint64_t semTimedwait(std::uint64_t address, std::uint64_t absolute) {
        if (!absolute || !span(machine, absolute, 16, Permission::Read)) return fail(Fault);
        const auto seconds = load<std::int64_t>(machine, absolute), nanoseconds = load<std::int64_t>(machine, absolute + 8);
        if (nanoseconds < 0 || nanoseconds >= 1000000000) return fail(Invalid);
        const auto target = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanoseconds)));
        const auto remaining = target - std::chrono::system_clock::now();
        const auto limit = std::chrono::duration_cast<std::chrono::microseconds>(Clock::time_point::max() - Clock::now()).count();
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
        const auto deadline = micros <= 0 ? Clock::now() : micros >= limit ? Clock::time_point::max() : Clock::now() + std::chrono::microseconds(micros);
        return semWait(address, deadline);
    }

    std::uint64_t invoke(Op op, Machine& m) {
        const auto rdi = m.Get(Register::Rdi), rsi = m.Get(Register::Rsi), rdx = m.Get(Register::Rdx);
        switch (op) {
        case Op::RwInit: return rwInit(rdi);
        case Op::RwDestroy: return rwDestroy(rdi);
        case Op::RwWrlock: return rwLock(rdi, true, false, std::nullopt);
        case Op::RwRdlock: return rwLock(rdi, false, false, std::nullopt);
        case Op::RwTrywrlock: return rwLock(rdi, true, true, std::nullopt);
        case Op::RwTryrdlock: return rwLock(rdi, false, true, std::nullopt);
        case Op::RwTimedwrlock: return rwLock(rdi, true, false, after(static_cast<std::uint32_t>(rsi)));
        case Op::RwTimedrdlock: return rwLock(rdi, false, false, after(static_cast<std::uint32_t>(rsi)));
        case Op::RwUnlock: return rwUnlock(rdi);
        case Op::KeyCreate: return keyCreate(rdi, rsi);
        case Op::KeyDelete: return keyDelete(rdi);
        case Op::Setspecific: return setSpecific(rdi, rsi);
        case Op::Getspecific: return getSpecific(rdi);
        case Op::SyncWait: case Op::SyncWait32: return syncWait(rdi, rsi, rdx, 4);
        case Op::SyncWait64: return syncWait(rdi, rsi, rdx, 8);
        case Op::SyncWake: return syncWake(rdi, static_cast<std::int32_t>(rsi));
        case Op::ModuleFromAddr: return moduleFromAddr(rdi, rdx);
        case Op::ModuleForUnwind: return moduleForUnwind(rdi, rdx);
        case Op::DebugRaise: case Op::DebugRaiseRelease:
            // Retail kernels deliver a debug exception only to an attached
            // debugger; without one the caller continues.
            std::cerr << "{\"schema_version\":1,\"event\":\"kernel_debug_exception\",\"release_mode\":"
                      << (op == Op::DebugRaiseRelease ? "true" : "false") << ",\"code\":" << static_cast<std::uint32_t>(rdi)
                      << ",\"parameter\":" << static_cast<std::uint32_t>(rsi);
            {
                // Return address plus the frame-pointer chain identify which guest code raised it.
                const auto rsp = m.Get(Register::Rsp);
                if (span(m, rsp, 8, Permission::Read)) std::cerr << ",\"return_address\":" << load<std::uint64_t>(m, rsp);
                std::cerr << ",\"frames\":[";
                auto frame = m.Get(Register::Rbp);
                for (int depth = 0; depth < 12 && span(m, frame, 16, Permission::Read); ++depth) {
                    std::cerr << (depth ? "," : "") << load<std::uint64_t>(m, frame + 8);
                    const auto next = load<std::uint64_t>(m, frame);
                    if (next <= frame) break;
                    frame = next;
                }
                std::cerr << "]}\n";
            }
            return 0;
        case Op::DebugCppInfo: return 0;
        case Op::LoadStart: return loadStart(rdi, m.Get(Register::R9));
        case Op::StopUnload: return stopUnload(static_cast<std::int32_t>(rdi), m.Get(Register::R9));
        case Op::Dlsym: return dlsym(static_cast<std::int32_t>(rdi), rsi, rdx);
        case Op::Trinity: case Op::Coredump: return 0;
        case Op::Uuid: return uuidCreate(rdi);
        case Op::AtexitInc: return atexitReference(rdi, true);
        case Op::AtexitDec: return atexitReference(rdi, false);
        case Op::Getpid: return static_cast<std::uint32_t>(::getpid());
        case Op::SemInit: return semInit(rdi, static_cast<std::uint32_t>(rdx));
        case Op::SemPost: return semPost(rdi);
        case Op::SemWait: return semWait(rdi, std::nullopt);
        case Op::SemDestroy: return semDestroy(rdi);
        case Op::SemTimedwait: return semTimedwait(rdi, rsi);
        case Op::Getrusage: return getrusage(static_cast<std::int32_t>(rdi), rsi);
        case Op::Signal: return signalSet(rdi, rsi);
        case Op::Sigprocmask: return sigprocmask(static_cast<std::int32_t>(rdi), rsi, rdx);
        case Op::PhdrMatch: return phdrMatch(rdi, rsi);
        case Op::Once: case Op::PosixBegin: break;
        }
        throw std::runtime_error("Kernel thread extras operation is not routed");
    }
};

KernelThreadExtras::KernelThreadExtras(Machine& m, const std::shared_ptr<GuestThreads>& threads, std::uint64_t base)
    : impl(std::make_shared<Impl>(m, threads, base)) {
    impl->waits.emplace(threads->CreateWaitDomain(m, [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle id) {
        if (const auto provider = weak.lock()) provider->stopped(id);
    }));
    impl->waits->SetOwnerPump([weak = std::weak_ptr<Impl>(impl)] {
        const auto provider = weak.lock();
        return provider && provider->pump();
    });
    impl->install(impl);
}
KernelThreadExtras::~KernelThreadExtras() = default;
void KernelThreadExtras::SetModuleSource(ModuleSource source) { impl->modules = std::move(source); }
std::optional<std::uint64_t> KernelThreadExtras::Resolve(const SceImport& import, std::uint8_t type, std::uint64_t size) {
    // libSceSysmodule's unwind lookup takes the same (address, flags, info) arguments and fills
    // the same 0x130-byte record as sceKernelGetModuleInfoForUnwind, from the guest module graph.
    if (import.Nid == sysmoduleUnwind.Nid) {
        if (import.LibraryName != "libSceSysmodule" || import.ModuleName != "libSceSysmodule" ||
            import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
            return std::nullopt;
        if (type != 2 || size != 0)
            throw std::runtime_error("Unsupported kernel thread extras symbol type/size: " + import.Nid);
        if (const auto found = impl->gates.find(import.Nid); found != impl->gates.end()) return found->second;
        const auto gate = impl->trampolines.Add([weak = std::weak_ptr<Impl>(impl)](Machine& m) {
            const auto state = weak.lock();
            if (!state) throw std::runtime_error("Kernel thread extras provider expired");
            m.Set(Register::Rax, state->invoke(Op::ModuleForUnwind, m));
        });
        impl->gates.emplace(import.Nid, gate);
        return gate;
    }
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size()) return std::nullopt;
    const bool coredump = op >= coredumpFirst;
    const bool posixRow = !coredump && op >= static_cast<unsigned>(Op::PosixBegin);
    const bool library = coredump ? import.LibraryName == "libSceCoredump"
        : import.LibraryName == "libkernel" || (posixRow && import.LibraryName == "libScePosix");
    if (!library || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        return std::nullopt;
    if (type != 2 || size != 0)
        throw std::runtime_error("Unsupported kernel thread extras symbol type/size: " + import.Nid);
    const bool posix = posixRow;
    const bool errnoStyle = posix && op - static_cast<unsigned>(Op::PosixBegin) < errnoReturnRows;
    const auto operation = posix ? posixOps[op - static_cast<unsigned>(Op::PosixBegin)] : coredump ? Op::Coredump : static_cast<Op>(op);
    if (operation == Op::Once) return posix ? impl->posixOnceStub : impl->onceStub;
    if (const auto found = impl->gates.find(import.Nid); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines.Add([weak = std::weak_ptr<Impl>(impl), operation, errnoStyle](Machine& m) {
        const auto state = weak.lock();
        if (!state) throw std::runtime_error("Kernel thread extras provider expired");
        const auto result = state->invoke(operation, m);
        // getspecific returns a value, never an error.
        m.Set(Register::Rax, errnoStyle && operation != Op::Getspecific ? posixResult(result) : result);
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}
}
