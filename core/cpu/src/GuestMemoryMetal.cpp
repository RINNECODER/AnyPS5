#include <cpu/GuestMemoryMetal.hpp>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace Cpu {
namespace {

struct MappingLease {
    GuestMemorySnapshot snapshot;
    std::shared_ptr<const void> pinnedOwner;
};

void pinnedLifetime(std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
                    const std::shared_ptr<const void>& owner) {
    if (!ranges.empty() && !owner)
        throw std::invalid_argument("Pinned Metal guest ranges require their backing owner");
}

}

GuestMemoryMetalMappings BorrowGuestMemoryForMetal(const GuestMemorySnapshot& snapshot,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> pinnedRanges,
    std::shared_ptr<const void> pinnedOwner) {
    pinnedLifetime(pinnedRanges, pinnedOwner);
    if (!snapshot.Views.empty() && snapshot.Owners.empty())
        throw std::invalid_argument("Metal guest memory snapshot has no backing owners");
    for (const auto& owner : snapshot.Owners) if (!owner)
        throw std::invalid_argument("Metal guest memory snapshot contains an empty backing owner");
    GuestMemoryMetalMappings result;
    result.Ranges.reserve(pinnedRanges.size() + snapshot.Views.size());
    result.Ranges.insert(result.Ranges.end(), pinnedRanges.begin(), pinnedRanges.end());
    for (const auto& view : snapshot.Views) {
        if (view.Protection & ~0x37u)
            throw std::invalid_argument("Unsupported Metal guest memory protection bits");
        const auto gpu = view.Protection & 0x30u;
        if (gpu == 0x20u)
            throw std::invalid_argument("GPU write-only guest memory is not representable by Metal borrowed ranges");
        if (!gpu) continue;
        result.Ranges.push_back({view.Address, view.Bytes, (gpu & 0x20u) != 0, view.Identity});
    }
    std::sort(result.Ranges.begin(), result.Ranges.end(), [](const auto& a, const auto& b) {
        return a.guestAddress < b.guestAddress;
    });
    {
        AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(result.Ranges);
    }
    result.Owner = std::make_shared<MappingLease>(MappingLease{snapshot, std::move(pinnedOwner)});
    return result;
}

GuestMemoryRuntime::Transaction MakeGuestMemoryMetalTransaction(AgcDriver::Metal::MetalDriver& driver,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> pinnedRanges,
    std::shared_ptr<const void> pinnedOwner) {
    pinnedLifetime(pinnedRanges, pinnedOwner);
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> pinned(pinnedRanges.begin(), pinnedRanges.end());
    {
        AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(pinned);
    }
    return [&driver, pinned = std::move(pinned), owner = std::move(pinnedOwner)](
        const GuestMemorySnapshot& previous, const GuestMemorySnapshot& next,
        const std::function<void()>& mutateCpu) {
        auto before = BorrowGuestMemoryForMetal(previous, pinned, owner);
        auto after = BorrowGuestMemoryForMetal(next, pinned, owner);
        driver.MutateBorrowedRanges(after.Ranges, next.Generation, mutateCpu,
            std::move(before.Owner), std::move(after.Owner));
    };
}

