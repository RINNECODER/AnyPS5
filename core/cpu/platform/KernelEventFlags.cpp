#include "KernelEventFlags.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace Cpu::Platform {
namespace {
constexpr std::array<KernelEventFlagImport, 7> inventory{{
    {"BpFoboUJoZU", "sceKernelCreateEventFlag"}, {"8mql9OcQnd4", "sceKernelDeleteEventFlag"},
    {"IOnSvHzqu6A", "sceKernelSetEventFlag"}, {"7uhBFWRAS60", "sceKernelClearEventFlag"},
    {"JTvBflhYazQ", "sceKernelWaitEventFlag"}, {"9lvj5DjHZiA", "sceKernelPollEventFlag"},
    {"PZku4ZrXJqg", "sceKernelCancelEventFlag"}}};
enum Op : unsigned { Create, Delete, Set, Clear, Wait, Poll, Cancel };
// Semantics follow the public upstream EventFlag.cpp and shadPS4 event_flag.cpp.
constexpr std::uint32_t Perm = 0x80020001u, Missing = 0x80020003u, Deleted = 0x8002000du,
                        Fault = 0x8002000eu, Busy = 0x80020010u, Invalid = 0x80020016u,
                        TimedOut = 0x8002003cu, Canceled = 0x80020055u;
constexpr std::uint32_t AttrFifo = 0x01, AttrPriority = 0x02, AttrSingle = 0x10, AttrMulti = 0x20;
constexpr std::uint32_t ModeAnd = 0x01, ModeOr = 0x02, ClearAll = 0x10, ClearPattern = 0x20;
constexpr std::size_t MaxFlags = 65535;
using Clock = std::chrono::steady_clock;
// Process-wide nonrecycled numeric identities, distinct from semaphores and
// equeues; no identity contains a host address.
std::atomic<std::uint64_t> nextIdentity{0x0c0f000000000001ULL};
std::uint64_t identity() {
    auto candidate = nextIdentity.load();
    for (;;) {
        if (!candidate || candidate >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("Kernel event flag identity exhausted");
        if (nextIdentity.compare_exchange_weak(candidate, candidate + 1)) return candidate;
    }
}
unsigned operation(const SceImport& import) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    return op;
}
void scope(const SceImport& import, std::uint8_t type, std::uint64_t size) {
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 || type != 2 || size != 0)
        throw std::runtime_error("Unsupported kernel event flag scope/version/type/size: " + import.Nid);
}
bool accessible(Machine& machine, std::uint64_t address, std::uint64_t size, Permission permission) {
    if (!address || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
    try { machine.CheckAccess(address, size, permission); return true; }
    catch (const std::exception&) { return false; }
}
template<class T> void write(Machine& machine, std::uint64_t address, const T& value) {
    machine.CheckAccess(address, sizeof(T), Permission::Write);
    machine.Write(address, std::as_bytes(std::span(&value, 1)));
}
bool validName(Machine& machine, std::uint64_t address) {
    // Diagnostic bounded scan, not a claimed vendor name-length limit.
    for (std::uint64_t offset = 0; offset < 4096; ++offset) {
        if (!accessible(machine, address + offset, 1, Permission::Read)) return false;
        std::byte byte{}; machine.Read(address + offset, std::span(&byte, 1));
        if (byte == std::byte{}) return true;
    }
    return false;
}
bool validMode(std::uint32_t mode) {
    const auto match = mode & (ModeAnd | ModeOr), clear = mode & (ClearAll | ClearPattern);
    return !(mode & ~(ModeAnd | ModeOr | ClearAll | ClearPattern)) && (match == ModeAnd || match == ModeOr) &&
           clear != (ClearAll | ClearPattern);
}
bool satisfied(std::uint64_t pattern, std::uint64_t bits, std::uint32_t mode) {
    return (mode & ModeAnd) ? (pattern & bits) == bits : (pattern & bits) != 0;
}
}
std::span<const KernelEventFlagImport> KernelEventFlagInventory() { return inventory; }

