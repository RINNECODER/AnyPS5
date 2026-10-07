#include "KernelPrimitives.hpp"
#include <cpu/SceElf.hpp>
#include <array>
#include <atomic>
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
std::uint32_t error(unsigned posix) { return posix ? 0x80020000u + posix : 0; }
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
    struct Attribute { std::uint64_t token; unsigned type = 1; };
    struct Mutex { std::uint64_t token; unsigned type; std::uint64_t owner = 0; unsigned depth = 0; };
    using Key = std::tuple<std::string, std::uint16_t, std::uint16_t>;
    Machine& machine;
    std::function<std::uint64_t()> active;
    std::uint64_t base;
    std::map<std::uint64_t, Attribute> attributes;
    std::map<std::uint64_t, Mutex> mutexes;
    std::map<Key, std::uint64_t> gates;
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
    ~Impl() { machine.Unmap(base, 4096); }
    std::uint64_t thread() {
        const auto id = active();
        if (!id) throw std::runtime_error("Kernel primitive requires an active guest thread");
        return id;
    }
    std::uint32_t invoke(unsigned op, std::uint64_t slot, std::uint64_t arg, std::uint64_t label) {
        if (op == 4) {
            span(machine, slot, Permission::Write);
            if (attributes.contains(slot)) return error(16);
            auto token = nextToken.fetch_add(1);
            attributes.emplace(slot, Attribute{token}); write(machine, slot, token); return 0;
        }
        if (op == 5 || op == 6 || op == 7) {
            const auto token = read(machine, slot);
            auto attr = attributes.find(slot);
            if (attr == attributes.end() || attr->second.token != token) return error(22);
            if (op == 5) {
                const auto type = static_cast<std::uint32_t>(arg);
                if (type < 1 || type > 4) return error(22);
                attr->second.type = type; return 0;
            }
            if (op == 7) {
                const auto protocol = static_cast<std::uint32_t>(arg);
                if (protocol > 2) return error(22);
                if (protocol) throw std::runtime_error("Unsupported kernel mutex priority protocol: guest scheduler integration required");
                return 0;
            }
            span(machine, slot, Permission::Write); write(machine, slot, 0); attributes.erase(attr); return 0;
        }
        if (op == 0) {
            span(machine, slot, Permission::Write);
            if (mutexes.contains(slot)) return error(16);
            unsigned type = 1;
            if (arg) {
                const auto token = read(machine, arg); auto attr = attributes.find(arg);
                if (token) {
                    if (attr == attributes.end() || attr->second.token != token) return error(22);
                    type = attr->second.type;
                }
            }
            name(machine, label);
            auto token = nextToken.fetch_add(1);
            mutexes.emplace(slot, Mutex{token, type}); write(machine, slot, token); return 0;
        }
        const auto token = read(machine, slot);
        if (op == 3 && token < 2) return 0; // Upstream static initializer destroy preserves the slot.
        if (token == 2) return error(22);
        if (op == 2 && token < 2) return error(1);
        auto mutex = mutexes.find(slot);
        if (mutex != mutexes.end() && mutex->second.token != token) return error(22);
        if (token < 2 && (op == 1 || op == 8)) {
            thread(); // Validate scheduler identity before publishing a lazy initializer.
            span(machine, slot, Permission::Write);
            auto created = nextToken.fetch_add(1);
            mutex = mutexes.emplace(slot, Mutex{created, token == 1 ? 4u : 1u}).first;
            write(machine, slot, created);
        } else if (mutex == mutexes.end() || mutex->second.token != token) return error(22);
        auto& state = mutex->second;
        if (op == 3) {
            if (state.owner) return error(16);
            span(machine, slot, Permission::Write); write(machine, slot, 2); mutexes.erase(mutex); return 0;
        }
        const auto id = thread();
        if (op == 2) {
            if (state.owner != id) return error(1);
            if (--state.depth == 0) state.owner = 0;
            return 0;
        }
        if (state.owner) {
            if (state.owner == id && state.type == 2) {
                if (state.depth == std::numeric_limits<int>::max()) return error(35);
                ++state.depth; return 0;
            }
            if (op == 1) return error(16);
            if (state.owner == id && (state.type == 1 || state.type == 4)) return error(11);
            throw std::runtime_error("Unsupported contended kernel mutex lock: guest wait/wake scheduler integration required");
        }
        state.owner = id; state.depth = 1; return 0;
    }
};
KernelPrimitives::KernelPrimitives(Machine& m, std::function<std::uint64_t()> active, std::uint64_t b)
    : impl(std::make_shared<Impl>(m, std::move(active), b)) {}
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
}