namespace {

template<class A, class B>
bool sameOwner(const std::shared_ptr<A>& a, const std::shared_ptr<B>& b) {
    return a.get() == b.get() && !a.owner_before(b) && !b.owner_before(a);
}

bool sameSnapshot(const GuestMemorySnapshot& a, const GuestMemorySnapshot& b) {
    if (a.Generation != b.Generation || !sameOwner(a.Scope, b.Scope) ||
        !sameOwner(a.MachineScope, b.MachineScope) || a.Views.size() != b.Views.size() ||
        a.Owners.size() != b.Owners.size()) return false;
    for (std::size_t i = 0; i < a.Owners.size(); ++i)
        if (!sameOwner(a.Owners[i], b.Owners[i])) return false;
    for (std::size_t i = 0; i < a.Views.size(); ++i) {
        const auto& x = a.Views[i];
        const auto& y = b.Views[i];
        if (x.Address != y.Address || x.Bytes.data() != y.Bytes.data() || x.Bytes.size() != y.Bytes.size() ||
            x.Protection != y.Protection || x.Identity != y.Identity ||
            x.PhysicalId != y.PhysicalId || x.PhysicalOffset != y.PhysicalOffset) return false;
    }
    return true;
}

void validateSpan(std::uint64_t address, std::span<std::byte> bytes) {
    if (!address || bytes.empty() || !bytes.data() ||
        bytes.size() > std::numeric_limits<std::uint64_t>::max() - address ||
        bytes.size() > std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(bytes.data()))
        throw std::invalid_argument("Invalid compositor guest memory extent");
}

struct CompositeLease {
    OwnedMappingSnapshot owned;
    GuestMemorySnapshot dynamic;
};

// The shared invocation survives an escaped callback, but the CPU action is
// discarded on return. A foreign thread may only record/reject a violation.
struct PublicationInvocation {
    std::mutex mutex;
    const std::thread::id owner = std::this_thread::get_id();
    bool active = true, invoked = false, started = false, finished = false, violated = false;
    std::function<void()> preflight, action;

    void Invoke() {
        {
            std::lock_guard lock(mutex);
            if (!active || invoked || violated || std::this_thread::get_id() != owner) {
                violated = true;
                throw std::logic_error("Compositor CPU mutation must execute exactly once synchronously on its owner thread");
            }
            invoked = true;
        }
        preflight();
        {
            std::lock_guard lock(mutex);
            if (violated)
                throw std::logic_error("Compositor CPU mutation must execute exactly once synchronously on its owner thread");
            started = true;
        }
        action();
        std::lock_guard lock(mutex);
        finished = true;
    }
    void RejectReentry() {
        std::lock_guard lock(mutex);
        violated = true;
    }
    bool Close() {
        std::lock_guard lock(mutex);
        active = false;
        preflight = {};
        action = {};
        return finished && !violated;
    }
    bool Started() {
        std::lock_guard lock(mutex);
        return started;
    }
};

}

struct GuestMemoryMetalCompositor::Impl {
    Machine& machine;
    const std::thread::id owner = std::this_thread::get_id();
    OwnedMappingSnapshot selected;
    std::vector<std::uint64_t> writable;
    GuestMemorySnapshot accepted{};
    GuestMemoryMetalMappings initial;
    GuestMemoryMetalMappings current;
    // Each table is keyed within its one retained source Scope. The domains are
    // separate even when the source-local identity numbers are equal.
    std::unordered_map<std::uint64_t, std::uint64_t> ownedIds, dynamicIds;
    std::uint64_t lastIdentity = 0, generation = 1, sequence = 1;
    bool bound = false, poisoned = false;
    std::shared_ptr<PublicationInvocation> active;
    std::shared_ptr<const void> terminalPrevious, terminalNext;

    Impl(Machine& value, OwnedMappingSnapshot pins, std::span<const std::uint64_t> writes)
        : machine(value), selected(std::move(pins)), writable(writes.begin(), writes.end()) {
        if (!selected.Scope || !sameOwner(selected.Scope, machine.OwnedMappingScope()))
            throw std::invalid_argument("Compositor owned mapping scope is foreign or empty");
        machine.ValidateOwnedMappings(selected);
        std::sort(writable.begin(), writable.end());
        if (std::adjacent_find(writable.begin(), writable.end()) != writable.end())
            throw std::invalid_argument("Duplicate writable owned mapping address");
        for (const auto address : writable) {
            const auto found = std::find_if(selected.Views.begin(), selected.Views.end(),
                [address](const auto& view) { return view.Region.Address == address; });
            if (found == selected.Views.end() || (static_cast<unsigned>(found->Region.Permissions) & 3u) != 3u)
                throw std::invalid_argument("Writable owned mapping requires an exact selected readable/writable view");
        }
        for (const auto& view : selected.Views) {
            validateSpan(view.Region.Address, view.Bytes);
            if (!(static_cast<unsigned>(view.Region.Permissions) & 1u))
                throw std::invalid_argument("Selected owned mapping must be readable");
        }
        current = Compose(accepted);
    }

