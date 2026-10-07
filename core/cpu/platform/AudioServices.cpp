#include "AudioServices.hpp"
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>

namespace Cpu::Platform {
namespace {
constexpr std::uint32_t invalidContext = 0x80930002;
constexpr std::uint32_t invalidParameter = 0x80930005;
constexpr std::uint32_t outOfResources = 0x80930007;
constexpr std::uint32_t codecNotSupported = 0x80930008;
// Tokens belong to one provider's set, but cannot collide across providers or replacements.
// Exhaustion stays latched at UINT32_MAX+1; it never wraps into a live or stale token.
std::atomic<std::uint64_t> nextContext{1};
std::optional<std::uint32_t> reserveContext() {
    auto current = nextContext.load(std::memory_order_relaxed);
    while (current <= std::numeric_limits<std::uint32_t>::max()) {
        if (nextContext.compare_exchange_weak(current, current + 1, std::memory_order_relaxed))
            return static_cast<std::uint32_t>(current);
    }
    return std::nullopt;
}
[[noreturn]] void fault(const char* message) {
    throw std::runtime_error(std::string("Unsupported SCE AJM: ") + message);
}
}
struct AudioServices::Impl {
    Machine& machine;
    std::uint64_t base;
    std::set<std::uint32_t> contexts;
    std::array<bool, 3> registered{};
    Impl(Machine& guest, std::uint64_t address) : machine(guest), base(address) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("AJM gates require an aligned low canonical page");
        std::array<std::byte, 4096> code;
        code.fill(std::byte{0xcc});
        for (std::size_t i = 0; i < registered.size(); ++i) code[i * 16] = std::byte{0xc3};
        machine.Map(base, code.size(), Permission::Read | Permission::Write);
        try {
            machine.Write(base, code);
            machine.Protect(base, code.size(), Permission::Read | Permission::Execute);
        } catch (...) {
            machine.Unmap(base, code.size());
            throw;
        }
    }
    ~Impl() noexcept {
        // Unmap also removes this page's host calls; a replacement can safely reuse the address.
        try { machine.Unmap(base, 4096); }
        catch (...) { std::terminate(); }
    }
    std::uint32_t invoke(Machine& guest, std::size_t operation) {
        if (operation == 0) {
            const auto destination = guest.Get(Register::Rsi);
            if (guest.Get(Register::Rdi) || !destination) return invalidParameter;
            // Never cast guest storage to a host pointer. Validate the entire output before state changes.
            if (destination > std::numeric_limits<std::uint64_t>::max() - 4)
                fault("context output span overflows");
            guest.CheckAccess(destination, 4, Permission::Write);
            const auto token = reserveContext();
            if (!token) return outOfResources;
            const auto id = *token;
            contexts.insert(id);
            const std::array bytes{std::byte(id & 255), std::byte((id >> 8) & 255),
                                   std::byte((id >> 16) & 255), std::byte((id >> 24) & 255)};
            try { guest.Write(destination, bytes); }
            catch (...) { contexts.erase(id); throw; }
            return 0;
        }
        // Public ModuleRegister validates its reserved argument before looking up the context.
        if (operation == 2 && guest.Get(Register::Rdx)) return invalidParameter;
        const auto context = static_cast<std::uint32_t>(guest.Get(Register::Rdi));
        if (!contexts.contains(context)) return invalidContext;
        if (operation == 1) {
            contexts.erase(context);
            return 0;
        }
        // Context existence is real; codec availability is not. No registration success without a decoder.
        return codecNotSupported;
    }
};
AudioServices::AudioServices(Machine& machine, std::uint64_t base) : impl(std::make_shared<Impl>(machine, base)) {}
AudioServices::~AudioServices() = default;
std::optional<std::uint64_t> AudioServices::Resolve(const SceImport& import, std::uint8_t type) {
    if (import.LibraryName != "libSceAjm") return std::nullopt;
    if (import.ModuleName != "libSceAjm" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 || type != 2)
        return std::nullopt;
    const auto found = std::find(Nids.begin(), Nids.end(), import.Nid);
    if (found == Nids.end()) return std::nullopt;
    const auto operation = static_cast<std::size_t>(found - Nids.begin());
    const auto gate = impl->base + operation * 16;
    if (!impl->registered[operation]) {
        impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation](Machine& guest) {
            const auto owner = state.lock();
            if (!owner) fault("provider lifetime ended");
            const auto result = owner->invoke(guest, operation);
            // int result uses EAX (upper half zero); consumers inspect the signed low 32 bits.
            guest.Set(Register::Rax, result);
        });
        impl->registered[operation] = true;
    }
    return gate;
}
}
