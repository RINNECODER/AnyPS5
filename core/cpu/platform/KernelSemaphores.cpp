#include "KernelSemaphores.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace Cpu::Platform {
namespace {
constexpr std::array<KernelSemaphoreImport, 4> inventory{{
    {"188x57JYp0g", "sceKernelCreateSema"}, {"R1Jvn8bSCW8", "sceKernelDeleteSema"},
    {"Zxa0VhQVTsk", "sceKernelWaitSema"}, {"4czppHBiriw", "sceKernelSignalSema"}}};
// Engineered from pinned shadPS4 945dbc3c semaphore.cpp/orbis_error.h;
// target firmware error/race parity has not been established.
constexpr std::uint32_t Missing = 0x80020003u, Deleted = 0x8002000du, Invalid = 0x80020016u;
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
    };
    struct Semaphore {
        bool live = true;
        std::int32_t count, maximum;
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
    std::optional<SceHostTrampolines> trampolines;
    std::uint64_t nextWait = 1;
    bool live = true;

    Impl(Machine& m, const std::shared_ptr<GuestThreads>& t, std::uint64_t base)
        : machine(m), threads(t) {
        if (!t) throw std::invalid_argument("Kernel semaphores require a persistent guest scheduler");
        t->CheckIdleOwner();
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("Invalid kernel semaphore gate page");
        for (const auto& mapping : m.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("Kernel semaphore gate page already mapped");
        trampolines.emplace(m, base, SceHostTrampolines::DefaultCapacity, "Kernel semaphore");
    }
    ~Impl() { trampolines->Release(); }
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
            const auto& p = it->second;
            if (p.wait->thread != thread) { ++it; continue; }
            std::erase(p.semaphore->waiting, p.wait);
            // Signal consumed assigned tokens exactly once at selection, as in
            // the public source. Stop retires that exact reservation; it cannot
            // resurrect a consumed token or publish successful guest return.
            p.wait->reserved = false;
            it = pending.erase(it);
        }
    }
    std::uint32_t deliver(const std::shared_ptr<Semaphore>& semaphore, const std::shared_ptr<Wait>& wait) {
        owner(true);
        // GuestThreads validated the exact suspension/gate before invoking us.
        const auto found = pending.find(wait->key);
        if (found == pending.end() || found->second.semaphore != semaphore || found->second.wait != wait ||
            !wait->selected || (wait->result == 0 && !wait->reserved))
            throw std::runtime_error("Kernel semaphore completion lacks its exact reservation");
        const auto result = wait->result;
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
            if (wait->need > semaphore->count) { ++it; continue; }
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
    std::uint32_t invoke(unsigned op, Machine& m) {
        owner(false);
        const auto first = m.Get(Register::Rdi);
        if (op == 0) {
            const auto label = m.Get(Register::Rsi);
            const auto attr = static_cast<std::uint32_t>(m.Get(Register::Rdx));
            const auto initial = static_cast<std::int32_t>(m.Get(Register::Rcx));
            const auto maximum = static_cast<std::int32_t>(m.Get(Register::R8));
            if (!first || !label || attr != 0 || m.Get(Register::R9) != 0 || initial < 0 ||
                maximum <= 0 || initial > maximum) return Invalid;
            checked(m, first, 8, Permission::Write); name(m, label);
            if (semaphores.size() >= MaxSemaphores)
                throw std::runtime_error("Kernel semaphore owned object bound exhausted");
            const auto token = identity();
            auto semaphore = std::make_shared<Semaphore>();
            semaphore->count = initial; semaphore->maximum = maximum;
            semaphores.emplace(token, semaphore);
            try { m.Write(first, std::as_bytes(std::span(&token, 1))); }
            catch (...) { semaphores.erase(token); throw; }
            return 0;
        }
        const auto found = semaphores.find(first);
        if (found == semaphores.end()) return Missing;
        const auto semaphore = found->second;
        if (op == 1) {
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
        if (count <= 0 || count > semaphore->maximum) return Invalid;
        if (op == 3) {
            // Wider arithmetic prevents signed overflow before max comparison.
            const auto sum = static_cast<std::int64_t>(semaphore->count) + count;
            if (sum > semaphore->maximum) return Invalid;
            semaphore->count = static_cast<std::int32_t>(sum);
            signal(semaphore);
            return 0;
        }
        // Non-null timeout width/remaining-time semantics are unqualified.
        if (m.Get(Register::Rdx) != 0) return Invalid;
        if (semaphore->count >= count) { semaphore->count -= count; return 0; }
        if (nextWait == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Kernel semaphore wait identity exhausted");
        auto wait = std::make_shared<Wait>();
        wait->thread = waits->ActiveThread(); wait->key = nextWait++;
        wait->need = count; wait->priority = scheduler()->BasePriority(wait->thread);
        // Ascending numeric base priority; upper_bound preserves equal ties.
        const auto position = std::upper_bound(semaphore->waiting.begin(), semaphore->waiting.end(), wait->priority,
            [](std::int32_t priority, const auto& other) { return priority < other->priority; });
        semaphore->waiting.insert(position, wait);
        try {
            pending.emplace(wait->key, Pending{semaphore, wait});
            waits->BlockFromHostCall(wait->key);
        } catch (...) { std::erase(semaphore->waiting, wait); pending.erase(wait->key); throw; }
        return 0; // only scheduler completion returns the actual parked import
    }
    bool pump() {
        owner(true);
        // Keeps an indefinite wait responsive to the owner's stop/idle policy.
        // No time cap here is interpreted as a successful wait or API timeout.
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
    const auto gate = impl->trampolines->Add([weak = std::weak_ptr<Impl>(impl), op](Machine& m) {
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