    void CheckOwner() const {
        if (std::this_thread::get_id() != owner)
            throw std::logic_error("Compositor must be used on its owner thread");
    }
    void Available() const {
        CheckOwner();
        if (active) {
            active->RejectReentry();
            throw std::logic_error("Compositor publication reentry is forbidden");
        }
        if (poisoned) throw std::runtime_error("Compositor publication poisoned; graphics shutdown required");
    }

    static void ValidateDynamic(const GuestMemorySnapshot& snapshot) {
        if (snapshot.Owners.size() < snapshot.Views.size())
            throw std::invalid_argument("Compositor runtime snapshot has no backing owners");
        for (const auto& retained : snapshot.Owners) if (!retained)
            throw std::invalid_argument("Compositor runtime snapshot contains an empty backing owner");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> extents;
        extents.reserve(snapshot.Views.size());
        for (const auto& view : snapshot.Views) {
            validateSpan(view.Address, view.Bytes);
            if (!view.Identity || view.Protection & ~0x37u ||
                view.PhysicalOffset > std::numeric_limits<std::uint64_t>::max() - view.Bytes.size())
                throw std::invalid_argument("Invalid compositor runtime snapshot metadata");
            if ((view.Protection & 0x30u) == 0x20u)
                throw std::invalid_argument("GPU write-only guest memory is not representable by Metal borrowed ranges");
            extents.emplace_back(view.Address, view.Address + view.Bytes.size());
        }
        std::sort(extents.begin(), extents.end());
        for (std::size_t i = 1; i < extents.size(); ++i)
            if (extents[i].first < extents[i - 1].second)
                throw std::invalid_argument("Compositor runtime snapshot extents overlap");
    }