struct KernelEventFlags::Impl : std::enable_shared_from_this<KernelEventFlags::Impl> {
    struct Waiter {
        GuestThreadHandle thread;
        std::uint64_t key, bits, result, timeout;
        std::uint32_t mode;
        std::int32_t priority;
        std::optional<Clock::time_point> deadline;
        bool selected = false;
        std::uint32_t code = 0;
        std::uint64_t observed = 0;
        std::uint64_t cleared = 0; // bits this waiter's clear mode removed at selection
    };
    struct Flag {
        bool live = true, multi = false, fifo = true;
        std::uint64_t pattern = 0;
        std::vector<std::shared_ptr<Waiter>> waiting;
    };
    struct Pending { std::shared_ptr<Flag> flag; std::shared_ptr<Waiter> wait; };
    using GateKey = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    std::optional<GuestThreads::WaitDomain> waits;
    std::map<std::uint64_t, std::shared_ptr<Flag>> flags;
    std::map<std::uint64_t, Pending> pending;
    std::map<GateKey, std::uint64_t> gates;
    // Flags that regained bits from a selected waiter that stopped before returning.
    std::vector<std::shared_ptr<Flag>> refunded;
    std::uint64_t base, nextWait = 1;
    bool live = true;

    Impl(Machine& m, const std::shared_ptr<GuestThreads>& t, std::uint64_t b) : machine(m), threads(t), base(b) {
        if (!t) throw std::invalid_argument("Kernel event flags require a persistent guest scheduler");
        t->CheckIdleOwner();
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("Invalid kernel event flag gate page");
        for (const auto& mapping : m.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("Kernel event flag gate page already mapped");
        std::array<std::byte, 4096> bytes; bytes.fill(std::byte{0xcc});
        m.Map(base, bytes.size(), Permission::Read | Permission::Write);
        try { m.Write(base, bytes); m.Protect(base, bytes.size(), Permission::Read | Permission::Execute); }
        catch (...) { m.Unmap(base, bytes.size()); throw; }
    }
    ~Impl() { machine.Unmap(base, 4096); }
    std::shared_ptr<GuestThreads> scheduler() const {
        auto t = threads.lock();
        if (!t) throw std::runtime_error("Kernel event flag scheduler expired");
        return t;
    }
    void owner(bool idle) const {
        const auto t = scheduler();
        if (idle) t->CheckIdleOwner(); else (void)t->ActiveThread();
        if (!live) throw std::runtime_error("Kernel event flag provider was shut down");
    }
    void stopped(GuestThreadHandle thread) {
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->second.wait->thread != thread) { ++it; continue; }
            const auto p = it->second;
            std::erase(p.flag->waiting, p.wait);
            it = pending.erase(it);
            // A selected waiter that never returned did not observe the bits its
            // clear mode consumed; give them back and re-run release from pump().
            if (p.wait->selected && p.wait->code == 0 && p.wait->cleared && p.flag->live) {
                p.flag->pattern |= p.wait->cleared;
                refunded.push_back(p.flag);
            }
        }
    }
    static std::uint64_t consume(Flag& flag, std::uint64_t bits, std::uint32_t mode) {
        const auto before = flag.pattern;
        if (mode & ClearAll) flag.pattern = 0;
        else if (mode & ClearPattern) flag.pattern &= ~bits;
        return before & ~flag.pattern;
    }
    std::uint32_t deliver(const std::shared_ptr<Flag>& flag, const std::shared_ptr<Waiter>& wait) {
        owner(true);
        const auto found = pending.find(wait->key);
        if (found == pending.end() || found->second.flag != flag || found->second.wait != wait || !wait->selected)
            throw std::runtime_error("Kernel event flag completion lacks its exact selection");
        pending.erase(found);
        // A deleted flag has no pattern left to report.
        if (wait->result && wait->code != Deleted) write(machine, wait->result, wait->observed);
        if (wait->timeout) {
            std::uint32_t remaining = 0;
            if (wait->code != TimedOut && wait->deadline) {
                const auto left = *wait->deadline - Clock::now();
                if (left > Clock::duration::zero())
                    remaining = static_cast<std::uint32_t>(std::min<std::int64_t>(
                        std::chrono::ceil<std::chrono::microseconds>(left).count(), std::numeric_limits<std::uint32_t>::max()));
            }
            write(machine, wait->timeout, remaining);
        }
        return wait->code;
    }
    bool select(const std::shared_ptr<Flag>& flag, const std::shared_ptr<Waiter>& wait,
                std::uint32_t code, std::uint64_t observed) {
        wait->selected = true; wait->code = code; wait->observed = observed;
        const auto state = shared_from_this();
        if (waits->Wake(wait->thread, wait->key, [state, flag, wait] { return state->deliver(flag, wait); }))
            return true;
        wait->selected = false;
        return false;
    }
    void release(const std::shared_ptr<Flag>& flag) {
        // Walk waiters in wake order; each satisfied waiter observes the pattern
        // before its own clear mode applies, so a clearing waiter can starve
        // later ones exactly as on the source implementation.
        for (auto it = flag->waiting.begin(); it != flag->waiting.end();) {
            const auto wait = *it;
            if (!waits->IsWaiting(wait->thread, wait->key)) {
                pending.erase(wait->key); it = flag->waiting.erase(it); continue;
            }
            if (!satisfied(flag->pattern, wait->bits, wait->mode)) { ++it; continue; }
            const auto observed = flag->pattern;
            if (select(flag, wait, 0, observed)) wait->cleared = consume(*flag, wait->bits, wait->mode);
            else pending.erase(wait->key);
            it = flag->waiting.erase(it);
        }
    }
    void releaseAll(const std::shared_ptr<Flag>& flag, std::uint32_t code, std::int32_t* released = nullptr) {
        for (const auto& wait : flag->waiting) {
            if (select(flag, wait, code, flag->pattern)) { if (released) ++*released; }
            else pending.erase(wait->key);
        }
        flag->waiting.clear();
    }
    std::uint32_t invoke(unsigned op, Machine& m) {
        owner(false);
        const auto first = m.Get(Register::Rdi);
        if (op == Create) {
            const auto label = m.Get(Register::Rsi);
            const auto attr = static_cast<std::uint32_t>(m.Get(Register::Rdx));
            const auto initial = m.Get(Register::Rcx);
            if (!first || !label || (attr & ~(AttrFifo | AttrPriority | AttrSingle | AttrMulti)) ||
                (attr & (AttrFifo | AttrPriority)) == (AttrFifo | AttrPriority) ||
                (attr & (AttrSingle | AttrMulti)) == (AttrSingle | AttrMulti)) return Invalid;
            if (!accessible(m, first, 8, Permission::Write) || !validName(m, label)) return Invalid;
            if (flags.size() >= MaxFlags) throw std::runtime_error("Kernel event flag owned object bound exhausted");
            const auto token = identity();
            auto flag = std::make_shared<Flag>();
            flag->pattern = initial; flag->multi = (attr & AttrMulti) != 0; flag->fifo = !(attr & AttrPriority);
            flags.emplace(token, flag);
            m.Write(first, std::as_bytes(std::span(&token, 1)));
            return 0;
        }
        const auto found = flags.find(first);
        if (found == flags.end()) return Missing;
        const auto flag = found->second;
        if (op == Delete) {
            flag->live = false;
            releaseAll(flag, Deleted);
            flags.erase(found);
            return 0;
        }
        const auto bits = m.Get(Register::Rsi);
        if (op == Set) { flag->pattern |= bits; release(flag); return 0; }
        if (op == Clear) { flag->pattern &= bits; return 0; }
        if (op == Cancel) {
            const auto output = m.Get(Register::Rdx);
            if (output && !accessible(m, output, 4, Permission::Write)) return Fault;
            flag->pattern = bits;
            std::int32_t released = 0;
            releaseAll(flag, Canceled, &released);
            if (output) m.Write(output, std::as_bytes(std::span(&released, 1)));
            return 0;
        }
        const auto mode = static_cast<std::uint32_t>(m.Get(Register::Rdx));
        const auto result = m.Get(Register::Rcx);
        const auto timeout = op == Wait ? m.Get(Register::R8) : 0;
        if (!bits || !validMode(mode)) return Invalid;
        if (result && !accessible(m, result, 8, Permission::Write)) return Fault;
        if (timeout && !accessible(m, timeout, 4, Permission::Read | Permission::Write)) return Fault;
        if (!flag->multi && !flag->waiting.empty()) return Perm;
        if (satisfied(flag->pattern, bits, mode)) {
            if (result) m.Write(result, std::as_bytes(std::span(&flag->pattern, 1)));
            consume(*flag, bits, mode);
            return 0;
        }
        std::optional<std::uint32_t> micros;
        if (timeout) {
            std::uint32_t value = 0;
            m.Read(timeout, std::as_writable_bytes(std::span(&value, 1)));
            micros = value;
        }
        if (op == Poll || (micros && *micros == 0)) {
            if (result) m.Write(result, std::as_bytes(std::span(&flag->pattern, 1)));
            return op == Poll ? Busy : TimedOut;
        }
        if (nextWait == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Kernel event flag wait identity exhausted");
        auto wait = std::make_shared<Waiter>();
        wait->thread = waits->ActiveThread(); wait->key = nextWait++;
        wait->bits = bits; wait->mode = mode; wait->result = result; wait->timeout = timeout;
        wait->priority = scheduler()->BasePriority(wait->thread);
        if (micros) wait->deadline = Clock::now() + std::chrono::microseconds(*micros);
        if (flag->fifo) flag->waiting.push_back(wait);
        else {
            const auto position = std::upper_bound(flag->waiting.begin(), flag->waiting.end(), wait->priority,
                [](std::int32_t priority, const auto& other) { return priority < other->priority; });
            flag->waiting.insert(position, wait);
        }
        try {
            pending.emplace(wait->key, Pending{flag, wait});
            waits->BlockFromHostCall(wait->key);
        } catch (...) { std::erase(flag->waiting, wait); pending.erase(wait->key); throw; }
        return 0; // only scheduler completion returns the actual parked import
    }
    bool pump() {
        owner(true);
        for (const auto& flag : std::exchange(refunded, {})) if (flag->live) release(flag);
        const auto now = Clock::now();
        std::vector<Pending> expired;
        for (const auto& [key, p] : pending)
            if (!p.wait->selected && p.wait->deadline && now >= *p.wait->deadline) expired.push_back(p);
        for (const auto& p : expired) {
            std::erase(p.flag->waiting, p.wait);
            if (!select(p.flag, p.wait, TimedOut, p.flag->pattern)) pending.erase(p.wait->key);
        }
        return std::any_of(pending.begin(), pending.end(), [](const auto& item) { return !item.second.wait->selected; });
    }
};

