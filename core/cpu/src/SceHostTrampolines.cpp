#include <cpu/SceHostTrampolines.hpp>
#include <array>
#include <stdexcept>
#include <vector>

namespace Cpu {
namespace {
constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t SlotSize = 16;
}

struct SceHostTrampolines::Impl {
    Machine& machine;
    std::uint64_t base;
    std::size_t capacity;
    std::vector<std::function<void(Machine&)>> handlers;
};

SceHostTrampolines::SceHostTrampolines(Machine& machine, std::uint64_t base, std::size_t capacity)
    : impl(std::make_shared<Impl>(Impl{machine, base, capacity, {}})) {
    if (!base || (base & (PageSize - 1)) || base >= 0x7ffffffff000)
        throw std::invalid_argument("SCE host trampolines require a nonzero aligned low canonical guest page");
    if (!capacity || capacity > 0x7fffffff / SlotSize)
        throw std::invalid_argument("SCE host trampolines require a bounded nonzero capacity");
    // Slot 0 is the shared gate; slots 1..capacity hold the trampolines.
    const auto size = ((capacity + 1) * SlotSize + PageSize - 1) & ~(PageSize - 1);
    std::vector<std::byte> bytes(size, std::byte{0xcc});
    bytes[0] = std::byte{0xc3};
    machine.Map(base, size, Permission::Read | Permission::Write);
    machine.Write(base, bytes);
    machine.Protect(base, size, Permission::Read | Permission::Execute);
    machine.AddHostCall(base, [state = std::weak_ptr<Impl>(impl)](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE host trampoline runtime has expired");
        const auto index = guest.Get(Register::R11);
        if (!index || index > context->handlers.size())
            throw std::runtime_error("Unsupported SCE host trampoline index " + std::to_string(index));
        // Copy: a handler may add trampolines and reallocate the table.
        const auto handler = context->handlers[index - 1];
        handler(guest);
    });
}

SceHostTrampolines::~SceHostTrampolines() = default;

std::uint64_t SceHostTrampolines::Add(std::function<void(Machine&)> handler) {
    if (!handler) throw std::invalid_argument("SCE host trampoline requires a handler");
    if (impl->handlers.size() == impl->capacity)
        throw std::runtime_error("SCE host trampoline table is exhausted");
    const auto index = static_cast<std::uint32_t>(impl->handlers.size() + 1);
    const auto slot = impl->base + index * SlotSize;
    // mov r11d, index ; jmp base
    const auto displacement = static_cast<std::uint32_t>(static_cast<std::int64_t>(impl->base) -
                                                         static_cast<std::int64_t>(slot + 11));
    const std::array code{std::byte{0x41}, std::byte{0xbb},
        static_cast<std::byte>(index), static_cast<std::byte>(index >> 8),
        static_cast<std::byte>(index >> 16), static_cast<std::byte>(index >> 24),
        std::byte{0xe9}, static_cast<std::byte>(displacement), static_cast<std::byte>(displacement >> 8),
        static_cast<std::byte>(displacement >> 16), static_cast<std::byte>(displacement >> 24)};
    impl->machine.Write(slot, code);
    impl->handlers.push_back(std::move(handler));
    return slot;
}

std::size_t SceHostTrampolines::Size() const { return impl->handlers.size(); }

}
