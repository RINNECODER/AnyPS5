// A runtime mprotect that reaches a pinned Machine mapping (MEM-29) must not
// nest the owned-mapping publication inside the runtime publication: the
// compositor forbids publication reentry, so each part publishes on its own.
#include <cpu/Cpu.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Cpu::Permission;
using Compositor = Cpu::GuestMemoryMetalCompositor;
using Access = Compositor::OwnedGpuAccess;
constexpr std::uint64_t page = 0x4000;
constexpr std::uint64_t first = 0x1000000000;
constexpr std::uint64_t pinned = first + 0x40000;
constexpr auto rw = Permission::Read | Permission::Write;

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

bool accessible(Cpu::Machine& machine, std::uint64_t address, Permission permission) {
    try { machine.CheckAccess(address, 1, permission); return true; } catch (...) { return false; }
}

Access select(const Cpu::OwnedMappingView& view) {
    const auto bits = static_cast<unsigned>(view.Region.Permissions);
    if (!(bits & 1u)) return Access::CpuOnly;
    return (bits & 2u) ? Access::ReadWrite : Access::ReadOnly;
}

struct Fixture {
    Cpu::Machine machine;
    Cpu::GuestMemoryRuntime::Transaction runtimeTransaction;
    bool drained = false;
    Cpu::GuestMemoryRuntime runtime{machine, 8 * page, [this](const auto& previous, const auto& next, const auto& mutate) {
        if (drained) { mutate(); return; }
        require(static_cast<bool>(runtimeTransaction), "Runtime mutation before compositor binding");
        runtimeTransaction(previous, next, mutate);
    }};
    std::unique_ptr<Compositor> compositor;
    unsigned depth = 0, publications = 0;

    Fixture() {
        machine.Map(pinned, page, rw); // a module data segment the runtime does not own
        auto selected = machine.PinOwnedMappings();
        std::erase_if(selected.Views, [](const auto& view) { return select(view) == Access::CpuOnly; });
        std::vector<std::uint64_t> writable;
        for (const auto& view : selected.Views) if (select(view) == Access::ReadWrite) writable.push_back(view.Region.Address);
        compositor = std::make_unique<Compositor>(machine, std::move(selected), writable);
        compositor->BindRuntime(runtime.Snapshot());
        // Stands in for SceNativeGraphicsSession::MutateBorrowedRanges: one
        // synchronous CPU commit per publication, and never a nested one.
        Compositor::Publisher publish = [this](auto, auto, const auto& commit, auto, auto) {
            require(depth == 0, "Nested native mapping publication");
            ++depth;
            try { commit(); } catch (...) { --depth; throw; }
            --depth;
            ++publications;
        };
        runtimeTransaction = compositor->MakeTransaction(publish);
        machine.SetOwnedMappingTransaction(compositor->MakeOwnedTransaction(publish, select));
    }
    ~Fixture() {
        machine.SetOwnedMappingTransaction({});
        drained = true;
        try { runtime.Shutdown(); } catch (...) {}
    }
};

void protectPinnedOnly() {
    Fixture f;
    const auto before = f.compositor->Generation();
    f.runtime.Protect(pinned, page, 1);
    require(accessible(f.machine, pinned, Permission::Read) && !accessible(f.machine, pinned, Permission::Write),
            "Mprotect did not reach the pinned Machine mapping under a live compositor");
    require(f.compositor->Generation() > before, "Pinned protection did not publish the owned GPU access change");
    f.runtime.Protect(pinned, page, 3);
    require(accessible(f.machine, pinned, Permission::Write), "Pinned mapping did not regain write access");
}

void protectRuntimeAndPinned() {
    Fixture f;
    f.runtime.MapFlexible(pinned - page, page, 3, 0x10);
    const auto publications = f.publications;
    f.runtime.Protect(pinned - page, 2 * page, 1);
    require(f.runtime.Query(pinned - page).Protection == 1, "Runtime half of a mixed mprotect was not recorded");
    for (const auto address : {pinned - page, pinned})
        require(accessible(f.machine, address, Permission::Read) && !accessible(f.machine, address, Permission::Write),
                "Mixed mprotect left a page writable");
    require(f.publications >= publications + 2, "Mixed mprotect did not publish both runtime and owned changes");
    f.runtime.Protect(pinned - page, 2 * page, 3);
    for (const auto address : {pinned - page, pinned})
        require(accessible(f.machine, address, Permission::Write), "Mixed mprotect did not restore write access");
}
}

int main() {
    try {
        protectPinnedOnly();
        protectRuntimeAndPinned();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "compositor pinned protect passed\n";
}