KernelEventFlags::KernelEventFlags(Machine& m, const std::shared_ptr<GuestThreads>& threads, std::uint64_t base)
    : impl(std::make_shared<Impl>(m, threads, base)) {
    impl->waits.emplace(threads->CreateWaitDomain(m, [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle thread) {
        if (const auto state = weak.lock()) state->stopped(thread);
    }));
    impl->waits->SetOwnerPump([weak = std::weak_ptr<Impl>(impl)] {
        const auto state = weak.lock(); return state && state->live ? state->pump() : false;
    });
}
KernelEventFlags::~KernelEventFlags() { Shutdown(); }
std::optional<std::uint64_t> KernelEventFlags::Resolve(const SceImport& import, std::uint8_t type, std::uint64_t size) {
    // Unrelated imports pass through untouched, before any owner/lifetime check.
    const auto op = operation(import);
    if (op == inventory.size()) return std::nullopt;
    impl->owner(true);
    scope(import, type, size);
    const Impl::GateKey key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() >= 256) throw std::runtime_error("Kernel event flag gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}}; impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [weak = std::weak_ptr<Impl>(impl), op](Machine& m) {
        const auto state = weak.lock();
        if (!state) throw std::runtime_error("Kernel event flag provider expired");
        m.Set(Register::Rax, state->invoke(op, m));
    });
    impl->gates.emplace(key, gate);
    return gate;
}
void KernelEventFlags::Shutdown() {
    if (!impl || !impl->live) return;
    auto idle = impl->machine.CaptureContext();
    impl->waits->Withdraw();
    for (auto& [token, flag] : impl->flags) flag->live = false;
    impl->pending.clear(); impl->flags.clear(); impl->live = false;
}
}
