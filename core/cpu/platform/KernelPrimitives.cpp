#include "KernelPrimitives.hpp"
#include <cpu/SceElf.hpp>
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

namespace Cpu::Platform {
namespace {
constexpr unsigned mutexCount = 9;
constexpr unsigned conditionEnd = 21;
constexpr unsigned timeoutBegin = 23;
constexpr unsigned mutexTimedlock = 25;
constexpr unsigned conditionSetclock = 26;
constexpr unsigned posixConditionSetclock = 27;
constexpr std::array<KernelPrimitiveImport, 28> inventory{{
    {"cmo1RIYva9o", "scePthreadMutexInit"}, {"upoVrzMHFeE", "scePthreadMutexTrylock"},
    {"tn3VlD0hG60", "scePthreadMutexUnlock"}, {"2Of0f+3mhhE", "scePthreadMutexDestroy"},
    {"F8bUHwAG284", "scePthreadMutexattrInit"}, {"iMp8QpE+XO4", "scePthreadMutexattrSettype"},
    {"smWEktiyyG0", "scePthreadMutexattrDestroy"}, {"1FGvU0i9saQ", "scePthreadMutexattrSetprotocol"},
    {"9UK1vLZQft4", "scePthreadMutexLock"},
    {"2Tb92quprl0", "scePthreadCondInit"}, {"g+PZd2hiacg", "scePthreadCondDestroy"},
    {"WKAXJ4XBPQ4", "scePthreadCondWait"}, {"kDh-NfxgMtE", "scePthreadCondSignal"},
    {"JGgj7Uvrl+A", "scePthreadCondBroadcast"}, {"m5-2bsNfv7s", "scePthreadCondattrInit"},
    {"waPcxYiR3WA", "scePthreadCondattrDestroy"},
    {"0TyVk4MSLt0", "pthread_cond_init"}, {"RXXqi4CtF8w", "pthread_cond_destroy"},
    {"Op8TBGY5KHg", "pthread_cond_wait"}, {"2MOy+rUfuhQ", "pthread_cond_signal"},
    {"mkx2fVhNMsg", "pthread_cond_broadcast"},
    {"7H0iTOciTLo", "pthread_mutex_lock"}, {"2Z+PpY6CaJg", "pthread_mutex_unlock"},
    {"27bAgiJmOh0", "pthread_cond_timedwait"},
    {"BmMjYxmew1w", "scePthreadCondTimedwait"},
    {"IafI2PxcPnQ", "scePthreadMutexTimedlock"},
    {"c-bxj027czs", "scePthreadCondattrSetclock"}, {"EjllaAqAPZo", "pthread_condattr_setclock"}}};
// POSIX rows return positive errno values and may also be exported by libScePosix.
bool posixRow(unsigned op) { return (op >= 16 && op <= timeoutBegin) || op == posixConditionSetclock; }
bool mutexRow(unsigned op) { return op < mutexCount || op == conditionEnd || op == conditionEnd + 1 || op == mutexTimedlock; }
std::atomic<std::uint64_t> nextToken{0xa005000000000003ULL};
constexpr auto rw = Permission::Read | Permission::Write;
// Guest Orbis error table, independent of the host's errno numerals.
enum GuestErrno : unsigned { Perm = 1, Deadlock = 11, Busy = 16, Invalid = 22, Again = 35, TimedOut = 60 };
std::uint32_t error(GuestErrno posix) { return 0x80020000u + posix; }
// libthr sentinels stored in the guest handle word. Anything else is a handle
// value: copies of it name the same object, wherever the copy lives.
constexpr std::uint64_t mutexDestroyed = 2, conditionDestroyed = 1;
// Guest clock ids (FreeBSD numbering); condition deadlines use one of two bases.
constexpr unsigned realtimeClock = 0, monotonicClock = 4;
void span(Machine& m, std::uint64_t p, Permission permission) {
    if (!p || p > std::numeric_limits<std::uint64_t>::max() - 8)
        throw std::runtime_error("Invalid kernel primitive guest slot");
    m.CheckAccess(p, 8, permission);
}
std::uint64_t read(Machine& m, std::uint64_t p) {
    span(m, p, Permission::Read);
    std::uint64_t v; m.Read(p, std::as_writable_bytes(std::span(&v, 1))); return v;
}
void write(Machine& m, std::uint64_t p, std::uint64_t v) {
    m.Write(p, std::as_bytes(std::span(&v, 1)));
}
void name(Machine& m, std::uint64_t p) {
    if (!p) return;
    for (unsigned i = 0; i < 4096; ++i) {
        if (p > std::numeric_limits<std::uint64_t>::max() - i)
            throw std::runtime_error("Invalid kernel mutex name span");
        m.CheckAccess(p + i, 1, Permission::Read);
        std::byte b; m.Read(p + i, std::span(&b, 1)); if (b == std::byte{0}) return;
    }
    throw std::runtime_error("Unsupported kernel mutex name exceeding bounded 4096-byte scan");
}
using DeadlineClock = std::chrono::steady_clock;
struct Timespec { std::int64_t seconds = 0, nanoseconds = 0; };
Timespec realtimeNow() {
    const auto epoch = std::chrono::system_clock::now().time_since_epoch();
    Timespec now{std::chrono::duration_cast<std::chrono::seconds>(epoch).count(), 0};
    now.nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch - std::chrono::seconds(now.seconds)).count();
    if (now.nanoseconds < 0) { --now.seconds; now.nanoseconds += 1000000000; }
    return now;
}
// The guest's monotonic clock family reads the host CLOCK_MONOTONIC base.
Timespec monotonicNow() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) throw std::runtime_error("Host monotonic clock unavailable");
    return {static_cast<std::int64_t>(value.tv_sec), static_cast<std::int64_t>(value.tv_nsec)};
}
// Each clock is read at most once per owner turn, and only when a deadline needs it.
class Clocks {
public:
    DeadlineClock::time_point Steady() { if (!steady) steady = DeadlineClock::now(); return *steady; }
    const Timespec& Realtime() { if (!realtime) realtime = realtimeNow(); return *realtime; }
    const Timespec& Monotonic() { if (!monotonic) monotonic = monotonicNow(); return *monotonic; }
private:
    std::optional<DeadlineClock::time_point> steady;
    std::optional<Timespec> realtime, monotonic;
};
struct Deadline {
    std::optional<DeadlineClock::time_point> relative;
    std::int64_t seconds = 0, nanoseconds = 0;
    unsigned clock = realtimeClock; // Base of an absolute deadline.
    bool Expired(Clocks& now) const {
        if (relative) return *relative <= now.Steady();
        const auto& base = clock == monotonicClock ? now.Monotonic() : now.Realtime();
        // Compare the validated raw fields without multiplying guest seconds.
        // Absolute realtime waits therefore follow host wall-clock adjustments
        // and even INT64_MAX seconds cannot overflow a duration conversion.
        return seconds < base.seconds || (seconds == base.seconds && nanoseconds <= base.nanoseconds);
    }
};
Deadline relativeDeadline(std::uint32_t microseconds) {
    const auto now = DeadlineClock::now();
    const auto maximum = DeadlineClock::time_point::max();
    const auto roomMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(maximum - now).count();
    if (microseconds > static_cast<std::uint64_t>(roomMicroseconds)) return {maximum};
    return {now + std::chrono::duration_cast<DeadlineClock::duration>(
        std::chrono::microseconds(static_cast<std::int64_t>(microseconds)))};
}
// libthr accepts CLOCK_REALTIME, CLOCK_VIRTUAL, CLOCK_PROF and CLOCK_MONOTONIC.
// The two process CPU-time clocks have no guest clock here to measure a
// deadline against, so they are refused rather than silently mismeasured.
std::optional<unsigned> conditionClock(std::uint32_t id) {
    if (id == realtimeClock || id == monotonicClock) return id;
    return std::nullopt;
}
}
std::span<const KernelPrimitiveImport> KernelPrimitiveInventory() { return std::span(inventory).first(mutexCount); }
std::span<const KernelPrimitiveImport> KernelConditionInventory() {
    return std::span(inventory).subspan(mutexCount, conditionEnd - mutexCount);
}
struct KernelPrimitives::Impl {
    // Every object is keyed by its handle value. `home` is the slot that last
    // published it, used only to retire an idle object its own slot re-inits.
    struct Attribute { std::uint64_t home; unsigned type = 1; unsigned protocol = 0; };
    struct ConditionAttribute { std::uint64_t home; unsigned clock = realtimeClock; };
    struct Mutex {
        std::uint64_t token, home;
        unsigned type;
        unsigned protocol;
        // A non-robust owner that exits keeps ownership: lockers queue, trylock
        // and destroy report EBUSY, and the scheduler reports the hang.
        std::uint64_t owner = 0;
        unsigned depth = 0;
        std::deque<GuestThreadHandle> waiters;
        unsigned conditionUsers = 0;
    };
    struct Condition {
        std::uint64_t token, home;
        unsigned clock = realtimeClock;
        std::deque<GuestThreadHandle> waiters;
        unsigned users = 0; // Waiters still parked on this condition (not yet moved to the mutex).
    };
    struct ConditionWait {
        std::uint64_t conditionToken, mutexToken;
        unsigned mutexDepth;
        std::uint32_t result = 0;
        std::optional<Deadline> deadline;
        std::uint32_t timeoutResult = 0;
        bool transferred = false; // Signalled or expired; now waits only for the mutex.
    };
    struct MutexWait { std::uint64_t token; Deadline deadline; std::uint32_t timeoutResult; };
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::function<std::uint64_t()> active;
    std::uint64_t base;
    std::map<std::uint64_t, Attribute> attributes;
    std::map<std::uint64_t, Mutex> mutexes;
    std::map<std::uint64_t, Condition> conditions;
    std::map<std::uint64_t, ConditionAttribute> conditionAttributes;
    std::map<GuestThreadHandle, ConditionWait> conditionWaits;
    std::map<GuestThreadHandle, MutexWait> mutexWaits;
    std::map<Key, std::uint64_t> gates;
    std::optional<GuestThreads::WaitDomain> waits;
    Impl(Machine& m, std::function<std::uint64_t()> callback, std::uint64_t b)
        : machine(m), active(std::move(callback)), base(b) {
        if (!active) throw std::invalid_argument("Kernel primitives need active guest thread identity");
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("Invalid kernel primitive gate page");
        for (const auto& mapping : m.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("Kernel primitive gate page already mapped");
        std::array<std::byte, 4096> bytes; bytes.fill(std::byte{0xcc});
        m.Map(base, bytes.size(), rw);
        try { m.Write(base, bytes); m.Protect(base, bytes.size(), Permission::Read | Permission::Execute); }
        catch (...) { m.Unmap(base, bytes.size()); throw; }
    }
    ~Impl() {
        // Cancel pending calls before removing the page they would return from.
        if (waits) waits->Withdraw();
        machine.Unmap(base, 4096);
    }
    void stopped(GuestThreadHandle id) {
        releaseConditionBinding(id);
        mutexWaits.erase(id);
        for (auto& [token, state] : conditions) std::erase(state.waiters, id);
        for (auto& [token, state] : mutexes) {
            std::erase(state.waiters, id);
            // Non-robust owner exit keeps the dead owner; it can no longer
            // receive a priority donation.
            if (state.owner == id && state.protocol == 1 && waits) waits->SetInheritanceOwner(state.token, 0);
        }
    }
    void releaseConditionBinding(GuestThreadHandle id) {
        const auto binding = conditionWaits.find(id);
        if (binding == conditionWaits.end()) return;
        if (!binding->second.transferred)
            if (const auto condition = conditions.find(binding->second.conditionToken); condition != conditions.end())
                --condition->second.users;
        if (const auto mutex = mutexes.find(binding->second.mutexToken); mutex != mutexes.end())
            --mutex->second.conditionUsers;
        conditionWaits.erase(binding);
    }
    std::uint32_t completeCondition(GuestThreadHandle id) {
        const auto binding = conditionWaits.find(id);
        if (binding == conditionWaits.end())
            throw std::runtime_error("Kernel condition wait lost its original binding");
        const auto& original = binding->second;
        // The condition may already be destroyed and its memory reused: after
        // the transfer the waiter depends only on its reserved mutex ownership.
        const auto mutex = mutexes.find(original.mutexToken);
        if (!original.transferred || mutex == mutexes.end() || mutex->second.owner != id || mutex->second.depth != 1)
            throw std::runtime_error("Kernel condition wait reacquisition ownership rejected");
        mutex->second.depth = original.mutexDepth;
        const auto result = original.result;
        releaseConditionBinding(id);
        return result;
    }
    void wake(Mutex& state) {
        state.owner = 0;
        state.depth = 0;
        if (state.protocol == 1) waits->SetInheritanceOwner(state.token, 0);
        while (!state.waiters.empty()) {
            // Only suspended calls may acquire. Cancellation and failed
            // continuation validation must not retain a priority donation.
            if (waits) std::erase_if(state.waiters, [&](auto id) {
                if (waits->IsWaiting(id, state.token)) return false;
                mutexWaits.erase(id);
                return true;
            });
            if (state.waiters.empty()) break;
            auto selected = state.waiters.begin();
            if (state.protocol == 1) {
                selected = std::min_element(state.waiters.begin(), state.waiters.end(), [&](auto a, auto b) {
                    return waits->EffectivePriority(a) < waits->EffectivePriority(b);
                }); // min_element preserves FIFO for equal effective priority.
            }
            const auto id = *selected;
            state.waiters.erase(selected);
            mutexWaits.erase(id);
            // Reserve exclusive ownership before making the selected thread
            // runnable; a lower-priority contender cannot steal the transfer.
            state.owner = id;
            state.depth = 1;
            if (state.protocol == 1) waits->SetInheritanceOwner(state.token, id);
            if (waits) {
                const bool granted = conditionWaits.contains(id)
                    ? waits->Wake(id, state.token, [this, id] { return completeCondition(id); })
                    : waits->Wake(id, state.token, 0);
                if (granted) return;
            }
            state.owner = 0;
            state.depth = 0;
            if (state.protocol == 1) waits->SetInheritanceOwner(state.token, 0);
        }
    }
    std::uint64_t thread() {
        const auto id = active();
        if (!id) throw std::runtime_error("Kernel primitive requires an active guest thread");
        return id;
    }
    // Init overwrites the slot (libthr never reports EBUSY). The object the slot
    // last published is dropped only when idle; copies of it become stale.
    void retireMutex(std::uint64_t slot, std::uint64_t old) {
        const auto found = mutexes.find(old);
        if (found != mutexes.end() && found->second.home == slot && !found->second.owner &&
            found->second.waiters.empty() && !found->second.conditionUsers)
            mutexes.erase(found);
    }
    void retireCondition(std::uint64_t slot, std::uint64_t old) {
        const auto found = conditions.find(old);
        if (found != conditions.end() && found->second.home == slot && !found->second.users && found->second.waiters.empty())
            conditions.erase(found);
    }
    template<class Map> static void retireAttribute(Map& map, std::uint64_t slot, std::uint64_t old) {
        const auto found = map.find(old);
        if (found != map.end() && found->second.home == slot) map.erase(found);
    }
    void transferCondition(Condition& state, GuestThreadHandle id, std::uint32_t result) {
        const auto binding = conditionWaits.find(id);
        if (binding == conditionWaits.end() || binding->second.transferred || !waits->IsWaiting(id, state.token))
            throw std::runtime_error("Kernel condition transfer lost its exact parked wait");
        const auto mutex = mutexes.find(binding->second.mutexToken);
        if (mutex == mutexes.end())
            throw std::runtime_error("Kernel condition transfer original mutex identity rejected");
        auto& lock = mutex->second;
        lock.waiters.push_back(id);
        if (!waits->TransferWait(id, state.token, lock.token)) {
            lock.waiters.pop_back();
            throw std::runtime_error("Kernel condition transfer lost its exact parked wait");
        }
        std::erase(state.waiters, id);
        binding->second.result = result;
        binding->second.deadline.reset();
        binding->second.transferred = true;
        --state.users;
        // Reacquisition uses the actual mutex queue, priority inheritance,
        // and reserved ownership. No runnable result precedes ownership.
        if (!lock.owner) wake(lock);
    }
    bool pumpDeadlines() {
        // Visit only conditions with a parked timed waiter: idle and leaked
        // objects never cost a scheduler turn.
        std::set<std::uint64_t> timed;
        for (const auto& [id, binding] : conditionWaits)
            if (binding.deadline && !binding.transferred) timed.insert(binding.conditionToken);
        if (timed.empty() && mutexWaits.empty()) return false;
        Clocks now;
        bool pending = false;
        for (const auto token : timed) {
            const auto condition = conditions.find(token);
            if (condition == conditions.end()) continue;
            auto& state = condition->second;
            // Preserve condition FIFO selection when multiple deadlines expire
            // on the same owner turn; transfer removes exactly this queue entry.
            const auto parked = state.waiters;
            for (const auto id : parked) {
                const auto binding = conditionWaits.find(id);
                if (binding == conditionWaits.end() || !binding->second.deadline) continue;
                if (!waits->IsWaiting(id, state.token)) continue;
                if (binding->second.deadline->Expired(now))
                    transferCondition(state, id, binding->second.timeoutResult);
                else pending = true;
            }
        }
        for (auto wait = mutexWaits.begin(); wait != mutexWaits.end();) {
            const auto id = wait->first;
            const auto [token, deadline, result] = wait->second;
            if (!deadline.Expired(now)) { pending = true; ++wait; continue; }
            wait = mutexWaits.erase(wait);
            // Expire only the exact queued timed lock, never a later wait of the same thread.
            const auto mutex = mutexes.find(token);
            if (mutex != mutexes.end() && !conditionWaits.contains(id) && std::erase(mutex->second.waiters, id) &&
                waits->IsWaiting(id, token))
                waits->Wake(id, token, result);
        }
        return pending;
    }
    // Resolves a mutex handle for a condition wait: static initializers are
    // never owned (EPERM), the destroyed sentinel and forged values are EINVAL.
    std::variant<std::uint32_t, Mutex*> ownedMutex(std::uint64_t pointer) {
        const auto token = read(machine, pointer);
        if (token < mutexDestroyed) return error(Perm);
        const auto mutex = mutexes.find(token);
        if (mutex == mutexes.end()) return error(Invalid);
        if (mutex->second.owner != thread()) return error(Perm);
        return &mutex->second;
    }
    std::uint32_t conditionInvoke(unsigned op, std::uint64_t slot, std::uint64_t arg, std::uint64_t label,
                                  std::optional<Deadline> deadline = {}, std::uint32_t timeoutResult = 0) {
        if (op == 5) {
            span(machine, slot, Permission::Write);
            const auto old = read(machine, slot);
            const auto token = nextToken.fetch_add(1);
            conditionAttributes.emplace(token, ConditionAttribute{slot});
            try { write(machine, slot, token); }
            catch (...) { conditionAttributes.erase(token); throw; }
            retireAttribute(conditionAttributes, slot, old);
            return 0;
        }
        if (op == 6 || op == 7) {
            const auto attr = conditionAttributes.find(read(machine, slot));
            if (attr == conditionAttributes.end()) return error(Invalid);
            if (op == 7) {
                const auto clock = conditionClock(static_cast<std::uint32_t>(arg));
                if (!clock) return error(Invalid);
                attr->second.clock = *clock;
                return 0;
            }
            span(machine, slot, Permission::Write);
            write(machine, slot, 0);
            conditionAttributes.erase(attr);
            return 0;
        }
        if (op == 0) {
            span(machine, slot, Permission::Write);
            unsigned clock = realtimeClock;
            if (arg) {
                const auto attr = conditionAttributes.find(read(machine, arg));
                if (attr == conditionAttributes.end()) return error(Invalid);
                clock = attr->second.clock;
            }
            name(machine, label);
            const auto old = read(machine, slot);
            const auto token = nextToken.fetch_add(1);
            conditions.emplace(token, Condition{token, slot, clock, {}, 0});
            try { write(machine, slot, token); }
            catch (...) { conditions.erase(token); throw; }
            retireCondition(slot, old);
            return 0;
        }
        const auto token = read(machine, slot);
        if (token == conditionDestroyed) return error(Invalid);
        std::map<std::uint64_t, Condition>::iterator condition;
        if (token == 0) {
            // Static initializer: destroy preserves it, every other use lazily
            // initializes. A rejected wait argument leaves the zero slot intact.
            if (op == 1) return 0;
            if (!waits) throw std::runtime_error("Kernel conditions require the actual guest scheduler");
            if (op == 2) {
                const auto owned = ownedMutex(arg);
                if (const auto rejected = std::get_if<std::uint32_t>(&owned)) return *rejected;
            }
            span(machine, slot, Permission::Write);
            const auto created = nextToken.fetch_add(1);
            condition = conditions.emplace(created, Condition{created, slot, realtimeClock, {}, 0}).first;
            try { write(machine, slot, created); }
            catch (...) { conditions.erase(condition); throw; }
        } else {
            condition = conditions.find(token);
            if (condition == conditions.end()) return error(Invalid);
        }
        auto& state = condition->second;
        if (op == 1) {
            if (state.users || !state.waiters.empty()) return error(Busy);
            span(machine, slot, Permission::Write);
            write(machine, slot, conditionDestroyed);
            conditions.erase(condition);
            return 0;
        }
        if (!waits) throw std::runtime_error("Kernel conditions require the actual guest scheduler");
        if (op == 2) {
            const auto owned = ownedMutex(arg);
            if (const auto rejected = std::get_if<std::uint32_t>(&owned)) return *rejected;
            auto& lock = *std::get<Mutex*>(owned);
            const auto id = thread();
            for (const auto& [waiter, binding] : conditionWaits)
                if (!binding.transferred && binding.conditionToken == state.token && binding.mutexToken != lock.token)
                    return error(Invalid);
            // An absolute deadline is measured on the condition's own clock.
            if (deadline && !deadline->relative) deadline->clock = state.clock;
            conditionWaits.emplace(id, ConditionWait{state.token, lock.token, lock.depth, 0, deadline, timeoutResult});
            try { state.waiters.push_back(id); }
            catch (...) { conditionWaits.erase(id); throw; }
            ++state.users;
            ++lock.conditionUsers;
            try { waits->BlockFromHostCall(state.token); }
            catch (...) { state.waiters.pop_back(); releaseConditionBinding(id); throw; }
            // One owner executes host gates serially: publication + suspension
            // + full recursive release is atomic with respect to every guest
            // signaler. The next guest slice cannot precede this release.
            wake(lock);
            return 0;
        }
        if (op != 3 && op != 4) throw std::runtime_error("Unknown kernel condition operation");
        while (!state.waiters.empty()) {
            const auto id = state.waiters.front();
            const auto binding = conditionWaits.find(id);
            if (binding == conditionWaits.end() || !waits->IsWaiting(id, state.token)) {
                state.waiters.pop_front();
                continue;
            }
            transferCondition(state, id, 0);
            if (op == 3) break; // Explicit condition order is FIFO, independent of mutex priority order.
        }
        return 0;
    }
    // Mutex ops: 0 init, 1 trylock, 2 unlock, 3 destroy, 4 attr init, 5 settype,
    // 6 attr destroy, 7 setprotocol, 8 lock, 9 timed lock.
    std::uint32_t invoke(unsigned op, std::uint64_t slot, std::uint64_t arg, std::uint64_t label,
                         std::optional<Deadline> deadline = {}) {
        if (op == 4) {
            span(machine, slot, Permission::Write);
            const auto old = read(machine, slot);
            const auto token = nextToken.fetch_add(1);
            attributes.emplace(token, Attribute{slot});
            try { write(machine, slot, token); }
            catch (...) { attributes.erase(token); throw; }
            retireAttribute(attributes, slot, old);
            return 0;
        }
        if (op == 5 || op == 6 || op == 7) {
            const auto attr = attributes.find(read(machine, slot));
            if (attr == attributes.end()) return error(Invalid);
            if (op == 5) {
                const auto type = static_cast<std::uint32_t>(arg);
                if (type < 1 || type > 4) return error(Invalid);
                attr->second.type = type; return 0;
            }
            if (op == 7) {
                const auto protocol = static_cast<std::uint32_t>(arg);
                if (protocol > 2) return error(Invalid);
                if (protocol == 2) throw std::runtime_error("Unsupported kernel mutex priority protocol2 protection");
                if (protocol == 1 && !waits)
                    throw std::runtime_error("Unsupported kernel mutex priority protocol1 inheritance: guest scheduler integration required");
                attr->second.protocol = protocol;
                return 0;
            }
            span(machine, slot, Permission::Write); write(machine, slot, 0); attributes.erase(attr); return 0;
        }
        if (op == 0) {
            span(machine, slot, Permission::Write);
            unsigned type = 1, protocol = 0;
            if (arg) {
                // A non-null attribute must name a live attribute; zero,
                // destroyed and forged values are EINVAL, never the default.
                const auto attr = attributes.find(read(machine, arg));
                if (attr == attributes.end()) return error(Invalid);
                type = attr->second.type;
                protocol = attr->second.protocol;
            }
            name(machine, label);
            const auto old = read(machine, slot);
            const auto token = nextToken.fetch_add(1);
            mutexes.emplace(token, Mutex{token, slot, type, protocol, 0, 0, {}, 0});
            try { write(machine, slot, token); }
            catch (...) { mutexes.erase(token); throw; }
            retireMutex(slot, old);
            return 0;
        }
        const auto token = read(machine, slot);
        if (token == mutexDestroyed) return error(Invalid);
        std::map<std::uint64_t, Mutex>::iterator mutex;
        if (token < mutexDestroyed) {
            // Static initializers 0 (default) and 1 (adaptive): destroy keeps the
            // slot, unlock has no owner, acquisition lazily initializes.
            if (op == 3) return 0;
            if (op == 2) return error(Perm);
            thread(); // Validate scheduler identity before publishing a lazy initializer.
            span(machine, slot, Permission::Write);
            const auto created = nextToken.fetch_add(1);
            mutex = mutexes.emplace(created, Mutex{created, slot, token == 1 ? 4u : 1u, 0, 0, 0, {}, 0}).first;
            try { write(machine, slot, created); }
            catch (...) { mutexes.erase(mutex); throw; }
        } else {
            mutex = mutexes.find(token);
            if (mutex == mutexes.end()) return error(Invalid);
        }
        auto& state = mutex->second;
        if (op == 3) {
            if (state.owner || !state.waiters.empty() || state.conditionUsers) return error(Busy);
            span(machine, slot, Permission::Write);
            write(machine, slot, mutexDestroyed);
            mutexes.erase(mutex); return 0;
        }
        const auto id = thread();
        if (op == 2) {
            if (state.owner != id) return error(Perm);
            if (--state.depth == 0) wake(state);
            return 0;
        }
        if (state.owner) {
            if (state.owner == id && state.type == 2) {
                if (state.depth == std::numeric_limits<int>::max()) return error(Again);
                ++state.depth; return 0;
            }
            if (op == 1) return error(Busy);
            if (state.owner == id && (state.type == 1 || state.type == 4)) return error(Deadlock);
            if (deadline) { Clocks now; if (deadline->Expired(now)) return error(TimedOut); }
            if (!waits)
                throw std::runtime_error("Unsupported contended kernel mutex lock: guest wait/wake scheduler integration required");
            state.waiters.push_back(id);
            try {
                if (deadline) mutexWaits.insert_or_assign(id, MutexWait{state.token, *deadline, error(TimedOut)});
                waits->BlockFromHostCall(state.token);
            }
            catch (...) { state.waiters.pop_back(); mutexWaits.erase(id); throw; }
            return 0; // Suspended call is completed by the scheduler only after wake or expiry.
        }
        state.owner = id; state.depth = 1;
        if (state.protocol == 1) waits->SetInheritanceOwner(state.token, id);
        return 0;
    }
};
KernelPrimitives::KernelPrimitives(Machine& m, std::function<std::uint64_t()> active, std::uint64_t b)
    : impl(std::make_shared<Impl>(m, std::move(active), b)) {}
KernelPrimitives::KernelPrimitives(Machine& m, const std::shared_ptr<GuestThreads>& threads, std::uint64_t b)
    : KernelPrimitives(m, [weak = std::weak_ptr<GuestThreads>(threads)] {
        const auto runtime = weak.lock();
        if (!runtime) throw std::runtime_error("Kernel mutex guest scheduler expired");
        return runtime->ActiveThread();
    }, b) {
    if (!threads) throw std::invalid_argument("Kernel mutexes need a guest scheduler");
    impl->waits.emplace(threads->CreateWaitDomain(m, [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle id) {
        if (const auto provider = weak.lock()) provider->stopped(id);
    }));
    impl->waits->SetOwnerPump([weak = std::weak_ptr<Impl>(impl)] {
        const auto provider = weak.lock();
        return provider && provider->pumpDeadlines();
    });
}
KernelPrimitives::~KernelPrimitives() = default;
std::optional<std::uint64_t> KernelPrimitives::Resolve(const SceImport& import, std::uint8_t type) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size()) return std::nullopt;
    if ((import.LibraryName != "libkernel" && !(posixRow(op) && import.LibraryName == "libScePosix")) ||
        import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 || type != 2)
        throw std::runtime_error("Unsupported kernel primitive scope/version/type: " + import.Nid);
    const Impl::Key key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() >= 256) throw std::runtime_error("Kernel primitive gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}}; impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [weak = std::weak_ptr<Impl>(impl), op](Machine& m) {
        auto state = weak.lock(); if (!state) throw std::runtime_error("Kernel primitive provider expired");
        const auto rdi = m.Get(Register::Rdi), rsi = m.Get(Register::Rsi), rdx = m.Get(Register::Rdx);
        std::uint32_t result;
        if (op == timeoutBegin) {
            if (!rdx) {
                m.Set(Register::Rax, static_cast<std::uint32_t>(Invalid));
                return;
            }
            if (rdx > std::numeric_limits<std::uint64_t>::max() - 16)
                throw std::runtime_error("Invalid kernel condition timespec span");
            m.CheckAccess(rdx, 16, Permission::Read);
            std::array<std::int64_t, 2> time;
            m.Read(rdx, std::as_writable_bytes(std::span(time)));
            if (time[1] < 0 || time[1] >= 1000000000) {
                m.Set(Register::Rax, static_cast<std::uint32_t>(Invalid));
                return;
            }
            // The condition's clock is applied once the condition is resolved.
            result = state->conditionInvoke(2, rdi, rsi, 0, Deadline{{}, time[0], time[1]},
                                            static_cast<std::uint32_t>(TimedOut));
        } else if (op == timeoutBegin + 1) {
            result = state->conditionInvoke(2, rdi, rsi, 0, relativeDeadline(static_cast<std::uint32_t>(rdx)),
                                            error(TimedOut));
        } else if (op == mutexTimedlock) {
            result = state->invoke(9, rdi, 0, 0, relativeDeadline(static_cast<std::uint32_t>(rsi)));
        } else if (op == conditionSetclock || op == posixConditionSetclock) {
            result = state->conditionInvoke(7, rdi, rsi, 0);
        } else if (mutexRow(op)) {
            result = state->invoke(op < mutexCount ? op : (op == conditionEnd ? 8 : 2), rdi, rsi, rdx);
        } else {
            result = state->conditionInvoke(op < 16 ? op - mutexCount : op - 16, rdi, rsi, op < 16 ? rdx : 0);
        }
        if (posixRow(op) && result >= 0x80020000u) result -= 0x80020000u;
        m.Set(Register::Rax, result);
    });
    impl->gates.emplace(key, gate); return gate;
}

TargetKernelMutexes::TargetKernelMutexes(Machine& machine, const std::shared_ptr<GuestThreads>& threads)
    : provider(machine, threads) {}
std::optional<std::uint64_t> TargetKernelMutexes::Resolve(const SceImport& import, std::uint8_t type) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size() || !mutexRow(op)) return std::nullopt;
    // The provider validates libkernel/libScePosix scope, version and symbol type
    // for every importing image; consumer identity is not an admission input.
    return provider.Resolve(import, type);
}
std::optional<std::uint64_t> TargetKernelMutexes::ResolveCondition(const SceImport& import, std::uint8_t type,
                                                                 std::uint64_t size) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size() || mutexRow(op)) return std::nullopt;
    if (size || type != 2)
        throw std::runtime_error("Unsupported target kernel condition symbol type/size: " + import.Nid);
    return provider.Resolve(import, type);
}
}