    std::uint64_t Identity(std::unordered_map<std::uint64_t, std::uint64_t>& domain, std::uint64_t source) {
        if (!source) throw std::invalid_argument("Compositor backing identity is zero");
        const auto found = domain.find(source);
        if (found != domain.end()) return found->second;
        if (lastIdentity == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Compositor publication identity exhausted");
        const auto value = lastIdentity + 1;
        domain.emplace(source, value);
        lastIdentity = value;
        return value;
    }

    GuestMemoryMetalMappings Compose(const GuestMemorySnapshot& snapshot) {
        ValidateDynamic(snapshot);
        GuestMemoryMetalMappings result;
        result.Ranges.reserve(selected.Views.size() + snapshot.Views.size());
        for (const auto& view : selected.Views)
            result.Ranges.push_back({view.Region.Address, view.Bytes,
                std::binary_search(writable.begin(), writable.end(), view.Region.Address),
                Identity(ownedIds, view.BackingIdentity)});
        for (const auto& view : snapshot.Views) {
            if (!(view.Protection & 0x30u)) continue;
            result.Ranges.push_back({view.Address, view.Bytes, (view.Protection & 0x20u) != 0,
                Identity(dynamicIds, view.Identity)});
        }
        std::sort(result.Ranges.begin(), result.Ranges.end(), [](const auto& a, const auto& b) {
            return a.guestAddress < b.guestAddress;
        });
        {
            AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(result.Ranges);
        }
        // Reject overlap even for a CPU-only runtime span: it must not be able
        // to replace a selected owned binding behind a skipped GPU descriptor.
        for (const auto& dynamic : snapshot.Views) for (const auto& owned : selected.Views)
            if (dynamic.Address < owned.Region.Address + owned.Bytes.size() &&
                owned.Region.Address < dynamic.Address + dynamic.Bytes.size())
                throw std::invalid_argument("Compositor owned and runtime extents overlap");
        result.Owner = std::make_shared<CompositeLease>(CompositeLease{selected, snapshot});
        return result;
    }

    void Publish(const Publisher& publisher, const GuestMemorySnapshot& previous,
                 const GuestMemorySnapshot& next, const std::function<void()>& mutateCpu) {
        Available();
        if (!bound) throw std::logic_error("Compositor runtime scope has not been bound");
        if (!sameSnapshot(previous, accepted))
            throw std::invalid_argument("Compositor previous snapshot differs from accepted runtime state");
        if (!next.Scope || !sameOwner(next.Scope, accepted.Scope) ||
            !sameOwner(next.MachineScope, selected.Scope))
            throw std::invalid_argument("Compositor runtime scope is foreign or empty");
        if (next.Generation <= accepted.Generation)
            throw std::invalid_argument("Compositor runtime generation must increase");
        if (!mutateCpu) throw std::invalid_argument("Compositor CPU mutation is empty");
        machine.ValidateOwnedMappings(selected);
        auto after = Compose(next);
        auto acceptedNext = next;
        if (sequence == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Compositor publication generation exhausted");
        auto invocation = std::make_shared<PublicationInvocation>();
        invocation->preflight = [this] { machine.ValidateOwnedMappings(selected); };
        invocation->action = [this, mutateCpu] {
            mutateCpu();
            // Runtime mutation must preserve the frozen selected bindings. A
            // violating CPU action has already started, so this failure retains
            // both publications and poisons the compositor instead of accepting
            // descriptors that no longer match the actual Machine.
            machine.ValidateOwnedMappings(selected);
        };
        const std::function<void()> guarded = [invocation] { invocation->Invoke(); };
        const auto publication = ++sequence;
        active = invocation;
        try {
            publisher(after.Ranges, publication, guarded, current.Owner, after.Owner);
            if (!invocation->Close())
                throw std::logic_error("Compositor CPU mutation must execute exactly once synchronously on its owner thread");
        } catch (...) {
            invocation->Close();
            if (invocation->Started()) {
                poisoned = true;
                terminalPrevious = current.Owner;
                terminalNext = after.Owner;
            }
            active.reset();
            throw;
        }
        current.Ranges.swap(after.Ranges);
        current.Owner.swap(after.Owner);
        std::swap(accepted, acceptedNext);
        generation = publication;
        active.reset();
    }
};

GuestMemoryMetalCompositor::GuestMemoryMetalCompositor(Machine& machine, OwnedMappingSnapshot selected,
    std::span<const std::uint64_t> writableOwnedAddresses)
    : impl(std::make_unique<Impl>(machine, std::move(selected), writableOwnedAddresses)) {}
GuestMemoryMetalCompositor::~GuestMemoryMetalCompositor() = default;

void GuestMemoryMetalCompositor::BindRuntime(const GuestMemorySnapshot& initial) {
    impl->Available();
    if (impl->bound) throw std::logic_error("Compositor runtime scope is already bound");
    if (!initial.Scope || !sameOwner(initial.MachineScope, impl->selected.Scope))
        throw std::invalid_argument("Compositor runtime scope is foreign or empty");
    if (initial.Generation || !initial.Views.empty() || !initial.Owners.empty())
        throw std::invalid_argument("Compositor requires a freshly empty runtime snapshot");
    impl->machine.ValidateOwnedMappings(impl->selected);
    auto mappings = impl->Compose(initial);
    auto snapshot = initial;
    auto initialMappings = mappings;
    impl->current.Ranges.swap(mappings.Ranges);
    impl->current.Owner.swap(mappings.Owner);
    std::swap(impl->accepted, snapshot);
    impl->initial.Ranges.swap(initialMappings.Ranges);
    impl->initial.Owner.swap(initialMappings.Owner);
    impl->bound = true;
}

GuestMemoryMetalMappings GuestMemoryMetalCompositor::InitialMappings() const {
    impl->Available();
    if (!impl->bound) throw std::logic_error("Compositor runtime scope has not been bound");
    if (impl->generation != 1)
        throw std::logic_error("Compositor initial mappings are only available before dynamic publication");
    impl->machine.ValidateOwnedMappings(impl->selected);
    return impl->initial;
}

std::uint64_t GuestMemoryMetalCompositor::Generation() const {
    impl->CheckOwner();
    return impl->generation;
}

GuestMemoryRuntime::Transaction GuestMemoryMetalCompositor::MakeTransaction(Publisher publisher) {
    impl->Available();
    if (!publisher) throw std::invalid_argument("Compositor publisher is empty");
    return [this, publisher = std::move(publisher)](const GuestMemorySnapshot& previous,
        const GuestMemorySnapshot& next, const std::function<void()>& mutateCpu) {
        impl->Publish(publisher, previous, next, mutateCpu);
    };
}

}
