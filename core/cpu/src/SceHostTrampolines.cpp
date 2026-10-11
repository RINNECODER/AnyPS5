#include <cpu/SceHostTrampolines.hpp>
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Cpu {
namespace {
constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t SlotSize = 16;
// Slot 0 is the shared gate; the last slot must stay within JMP rel32 reach of it.
constexpr std::size_t MaximumCapacity = 0x7fffffff / SlotSize - 1;
constexpr std::uint64_t AddressLimit = 0x7ffffffff000;

std::string hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}
}

struct SceHostTrampolines::Impl {
    Machine& machine;
    std::uint64_t base;
    std::size_t capacity;
    std::string name;
    std::uint64_t reserved;
    std::uint64_t mapped = 0;
    std::vector<std::uint64_t> chunks;
    std::vector<std::function<void(Machine&)>> handlers;

    // Maps [base + mapped, base + mapped + size) as INT3-filled executable code.
    void map(std::uint64_t size, bool gate) {
        const auto address = base + mapped;
        std::vector<std::byte> bytes(size, std::byte{0xcc});
        if (gate) bytes[0] = std::byte{0xc3};
        try {
            machine.Map(address, size, Permission::Read | Permission::Write);
        } catch (const std::exception& error) {
            throw HostCapacityError(name + " host entry table cannot grow to " + std::to_string(handlers.size() + 1) +
                " entries: guest range " + hex(address) + "+" + hex(size) + " is unavailable (" + error.what() + ")");
        }
        try {
            machine.Write(address, bytes);
            machine.Protect(address, size, Permission::Read | Permission::Execute);
        } catch (...) {
            machine.Unmap(address, size);
            throw;
        }
        chunks.push_back(size);
        mapped += size;
    }
};

SceHostTrampolines::SceHostTrampolines(Machine& machine, std::uint64_t base, std::size_t capacity, std::string name)
    : impl(std::make_shared<Impl>(Impl{machine, base, capacity, std::move(name), 0, 0, {}, {}})) {
    if (!base || (base & (PageSize - 1)) || base >= AddressLimit)
        throw std::invalid_argument("SCE host trampolines require a nonzero aligned low canonical guest page");
    if (!capacity || capacity > MaximumCapacity)
        throw std::invalid_argument("SCE host trampolines require a bounded nonzero capacity");
    impl->reserved = ((capacity + 1) * SlotSize + PageSize - 1) & ~(PageSize - 1);
    // A table at the top of low canonical memory holds only what fits below the limit.
    if (impl->reserved > AddressLimit - base) {
        impl->reserved = AddressLimit - base;
        impl->capacity = impl->reserved / SlotSize - 1;
    }
    impl->map(PageSize, true);
    try {
        // The machine keeps the table, like a per-entry gate keeps its handler: each handler
        // then reports its own provider's expiry exactly as it did behind a dedicated gate.
        machine.AddHostCall(base, [context = impl](Machine& guest) {
            const auto index = guest.Get(Register::R11);
            if (!index || index > context->handlers.size())
                throw std::runtime_error("Unsupported SCE host trampoline index " + std::to_string(index) +
                                         " in the " + context->name + " host entry table");
            // Copy: a handler may add trampolines and reallocate the table.
            const auto handler = context->handlers[index - 1];
            handler(guest);
        });
    } catch (...) {
        machine.Unmap(base, PageSize);
        throw;
    }
}

SceHostTrampolines::~SceHostTrampolines() = default;

std::uint64_t SceHostTrampolines::Add(std::function<void(Machine&)> handler) {
    if (!handler) throw std::invalid_argument("SCE host trampoline requires a handler");
    if (!impl->mapped) throw std::logic_error(impl->name + " host entry table was released");
    if (impl->handlers.size() == impl->capacity)
        throw HostCapacityError(impl->name + " host entry table is full: all " + std::to_string(impl->capacity) +
            " entries reserved at " + hex(impl->base) + " are bound");
    const auto index = static_cast<std::uint32_t>(impl->handlers.size() + 1);
    const auto offset = std::uint64_t{index} * SlotSize;
    // Double the mapped size, so a table of n entries needs only O(log n) guest mappings.
    if (offset + SlotSize > impl->mapped) impl->map(std::min(impl->mapped, impl->reserved - impl->mapped), false);
    const auto slot = impl->base + offset;
    // mov r11d, index ; jmp base
    const auto displacement = static_cast<std::uint32_t>(-static_cast<std::int64_t>(offset + 11));
    const std::array code{std::byte{0x41}, std::byte{0xbb},
        static_cast<std::byte>(index), static_cast<std::byte>(index >> 8),
        static_cast<std::byte>(index >> 16), static_cast<std::byte>(index >> 24),
        std::byte{0xe9}, static_cast<std::byte>(displacement), static_cast<std::byte>(displacement >> 8),
        static_cast<std::byte>(displacement >> 16), static_cast<std::byte>(displacement >> 24)};
    impl->machine.Write(slot, code);
    impl->handlers.push_back(std::move(handler));
    return slot;
}

void SceHostTrampolines::Release() {
    // Unmapping the first chunk also drops the gate and the machine's reference to the table.
    auto address = impl->base;
    for (const auto size : impl->chunks) {
        impl->machine.Unmap(address, size);
        address += size;
    }
    impl->chunks.clear();
    impl->mapped = 0;
    impl->handlers.clear();
}

std::size_t SceHostTrampolines::Size() const { return impl->handlers.size(); }
std::size_t SceHostTrampolines::Capacity() const { return impl->capacity; }

}
