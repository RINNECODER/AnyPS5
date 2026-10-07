#include "KernelPrimitives.hpp"
#include <cpu/SceElf.hpp>
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Cpu::Platform {
namespace {
constexpr unsigned mutexCount = 9;
constexpr unsigned conditionEnd = 21;
constexpr std::array<KernelPrimitiveImport, 23> inventory{{
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
    {"7H0iTOciTLo", "pthread_mutex_lock"}, {"2Z+PpY6CaJg", "pthread_mutex_unlock"}}};
std::atomic<std::uint64_t> nextToken{0xa005000000000003ULL};
constexpr auto rw = Permission::Read | Permission::Write;
// Guest Orbis error table, independent of the host's errno numerals.
enum GuestErrno : unsigned { Perm = 1, Deadlock = 11, Busy = 16, Invalid = 22, Again = 35 };
std::uint32_t error(GuestErrno posix) { return 0x80020000u + posix; }
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
}
std::span<const KernelPrimitiveImport> KernelPrimitiveInventory() { return std::span(inventory).first(mutexCount); }
std::span<const KernelPrimitiveImport> KernelConditionInventory() {
    return std::span(inventory).subspan(mutexCount, conditionEnd - mutexCount);
}
struct KernelPrimitives::Impl {
    struct Attribute { std::uint64_t token; unsigned type = 1; unsigned protocol = 0; };
    struct Mutex {
        std::uint64_t token;
        unsigned type;
        unsigned protocol;
        std::uint64_t owner = 0;
        unsigned depth = 0;
        bool abandoned = false;
        std::deque<GuestThreadHandle> waiters;
        unsigned conditionUsers = 0;
    };
    struct Condition {
        std::uint64_t token;
        std::deque<GuestThreadHandle> waiters;
        unsigned users = 0;
    };
    struct ConditionWait {
        std::uint64_t conditionSlot, conditionToken, mutexSlot, mutexToken;
        unsigned mutexDepth;
    };
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::function<std::uint64_t()> active;
    std::uint64_t base;
    std::map<std::uint64_t, Attribute> attributes;
    std::map<std::uint64_t, Mutex> mutexes;
    std::set<std::uint64_t> destroyedMutexSlots;
    std::map<std::uint64_t, Condition> conditions;
    std::set<std::uint64_t> destroyedConditionSlots;
    std::map<std::uint64_t, std::uint64_t> conditionAttributes;
    std::map<GuestThreadHandle, ConditionWait> conditionWaits;
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
        for (auto& [slot, state] : conditions) std::erase(state.waiters, id);
        for (auto& [slot, state] : mutexes) {
            std::erase(state.waiters, id);
            // Nonrobust owner death must never silently grant another owner.
            if (state.owner == id) {
                state.abandoned = true;
                if (state.protocol == 1 && waits) waits->SetInheritanceOwner(state.token, 0);
            }
        }
    }
    void releaseConditionBinding(GuestThreadHandle id) {
        const auto binding = conditionWaits.find(id);
        if (binding == conditionWaits.end()) return;
        const auto condition = conditions.find(binding->second.conditionSlot);
        if (condition != conditions.end() && condition->second.token == binding->second.conditionToken)
            --condition->second.users;
        const auto mutex = mutexes.find(binding->second.mutexSlot);
        if (mutex != mutexes.end() && mutex->second.token == binding->second.mutexToken)
            --mutex->second.conditionUsers;
        conditionWaits.erase(binding);
    }
    std::uint32_t completeCondition(GuestThreadHandle id) {
        const auto binding = conditionWaits.find(id);
        if (binding == conditionWaits.end())
            throw std::runtime_error("Kernel condition wait lost its original binding");
        const auto& original = binding->second;
        const auto condition = conditions.find(original.conditionSlot);
        const auto mutex = mutexes.find(original.mutexSlot);
        // The scheduler already validated the suspended gate. Validate both
        // original opaque slots and the reserved owner before returning Wait.
        if (condition == conditions.end() || condition->second.token != original.conditionToken ||
            read(machine, original.conditionSlot) != original.conditionToken ||
            mutex == mutexes.end() || mutex->second.token != original.mutexToken ||
            read(machine, original.mutexSlot) != original.mutexToken ||
            mutex->second.abandoned || mutex->second.owner != id || mutex->second.depth != 1)
            throw std::runtime_error("Kernel condition wait reacquisition identity/ownership rejected");
        mutex->second.depth = original.mutexDepth;
        releaseConditionBinding(id);
        return 0;
    }
    void wake(Mutex& state) {
        state.owner = 0;
        state.depth = 0;
        if (state.protocol == 1) waits->SetInheritanceOwner(state.token, 0);
        while (!state.waiters.empty()) {
            // Only suspended calls may acquire. Cancellation and failed
            // continuation validation must not retain a priority donation.
            if (waits) std::erase_if(state.waiters, [&](auto id) {
                return !waits->IsWaiting(id, state.token);
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
    std::uint32_t conditionInvoke(unsigned op, std::uint64_t slot, std::uint64_t arg, std::uint64_t label) {
        if (op == 5) {
            span(machine, slot, Permission::Write);
            if (conditionAttributes.contains(slot)) return error(Busy);
            const auto token = nextToken.fetch_add(1);
            conditionAttributes.emplace(slot, token);
            write(machine, slot, token);
            return 0;
        }
        if (op == 6) {
            const auto token = read(machine, slot);
            const auto attr = conditionAttributes.find(slot);
            if (attr == conditionAttributes.end() || attr->second != token) return error(Invalid);
            span(machine, slot, Permission::Write);
            write(machine, slot, 0);
            conditionAttributes.erase(attr);
            return 0;
        }
        if (op == 0) {
            span(machine, slot, Permission::Write);
            if (conditions.contains(slot)) return error(Busy);
            if (arg) {
                const auto token = read(machine, arg);
                const auto attr = conditionAttributes.find(arg);
                if (attr == conditionAttributes.end() || attr->second != token) return error(Invalid);
            }
            name(machine, label);
            const auto token = nextToken.fetch_add(1);
            conditions.emplace(slot, Condition{token, {}, 0});
            write(machine, slot, token);
            destroyedConditionSlots.erase(slot);
            return 0;
        }
        const auto token = read(machine, slot);
        auto condition = conditions.find(slot);
        if (condition != conditions.end() && condition->second.token != token) return error(Invalid);
        if (condition == conditions.end() && destroyedConditionSlots.contains(slot)) return error(Invalid);
        if (op == 1 && token == 0) return 0; // Pinned public static initializer destroy preserves zero.
        if (token == 1) return error(Invalid); // Condition destroyed sentinel; mutexes retain their own sentinel 2.
        if (token == 0 && (op == 2 || op == 3 || op == 4)) {
            if (!waits) throw std::runtime_error("Kernel conditions require the actual guest scheduler");
            if (op == 2) {
                // Validate the existing owned mutex before publishing a static
                // condition. A malformed argument must leave its zero slot intact.
                const auto mutexToken = read(machine, arg);
                const auto mutex = mutexes.find(arg);
                if (mutex == mutexes.end() || mutex->second.token != mutexToken) return error(Invalid);
                const auto id = thread();
                if (mutex->second.abandoned)
                    throw std::runtime_error("Unsupported nonrobust condition mutex owner-exit recovery");
                if (mutex->second.owner != id) return error(Perm);
            }
            span(machine, slot, Permission::Write);
            const auto created = nextToken.fetch_add(1);
            condition = conditions.emplace(slot, Condition{created, {}, 0}).first;
            try { write(machine, slot, created); }
            catch (...) { conditions.erase(condition); throw; }
        } else if (condition == conditions.end() || condition->second.token != token) return error(Invalid);
        auto& state = condition->second;
        if (op == 1) {
            if (state.users) return error(Busy);
            span(machine, slot, Permission::Write);
            destroyedConditionSlots.insert(slot);
            write(machine, slot, 1);
            conditions.erase(condition);
            return 0;
        }
        if (!waits) throw std::runtime_error("Kernel conditions require the actual guest scheduler");
        if (op == 2) {
            const auto mutexToken = read(machine, arg);
            const auto mutex = mutexes.find(arg);
            if (mutex == mutexes.end() || mutex->second.token != mutexToken) return error(Invalid);
            auto& lock = mutex->second;
            const auto id = thread();
            if (lock.abandoned) throw std::runtime_error("Unsupported nonrobust condition mutex owner-exit recovery");
            if (lock.owner != id) return error(Perm);
            for (const auto& [waiter, binding] : conditionWaits)
                if (binding.conditionToken == state.token && binding.mutexToken != mutexToken) return error(Invalid);
            conditionWaits.emplace(id, ConditionWait{slot, state.token, arg, mutexToken, lock.depth});
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
            const auto mutex = mutexes.find(binding->second.mutexSlot);
            if (mutex == mutexes.end() || mutex->second.token != binding->second.mutexToken ||
                read(machine, binding->second.mutexSlot) != binding->second.mutexToken || mutex->second.abandoned)
                throw std::runtime_error("Kernel condition signal original mutex identity rejected");
            auto& lock = mutex->second;
            lock.waiters.push_back(id);
            if (!waits->TransferWait(id, state.token, lock.token)) {
                lock.waiters.pop_back();
                throw std::runtime_error("Kernel condition signal lost its exact parked wait");
            }
            state.waiters.pop_front();
            // Reacquisition uses the actual mutex queue, priority inheritance,
            // and reserved ownership. No runnable success precedes ownership.
            if (!lock.owner) wake(lock);
            if (op == 3) break; // Explicit condition order is FIFO, independent of mutex priority order.
        }
        return 0;
    }
    std::uint32_t invoke(unsigned op, std::uint64_t slot, std::uint64_t arg, std::uint64_t label) {
        if (op == 4) {
            span(machine, slot, Permission::Write);
            if (attributes.contains(slot)) return error(Busy);
            auto token = nextToken.fetch_add(1);
            attributes.emplace(slot, Attribute{token}); write(machine, slot, token); return 0;
        }
        if (op == 5 || op == 6 || op == 7) {
            const auto token = read(machine, slot);
            auto attr = attributes.find(slot);
            if (attr == attributes.end() || attr->second.token != token) return error(Invalid);
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
            if (mutexes.contains(slot)) return error(Busy);
            unsigned type = 1, protocol = 0;
            if (arg) {
                const auto token = read(machine, arg); auto attr = attributes.find(arg);
                if (token) {
                    if (attr == attributes.end() || attr->second.token != token) return error(Invalid);
                    type = attr->second.type;
                    protocol = attr->second.protocol;
                }
            }
            name(machine, label);
            auto token = nextToken.fetch_add(1);
            const auto created = mutexes.emplace(slot, Mutex{token, type, protocol, 0, 0, false, {}}).first;
            try { write(machine, slot, token); }
            catch (...) { mutexes.erase(created); throw; }
            destroyedMutexSlots.erase(slot);
            return 0;
        }
        const auto token = read(machine, slot);
        auto mutex = mutexes.find(slot);
        // A live slot overwritten with a static value must not create a second
        // identity or pass static destroy/unlock shortcuts. Destroyed identities
        // require explicit Init, even if the guest overwrites the sentinel.
        if (mutex != mutexes.end() && mutex->second.token != token) return error(Invalid);
        if (destroyedMutexSlots.contains(slot)) return error(Invalid);
        if (op == 3 && token < 2) return 0; // Upstream static initializer destroy preserves the slot.
        if (token == 2) return error(Invalid);
        if (op == 2 && token < 2) return error(Perm);
        if (token < 2 && (op == 1 || op == 8)) {
            thread(); // Validate scheduler identity before publishing a lazy initializer.
            span(machine, slot, Permission::Write);
            auto created = nextToken.fetch_add(1);
            mutex = mutexes.emplace(slot, Mutex{created, token == 1 ? 4u : 1u, 0, 0, 0, false, {}}).first;
            try { write(machine, slot, created); }
            catch (...) { mutexes.erase(mutex); throw; }
        } else if (mutex == mutexes.end() || mutex->second.token != token) return error(Invalid);
        auto& state = mutex->second;
        if (state.abandoned)
            throw std::runtime_error("Unsupported nonrobust kernel mutex owner-exit recovery");
        if (op == 3) {
            if (state.owner || !state.waiters.empty() || state.conditionUsers) return error(Busy);
            span(machine, slot, Permission::Write);
            const auto [destroyed, inserted] = destroyedMutexSlots.insert(slot);
            try { write(machine, slot, 2); }
            catch (...) { if (inserted) destroyedMutexSlots.erase(destroyed); throw; }
            if (state.protocol == 1) waits->SetInheritanceOwner(state.token, 0);
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
            if (!waits)
                throw std::runtime_error("Unsupported contended kernel mutex lock: guest wait/wake scheduler integration required");
            state.waiters.push_back(id);
            try { waits->BlockFromHostCall(state.token); }
            catch (...) { state.waiters.pop_back(); throw; }
            return 0; // Suspended call is completed by the scheduler only after wake.
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
}
KernelPrimitives::~KernelPrimitives() = default;
std::optional<std::uint64_t> KernelPrimitives::Resolve(const SceImport& import, std::uint8_t type) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size()) return std::nullopt;
    const bool posix = op >= 16;
    if ((import.LibraryName != "libkernel" && !(posix && import.LibraryName == "libScePosix")) ||
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
        auto result = op < mutexCount || op >= conditionEnd
            ? state->invoke(op < mutexCount ? op : (op == conditionEnd ? 8 : 2),
                m.Get(Register::Rdi), m.Get(Register::Rsi), m.Get(Register::Rdx))
            : state->conditionInvoke(op < 16 ? op - mutexCount : op - 16,
                m.Get(Register::Rdi), m.Get(Register::Rsi), op < 16 ? m.Get(Register::Rdx) : 0);
        if (op >= 16 && result >= 0x80020000u) result -= 0x80020000u;
        m.Set(Register::Rax, result);
    });
    impl->gates.emplace(key, gate); return gate;
}

TargetKernelMutexes::TargetKernelMutexes(Machine& machine, const std::shared_ptr<GuestThreads>& threads)
    : provider(machine, threads) {}
std::optional<std::uint64_t> TargetKernelMutexes::Resolve(const SceImport& import, std::uint8_t type,
                                                        KernelMutexConsumer source) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size() || (op >= mutexCount && op < conditionEnd)) return std::nullopt;
    const bool eboot = source.Name == "eboot.bin" && source.Sha256 ==
        "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397";
    const bool libc = source.Name == "libc.prx" && source.Sha256 ==
        "78a080fdeccc28f2aa76356e97f82a35b3ba09deba8408dfce27db28fa0ce67f";
    const bool web = source.Name == "libSceNpCppWebApi.prx" && source.Sha256 ==
        "38db047fd9dfd27fc17dfc0dd2cff31a2e0533ac1be2350e5082f8499f59c6b9";
    if (op >= conditionEnd) {
        // Only this pinned libc imports the POSIX pair used around its shared
        // zero-filled static mutex/condition slots. The nine SCE rows stay intact.
        if (!libc || import.LibraryName != "libkernel" || import.LibraryId != 0 || import.ModuleId != 1)
            throw std::runtime_error("Unsupported target POSIX static mutex consumer source/import row");
        return provider.Resolve(import, type);
    }
    if (!eboot && !(libc && op != 7) && !(web && op != 1 && op != 7))
        throw std::runtime_error("Unsupported target kernel mutex consumer source/import row");
    if ((eboot && (import.LibraryId != 44 || import.ModuleId != 24)) ||
        (libc && (import.LibraryId != 0 || import.ModuleId != 1)) ||
        (web && (import.LibraryId != 4 || import.ModuleId != 5)))
        throw std::runtime_error("Unsupported target kernel mutex consumer source/import row IDs");
    return provider.Resolve(import, type);
}
std::optional<std::uint64_t> TargetKernelMutexes::ResolveCondition(const SceImport& import, std::uint8_t type,
                                                                 std::uint64_t size, KernelMutexConsumer source) {
    unsigned op = mutexCount;
    for (; op < conditionEnd; ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == conditionEnd) return std::nullopt;
    const bool eboot = source.Name == "eboot.bin" && source.Sha256 ==
        "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397";
    const bool libc = source.Name == "libc.prx" && source.Sha256 ==
        "78a080fdeccc28f2aa76356e97f82a35b3ba09deba8408dfce27db28fa0ce67f";
    const bool web = source.Name == "libSceNpCppWebApi.prx" && source.Sha256 ==
        "38db047fd9dfd27fc17dfc0dd2cff31a2e0533ac1be2350e5082f8499f59c6b9";
    const bool core = op < 14;
    const bool attrs = op == 14 || op == 15;
    const bool posix = op >= 16;
    if (size || type != 2 || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 ||
        !(eboot || (libc && (core || op == 18 || op == 20)) || (web && core)) ||
        (attrs && !eboot) ||
        (eboot && (import.LibraryId != (posix ? 43 : 44) || import.ModuleId != 24 ||
                   import.LibraryName != (posix ? "libScePosix" : "libkernel"))) ||
        (libc && (import.LibraryId != 0 || import.ModuleId != 1 || import.LibraryName != "libkernel")) ||
        (web && (import.LibraryId != 4 || import.ModuleId != 5 || import.LibraryName != "libkernel")))
        throw std::runtime_error("Unsupported target kernel condition consumer source/import row");
    return provider.Resolve(import, type);
}
}
