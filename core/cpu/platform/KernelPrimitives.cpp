#include "KernelPrimitives.hpp"
#include <cpu/SceElf.hpp>
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <tuple>

namespace Cpu::Platform {
namespace {
constexpr std::array<KernelPrimitiveImport, 9> inventory{{
    {"cmo1RIYva9o", "scePthreadMutexInit"}, {"upoVrzMHFeE", "scePthreadMutexTrylock"},
    {"tn3VlD0hG60", "scePthreadMutexUnlock"}, {"2Of0f+3mhhE", "scePthreadMutexDestroy"},
    {"F8bUHwAG284", "scePthreadMutexattrInit"}, {"iMp8QpE+XO4", "scePthreadMutexattrSettype"},
    {"smWEktiyyG0", "scePthreadMutexattrDestroy"}, {"1FGvU0i9saQ", "scePthreadMutexattrSetprotocol"},
    {"9UK1vLZQft4", "scePthreadMutexLock"}}};
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
std::span<const KernelPrimitiveImport> KernelPrimitiveInventory() { return inventory; }
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
    };
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::function<std::uint64_t()> active;
    std::uint64_t base;
    std::map<std::uint64_t, Attribute> attributes;
    std::map<std::uint64_t, Mutex> mutexes;
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
        for (auto& [slot, state] : mutexes) {
            std::erase(state.waiters, id);
            // Nonrobust owner death must never silently grant another owner.
            if (state.owner == id) {
                state.abandoned = true;
                if (state.protocol == 1 && waits) waits->SetInheritanceOwner(state.token, 0);
            }
        }
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
            if (waits && waits->Wake(id, state.token, 0)) return;
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
            mutexes.emplace(slot, Mutex{token, type, protocol, 0, 0, false, {}}); write(machine, slot, token); return 0;
        }
        const auto token = read(machine, slot);
        if (op == 3 && token < 2) return 0; // Upstream static initializer destroy preserves the slot.
        if (token == 2) return error(Invalid);
        if (op == 2 && token < 2) return error(Perm);
        auto mutex = mutexes.find(slot);
        if (mutex != mutexes.end() && mutex->second.token != token) return error(Invalid);
        if (token < 2 && (op == 1 || op == 8)) {
            thread(); // Validate scheduler identity before publishing a lazy initializer.
            span(machine, slot, Permission::Write);
            auto created = nextToken.fetch_add(1);
            mutex = mutexes.emplace(slot, Mutex{created, token == 1 ? 4u : 1u, 0, 0, 0, false, {}}).first;
            write(machine, slot, created);
        } else if (mutex == mutexes.end() || mutex->second.token != token) return error(Invalid);
        auto& state = mutex->second;
        if (state.abandoned)
            throw std::runtime_error("Unsupported nonrobust kernel mutex owner-exit recovery");
        if (op == 3) {
            if (state.owner || !state.waiters.empty()) return error(Busy);
            span(machine, slot, Permission::Write); write(machine, slot, 2);
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
    if (import.LibraryName != "libkernel" || import.ModuleName != "libkernel" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 || type != 2)
        throw std::runtime_error("Unsupported kernel primitive scope/version/type: " + import.Nid);
    const Impl::Key key{import.Nid, import.LibraryId, import.ModuleId};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->gates.size() >= 256) throw std::runtime_error("Kernel primitive gate page exhausted");
    const auto gate = impl->base + impl->gates.size() * 16;
    constexpr std::array ret{std::byte{0xc3}}; impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [weak = std::weak_ptr<Impl>(impl), op](Machine& m) {
        auto state = weak.lock(); if (!state) throw std::runtime_error("Kernel primitive provider expired");
        m.Set(Register::Rax, state->invoke(op, m.Get(Register::Rdi), m.Get(Register::Rsi), m.Get(Register::Rdx)));
    });
    impl->gates.emplace(key, gate); return gate;
}

TargetKernelMutexes::TargetKernelMutexes(Machine& machine, const std::shared_ptr<GuestThreads>& threads)
    : provider(machine, threads) {}
std::optional<std::uint64_t> TargetKernelMutexes::Resolve(const SceImport& import, std::uint8_t type,
                                                        KernelMutexConsumer source) {
    unsigned op = 0;
    for (; op < inventory.size(); ++op) if (inventory[op].Nid == import.Nid) break;
    if (op == inventory.size()) return std::nullopt;
    const bool eboot = source.Name == "eboot.bin" && source.Sha256 ==
        "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397";
    const bool libc = source.Name == "libc.prx" && source.Sha256 ==
        "78a080fdeccc28f2aa76356e97f82a35b3ba09deba8408dfce27db28fa0ce67f";
    const bool web = source.Name == "libSceNpCppWebApi.prx" && source.Sha256 ==
        "38db047fd9dfd27fc17dfc0dd2cff31a2e0533ac1be2350e5082f8499f59c6b9";
    if (!eboot && !(libc && op != 7) && !(web && op != 1 && op != 7))
        throw std::runtime_error("Unsupported target kernel mutex consumer source/import row");
    if ((eboot && (import.LibraryId != 44 || import.ModuleId != 24)) ||
        (libc && (import.LibraryId != 0 || import.ModuleId != 1)) ||
        (web && (import.LibraryId != 4 || import.ModuleId != 5)))
        throw std::runtime_error("Unsupported target kernel mutex consumer source/import row IDs");
    return provider.Resolve(import, type);
}
}
