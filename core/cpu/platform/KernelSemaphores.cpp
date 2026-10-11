#include "KernelSemaphores.hpp"
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
constexpr std::array<KernelSemaphoreImport, 6> inventory{{
    {"188x57JYp0g", "sceKernelCreateSema"}, {"R1Jvn8bSCW8", "sceKernelDeleteSema"},
    {"Zxa0VhQVTsk", "sceKernelWaitSema"}, {"4czppHBiriw", "sceKernelSignalSema"},
    {"12wOHk8ywb0", "sceKernelPollSema"}, {"4DM06U2BNEY", "sceKernelCancelSema"}}};
enum Op : unsigned { Create, Delete, Wait, Signal, Poll, Cancel };
// Engineered from pinned shadPS4 945dbc3c semaphore.cpp/orbis_error.h and the
// upstream Semaphore.cpp; target firmware error/race parity has not been established.
constexpr std::uint32_t Missing = 0x80020003u, Deleted = 0x8002000du, Fault = 0x8002000eu,
                        Busy = 0x80020010u, Invalid = 0x80020016u, TimedOut = 0x8002003cu,
                        Canceled = 0x80020055u;
// SCE_KERNEL_SEMA_ATTR_TH_FIFO / TH_PRIO; 0 keeps the historical priority order.
constexpr std::uint32_t AttrFifo = 1, AttrPriority = 2;
using Clock = std::chrono::steady_clock;
constexpr std::size_t MaxSemaphores = 65535;
// Process-wide monotonic numeric identities are never reused in another
// provider session. Significant upper bits detect low32 aliases; no identity
// contains a host address and no handle is dereferenced as guest memory.
std::atomic<std::uint64_t> nextIdentity{0x0c15000000000001ULL};
std::uint64_t identity() {
    auto candidate = nextIdentity.load();
    for (;;) {
        if (!candidate || candidate >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::runtime_error("Kernel semaphore identity exhausted");
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
        throw std::runtime_error("Unsupported kernel semaphore scope/version/type/size: " + import.Nid);
}
void checked(Machine& machine, std::uint64_t address, std::uint64_t size, Permission permission) {
    if (!address || !size || address > std::numeric_limits<std::uint64_t>::max() - size)
        throw std::runtime_error("Invalid kernel semaphore guest span");
    machine.CheckAccess(address, size, permission);
}
bool accessible(Machine& machine, std::uint64_t address, std::uint64_t size, Permission permission) {
    if (!address || address > std::numeric_limits<std::uint64_t>::max() - size) return false;
    try { machine.CheckAccess(address, size, permission); return true; }
    catch (const std::exception&) { return false; }
}
void name(Machine& machine, std::uint64_t address) {
    // Diagnostic bounded scan, not a claimed vendor name-length limit.
    for (std::uint64_t offset = 0; offset < 4096; ++offset) {
        if (address > std::numeric_limits<std::uint64_t>::max() - offset)
            throw std::runtime_error("Kernel semaphore name span overflows");
        checked(machine, address + offset, 1, Permission::Read);
        std::byte byte{}; machine.Read(address + offset, std::span(&byte, 1));
        if (byte == std::byte{}) return;
    }
    throw std::runtime_error("Kernel semaphore name exceeds diagnostic scan bound");
}
}
std::span<const KernelSemaphoreImport> KernelSemaphoreInventory() { return inventory; }

struct KernelSemaphores::Impl : std::enable_shared_from_this<KernelSemaphores::Impl> {
    struct Wait {
        GuestThreadHandle thread;
        std::uint64_t key;
        std::int32_t need, priority;
        bool selected = false, reserved = false;
        std::uint32_t result = 0;
        // Guest SceKernelUseconds* receiving the remaining time; 0 when absent.
        std::uint64_t timeout = 0;
        std::optional<Clock::time_point> deadline;
    };
    struct Semaphore {
        bool live = true, fifo = false;
        std::int32_t count, maximum, initial;
        std::vector<std::shared_ptr<Wait>> waiting;
    };
    struct Pending { std::shared_ptr<Semaphore> semaphore; std::shared_ptr<Wait> wait; };
    using GateKey = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    std::optional<GuestThreads::WaitDomain> waits;
    std::map<std::uint64_t, std::shared_ptr<Semaphore>> semaphores;
    // Retains selected object lifetimes after Delete removes their live handle.
    std::map<std::uint64_t, Pending> pending;
    std::map<GateKey, std::uint64_t> gates;
    // Semaphores that regained tokens from a stopped waiter (PLAT-24).
    std::vector<std::shared_ptr<Semaphore>> refunded;
    std::uint64_t base, nextWait = 1;
    bool live = true;

    Impl(Machine& m, const std::shared_ptr<GuestThreads>& t, std::uint64_t b)
        : machine(m), threads(t), base(b) {
        if (!t) throw std::invalid_argument("Kernel semaphores require a persistent guest scheduler");
        t->CheckIdleOwner();
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("Invalid kernel semaphore gate page");
        for (const auto& mapping : m.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("Kernel semaphore gate page already mapped");
        std::array<std::byte, 4096> bytes; bytes.fill(std::byte{0xcc});
        m.Map(base, bytes.size(), Permission::Read | Permission::Write);
        try { m.Write(base, bytes); m.Protect(base, bytes.size(), Permission::Read | Permission::Execute); }
        catch (...) { m.Unmap(base, bytes.size()); throw; }
    }
    ~Impl() { machine.Unmap(base, 4096); }
    std::shared_ptr<GuestThreads> scheduler() const {
        auto t = threads.lock();
        if (!t) throw std::runtime_error("Kernel semaphore scheduler expired");
        return t;
    }
    void owner(bool idle) const {
        const auto t = scheduler();
        if (idle) t->CheckIdleOwner(); else (void)t->ActiveThread();
        if (!live) throw std::runtime_error("Kernel semaphore provider was shut down");
    }
    void stopped(GuestThreadHandle thread) {
        for (auto it = pending.begin(); it != pending.end();) {
            const auto p = it->second;
            if (p.wait->thread != thread) { ++it; continue; }
            std::erase(p.semaphore->waiting, p.wait);
            it = pending.erase(it);
            // Signal consumed the tokens at selection. A waiter that stops
            // before returning never received them, so hand them back to the
            // live semaphore and let the remaining waiters claim them.
            if (p.wait->reserved && p.wait->result == 0 && p.semaphore->live)
                p.semaphore->count = static_cast<std::int32_t>(std::min<std::int64_t>(
                    p.semaphore->maximum, static_cast<std::int64_t>(p.semaphore->count) + p.wait->need));
            // Also re-offer when an unreserved FIFO head leaves: later waiters
            // may only have been blocked behind it.
            if (p.semaphore->live) refunded.push_back(p.semaphore);
            p.wait->reserved = false;
        }
        // Re-offered from pump() on the idle owner, never from this callback.
    }
    std::uint32_t remaining(const std::shared_ptr<Wait>& wait, bool expired) {
        if (!wait->timeout) return 0;
        std::uint64_t micros = 0;
        if (!expired && wait->deadline) {
            const auto left = *wait->deadline - Clock::now();
            if (left > Clock::duration::zero())
                micros = static_cast<std::uint64_t>(std::chrono::ceil<std::chrono::microseconds>(left).count());
        }
        const auto value = static_cast<std::uint32_t>(std::min<std::uint64_t>(micros, std::numeric_limits<std::uint32_t>::max()));
        checked(machine, wait->timeout, sizeof(value), Permission::Write);
        machine.Write(wait->timeout, std::as_bytes(std::span(&value, 1)));
        return value;
    }
    std::uint32_t deliver(const std::shared_ptr<Semaphore>& semaphore, const std::shared_ptr<Wait>& wait) {
        owner(true);
        // GuestThreads validated the exact suspension/gate before invoking us.
        const auto found = pending.find(wait->key);
        if (found == pending.end() || found->second.semaphore != semaphore || found->second.wait != wait ||
            !wait->selected || (wait->result == 0 && !wait->reserved))
            throw std::runtime_error("Kernel semaphore completion lacks its exact reservation");
        const auto result = wait->result;
        if (wait->timeout) remaining(wait, result == TimedOut);
        wait->reserved = false;
        pending.erase(found);
        return result;
    }
    bool select(const std::shared_ptr<Semaphore>& semaphore, const std::shared_ptr<Wait>& wait,
                std::uint32_t result) {
        wait->selected = true;
        wait->result = result;
        const auto state = shared_from_this();
        if (waits->Wake(wait->thread, wait->key, [state, semaphore, wait] { return state->deliver(semaphore, wait); }))
            return true;
        wait->selected = false;
        return false;
    }
    void signal(const std::shared_ptr<Semaphore>& semaphore) {
        // Stable insertion by base priority was done at block time. Scan all
        // fitting requests, matching pinned source rather than head-only FIFO.
        for (auto it = semaphore->waiting.begin(); it != semaphore->waiting.end();) {
            const auto wait = *it;
            if (!waits->IsWaiting(wait->thread, wait->key)) {
                pending.erase(wait->key); it = semaphore->waiting.erase(it); continue;
            }
            if (wait->need > semaphore->count) {
                // FIFO hands tokens out strictly in arrival order.
                if (semaphore->fifo) break;
                ++it; continue;
            }
            semaphore->count -= wait->need;
            wait->reserved = true;
            if (!select(semaphore, wait, 0)) {
                semaphore->count += wait->need;
                wait->reserved = false;
                pending.erase(wait->key);
            }
            it = semaphore->waiting.erase(it);
        }
    }
    bool takeNow(const Semaphore& semaphore, std::int32_t count) const {
        // A FIFO newcomer never takes tokens ahead of an already queued waiter.
        return semaphore.count >= count && !(semaphore.fifo && !semaphore.waiting.empty());
    }
    std::uint32_t invoke(unsigned op, Machine& m) {
        owner(false);
        const auto first = m.Get(Register::Rdi);
        if (op == Create) {
            const auto label = m.Get(Register::Rsi);
            const auto attr = static_cast<std::uint32_t>(m.Get(Register::Rdx));
            const auto initial = static_cast<std::int32_t>(m.Get(Register::Rcx));
            const auto maximum = static_cast<std::int32_t>(m.Get(Register::R8));
            if (!first || !label || attr > AttrPriority || m.Get(Register::R9) != 0 || initial < 0 ||
                maximum <= 0 || initial > maximum) return Invalid;
            checked(m, first, 8, Permission::Write); name(m, label);
            if (semaphores.size() >= MaxSemaphores)
                throw std::runtime_error("Kernel semaphore owned object bound exhausted");
            const auto token = identity();
            auto semaphore = std::make_shared<Semaphore>();
            semaphore->count = initial; semaphore->maximum = maximum; semaphore->initial = initial;
            semaphore->fifo = attr == AttrFifo;
            semaphores.emplace(token, semaphore);
            try { m.Write(first, std::as_bytes(std::span(&token, 1))); }
            catch (...) { semaphores.erase(token); throw; }
            return 0;
        }
        const auto found = semaphores.find(first);
        if (found == semaphores.end()) return Missing;
        const auto semaphore = found->second;
        if (op == Delete) {
            semaphore->live = false;
            // Previously selected successful wakes retain both their result and
            // object lifetime. Only still-blocked waits receive source EACCES.
            for (const auto& wait : semaphore->waiting)
                if (!select(semaphore, wait, Deleted)) pending.erase(wait->key);
            semaphore->waiting.clear();
            semaphores.erase(found);
            return 0;
        }
        const auto count = static_cast<std::int32_t>(m.Get(Register::Rsi));
        if (op == Cancel) {
            if (count > semaphore->maximum) return Invalid;
            const auto output = m.Get(Register::Rdx);
            if (output && !accessible(m, output, 4, Permission::Write)) return Fault;
            // Every still-blocked waiter returns ECANCELED; the count becomes
            // the requested value, or the creation count when it is negative.
            std::int32_t cancelled = 0;
            for (const auto& wait : semaphore->waiting) {
                if (select(semaphore, wait, Canceled)) ++cancelled;
                else pending.erase(wait->key);
            }
            semaphore->waiting.clear();
            semaphore->count = count < 0 ? semaphore->initial : count;
            if (output) m.Write(output, std::as_bytes(std::span(&cancelled, 1)));
            return 0;
        }
        if (count <= 0 || count > semaphore->maximum) return Invalid;
        if (op == Signal) {
            // Wider arithmetic prevents signed overflow before max comparison.
            const auto sum = static_cast<std::int64_t>(semaphore->count) + count;
            if (sum > semaphore->maximum) return Invalid;
            semaphore->count = static_cast<std::int32_t>(sum);
            signal(semaphore);
            return 0;
        }
        if (op == Poll) {
            if (!takeNow(*semaphore, count)) return Busy;
            semaphore->count -= count;
            return 0;
        }
        const auto timeout = m.Get(Register::Rdx);
        std::optional<std::uint32_t> micros;
        if (timeout) {
            if (!accessible(m, timeout, 4, Permission::Read | Permission::Write)) return Fault;
            std::uint32_t value = 0;
            m.Read(timeout, std::as_writable_bytes(std::span(&value, 1)));
            micros = value;
        }
        if (takeNow(*semaphore, count)) { semaphore->count -= count; return 0; }
        if (micros && *micros == 0) return TimedOut;
        if (nextWait == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Kernel semaphore wait identity exhausted");
        auto wait = std::make_shared<Wait>();
        wait->thread = waits->ActiveThread(); wait->key = nextWait++;
        wait->need = count; wait->priority = scheduler()->BasePriority(wait->thread);
        wait->timeout = timeout;
        if (micros) wait->deadline = Clock::now() + std::chrono::microseconds(*micros);
        if (semaphore->fifo) semaphore->waiting.push_back(wait);
        else {
            // Ascending numeric base priority; upper_bound preserves equal ties.
            const auto position = std::upper_bound(semaphore->waiting.begin(), semaphore->waiting.end(), wait->priority,
                [](std::int32_t priority, const auto& other) { return priority < other->priority; });
            semaphore->waiting.insert(position, wait);
        }
        try {
            pending.emplace(wait->key, Pending{semaphore, wait});
            waits->BlockFromHostCall(wait->key);
        } catch (...) { std::erase(semaphore->waiting, wait); pending.erase(wait->key); throw; }
        return 0; // only scheduler completion returns the actual parked import
    }
    bool pump() {
        owner(true);
        // Tokens handed back by a stopped waiter are offered to the others here,
        // on the idle owner, never from inside the scheduler's stop callback.
        const auto again = std::exchange(refunded, {});
        for (const auto& semaphore : again) if (semaphore->live) signal(semaphore);
        // Expire timed waits. An expired waiter leaves the queue and returns
        // ETIMEDOUT with a zero remainder; the idle-cap policy stays separate.
        const auto now = Clock::now();
        std::vector<Pending> expired;
        for (const auto& [key, p] : pending)
            if (!p.wait->selected && p.wait->deadline && now >= *p.wait->deadline) expired.push_back(p);
        std::vector<std::shared_ptr<Semaphore>> touched;
        for (const auto& p : expired) {
            std::erase(p.semaphore->waiting, p.wait);
            if (!select(p.semaphore, p.wait, TimedOut)) pending.erase(p.wait->key);
            touched.push_back(p.semaphore);
        }
        // Only after every expiry: a FIFO head that timed out may have been
        // holding back later waiters, and none of them may be selected twice.
        for (const auto& semaphore : touched) if (semaphore->live) signal(semaphore);
        // Keeps an indefinite wait responsive to the owner's stop/idle policy.
        return std::any_of(pending.begin(), pending.end(), [](const auto& item) { return !item.second.wait->selected; });
    }
};

KernelSemaphores::KernelSemaphores(Machine& m, const std::shared_ptr<GuestThreads>& threads, std::uint64_t base)
    : impl(std::make_shared<Impl>(m, threads, base)) {
    impl->waits.emplace(threads->CreateWaitDomain(m, [weak = std::weak_ptr<Impl>(impl)](GuestThreadHandle thread) {
        if (const auto state = weak.lock()) state->stopped(thread);
    }));
    impl->waits->SetOwnerPump([weak = std::weak_ptr<Impl>(impl)] {
        const auto state = weak.lock(); return state && state->live ? state->pump() : false;
    });
}
KernelSemaphores::~KernelSemaphores() { Shutdown(); }
std::optional<std::uint64_t> KernelSemaphores::Resolve(const SceImport& import, std::uint8_t type, std::uint64_t size) {
    impl->owner(true);
    const auto op = operation(import);
    if (op == inventory.size()) return std::nullopt;
    scope(import, type, size);
    const Impl::GateKey key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() >= 256) throw std::runtime_error("Kernel semaphore gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}}; impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [weak = std::weak_ptr<Impl>(impl), op](Machine& m) {
        const auto state = weak.lock();
        if (!state) throw std::runtime_error("Kernel semaphore provider expired");
        m.Set(Register::Rax, state->invoke(op, m));
    });
    impl->gates.emplace(key, gate);
    return gate;
}
void KernelSemaphores::Shutdown() {
    if (!impl || !impl->live) return;
    // Capture checks persistent Machine ownership even after scheduler teardown.
    auto idle = impl->machine.CaptureContext();
    impl->waits->Withdraw();
    for (auto& [token, semaphore] : impl->semaphores) semaphore->live = false;
    for (auto& [key, p] : impl->pending) p.wait->reserved = false;
    impl->pending.clear(); impl->semaphores.clear(); impl->live = false;
}
TargetKernelSemaphores::TargetKernelSemaphores(Machine& m, const std::shared_ptr<GuestThreads>& threads)
    : provider(m, threads) {}
std::optional<std::uint64_t> TargetKernelSemaphores::Resolve(const SceImport& import, std::uint8_t type,
                                                         std::uint64_t size) {
    if (operation(import) == inventory.size()) return std::nullopt;
    scope(import, type, size);
    return provider.Resolve(import, type, size);
}
}
