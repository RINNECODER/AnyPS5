#include <cpu/GuestMemoryRuntime.hpp>
#include <algorithm>
#include <limits>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>

namespace Cpu {
namespace {
constexpr std::uint64_t page = GuestMemoryRuntime::PageSize;
constexpr std::uint64_t virtualStart = 0x1000000000;
constexpr std::uint64_t virtualLimit = 0x0000800000000000;
constexpr std::uint32_t fixed = 0x10;
constexpr std::uint32_t noOverwrite = 0x80;
struct RuntimeScope {};

std::shared_ptr<const void> machineScope(Machine& machine) {
#if ANYPS5_CPU_MODERN_TCG
    return machine.OwnedMappingScope();
#else
    (void)machine;
    return {};
#endif
}

[[noreturn]] void error(unsigned code, const char* message) { throw GuestMemoryError(code, message); }
std::uint64_t end(std::uint64_t address, std::uint64_t size) {
    if (!size || size > std::numeric_limits<std::uint64_t>::max() - address) error(22, "Invalid guest memory extent");
    return address + size;
}
void length(std::uint64_t size) {
    if (!size || size % page || size > std::numeric_limits<std::size_t>::max()) error(22, "Invalid guest memory size");
}
std::uint64_t alignment(std::uint64_t value) {
    if (!value) return page;
    if (value < page || (value & (value - 1))) error(22, "Invalid guest memory alignment");
    return value;
}
std::uint64_t aligned(std::uint64_t value, std::uint64_t align) {
    if (value > std::numeric_limits<std::uint64_t>::max() - (align - 1)) error(22, "Guest memory alignment overflows");
    return (value + align - 1) & ~(align - 1);
}
Permission permissions(std::uint32_t protection) {
    // EINVAL, not a host abort: mprotect/mmap with a protection combination this kernel
    // cannot express is a request the guest can handle.
    if (protection & ~0x37u) throw GuestMemoryError(22, "Unsupported guest memory protection bits: " + std::to_string(protection));
    const auto cpu = protection & 7;
    return static_cast<Permission>(cpu | ((cpu & 2) ? 1 : 0));
}
void flags(std::uint32_t value) {
    if (value & ~(fixed | noOverwrite))
        throw GuestMemoryError(22, "Unsupported guest memory mapping flags: " + std::to_string(value));
    if ((value & noOverwrite) && !(value & fixed))
        throw GuestMemoryError(22, "Unsupported guest memory no-overwrite placement without fixed address");
}
bool overlaps(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) { return a < d && c < b; }
struct Storage {
    std::byte* data;
    std::size_t size;
    explicit Storage(std::uint64_t bytes) : data(nullptr), size(static_cast<std::size_t>(bytes)) {
        const auto hostPage = ::sysconf(_SC_PAGESIZE);
        if (hostPage <= 0 || page % static_cast<std::uint64_t>(hostPage))
            throw std::runtime_error("Unsupported guest memory host page size");
        auto* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) error(12, "Cannot allocate guest physical backing");
        data = static_cast<std::byte*>(mapped);
        if (reinterpret_cast<std::uintptr_t>(data) % static_cast<std::uint64_t>(hostPage)) std::terminate();
    }
    ~Storage() { if (::munmap(data, size)) std::terminate(); }
};
template<class T> void little(std::span<std::byte> bytes, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) bytes[offset + i] = std::byte((static_cast<std::uint64_t>(value) >> (8 * i)) & 255);
}
}

GuestMemoryError::GuestMemoryError(unsigned code, const std::string& message)
    : std::runtime_error(message), result(static_cast<std::int64_t>(static_cast<std::int32_t>(0x80020000u | code))) {}
std::int64_t GuestMemoryError::Result() const { return result; }
std::array<std::byte, 72> GuestMemoryQuery::Serialize() const {
    std::array<std::byte, 72> bytes{};
    little(std::span(bytes), 0, Start); little(std::span(bytes), 8, End); little(std::span(bytes), 16, Offset);
    little(std::span(bytes), 24, Protection); little(std::span(bytes), 28, MemoryType); little(std::span(bytes), 32, Flags);
    for (std::size_t i = 0; i < Name.size(); ++i) bytes[36 + i] = std::byte(static_cast<unsigned char>(Name[i]));
    bytes[68] = std::byte(GpuMaskId);
    return bytes;
}

struct GuestMemoryRuntime::Impl {
    struct Physical {
        std::uint64_t first, last, offset, id;
        std::int32_t type;
        std::shared_ptr<Storage> owner;
    };
    enum class Kind { Direct, Flexible, Reserved };
    struct Region {
        std::uint64_t first, last, allocationFirst, allocationLast, offset, physical, id, identity;
        std::uint32_t protection;
        std::int32_t type;
        Kind kind;
        std::shared_ptr<Storage> owner;
    };
    Machine& machine;
    const std::shared_ptr<const void> scope = std::make_shared<const RuntimeScope>();
    const std::shared_ptr<const void> ownedMachineScope;
    const std::uint64_t capacity;
    Transaction transaction;
    std::vector<Physical> physical;
    std::vector<Region> regions;
    std::vector<Region> terminalRegions;
    std::vector<std::shared_ptr<void>> terminalOwners;
    std::uint64_t generation = 0, nextGeneration = 0, nextId = 0, nextIdentity = 0;
    bool failed = false, closed = false, transactionInProgress = false;

    struct MutationScope {
        bool& active;
        explicit MutationScope(bool& value) : active(value) {
            if (active) throw std::logic_error("Guest memory transaction is already in progress");
            active = true;
        }
        ~MutationScope() { active = false; }
        MutationScope(const MutationScope&) = delete;
        MutationScope& operator=(const MutationScope&) = delete;
    };

    Impl(Machine& value, std::uint64_t bytes, Transaction callback)
        : machine(value), ownedMachineScope(machineScope(value)), capacity(bytes), transaction(std::move(callback)) {
        length(bytes);
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) error(22, "Invalid guest physical capacity");
        machine.Mappings();
    }
    void live() const {
        machine.Mappings();
        if (failed) throw std::runtime_error("Guest memory transaction failed after CPU mutation; shutdown required");
        if (closed) throw std::runtime_error("Guest memory runtime is shut down");
    }
    GuestMemorySnapshot snapshot(const std::vector<Region>& mappings, const std::vector<Physical>& allocations,
                                 std::uint64_t value) const {
        GuestMemorySnapshot result{value, {}, {}, scope, ownedMachineScope};
        result.Views.reserve(mappings.size());
        result.Owners.reserve(mappings.size() + allocations.size());
        for (const auto& region : mappings) if (region.owner) {
            result.Views.push_back({region.first, {region.owner->data + region.offset, static_cast<std::size_t>(region.last - region.first)},
                region.protection, region.identity, region.id, region.offset});
            result.Owners.push_back(region.owner);
        }
        for (const auto& allocation : allocations) result.Owners.push_back(allocation.owner);
        return result;
    }
    void commit(std::vector<Region> mappings, std::vector<Physical> allocations, std::function<void()> action) {
        if (nextGeneration == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Guest memory generation exhausted");
        const auto next = ++nextGeneration;
        auto previous = snapshot(regions, physical, generation);
        auto candidate = snapshot(mappings, allocations, next);
        auto retainedRegions = regions;
        retainedRegions.insert(retainedRegions.end(), mappings.begin(), mappings.end());
        auto retainedOwners = previous.Owners;
        retainedOwners.insert(retainedOwners.end(), candidate.Owners.begin(), candidate.Owners.end());
        struct Invocation { bool active = true, started = false, finished = false; };
        auto invocation = std::make_shared<Invocation>();
        std::function<void()> mutation = [invocation, action = std::move(action)] {
            if (!invocation->active || invocation->started) throw std::logic_error("Guest memory CPU mutation must execute exactly once synchronously");
            invocation->started = true;
            action();
            invocation->finished = true;
        };
        try {
            if (transaction) transaction(previous, candidate, mutation);
            else mutation();
            invocation->active = false;
            if (!invocation->finished) throw std::logic_error("Guest memory transaction did not execute CPU mutation");
        } catch (...) {
            invocation->active = false;
            if (invocation->started) {
                failed = true;
                terminalRegions.swap(retainedRegions);
                terminalOwners.swap(retainedOwners);
            }
            throw;
        }
        regions.swap(mappings); physical.swap(allocations); generation = next;
    }
    std::pair<std::uint64_t, std::uint64_t> search(std::int64_t start, std::int64_t stop, std::uint64_t align) const {
        if (start < 0 || stop <= start) error(22, "Invalid guest physical search range");
        return {aligned(static_cast<std::uint64_t>(start), align), std::min(static_cast<std::uint64_t>(stop), capacity)};
    }
    GuestPhysicalExtent available(std::int64_t start, std::int64_t stop, std::uint64_t align) const {
        const auto bounds = search(start, stop, align);
        auto cursor = bounds.first;
        for (const auto& block : physical) {
            if (block.last <= cursor) continue;
            if (block.first >= bounds.second) break;
            if (block.first > cursor) {
                const auto bytes = (std::min(block.first, bounds.second) - cursor) & ~(page - 1);
                if (bytes) return {cursor, bytes};
            }
            cursor = aligned(block.last, align);
            if (cursor >= bounds.second) break;
        }
        if (cursor < bounds.second && bounds.second - cursor >= page) return {cursor, (bounds.second - cursor) & ~(page - 1)};
        error(35, "Guest direct memory is exhausted");
    }
    std::uint64_t place(std::uint64_t hint, std::uint64_t size, std::uint32_t placement, std::uint64_t align) const {
        flags(placement);
        if ((placement & fixed) && (!hint || hint % align)) error(22, "Invalid fixed guest memory address");
        auto cursor = aligned(hint ? hint : virtualStart, align);
        end(cursor, size);
        const auto machineMappings = machine.Mappings();
        std::vector<std::pair<std::uint64_t, std::uint64_t>> occupied;
        occupied.reserve(machineMappings.size() + regions.size());
        for (const auto& mapping : machineMappings) occupied.emplace_back(mapping.Address, end(mapping.Address, mapping.Size));
        for (const auto& region : regions) occupied.emplace_back(region.first, region.last);
        std::sort(occupied.begin(), occupied.end());
        for (const auto& range : occupied) {
            if (range.second <= cursor) continue;
            if (size <= std::numeric_limits<std::uint64_t>::max() - cursor && cursor + size <= range.first) break;
            if (overlaps(cursor, end(cursor, size), range.first, range.second)) {
                if (placement & fixed) {
                    const auto own = std::any_of(regions.begin(), regions.end(), [&](const Region& region) {
                        return overlaps(cursor, cursor + size, region.first, region.last);
                    });
                    if (own && !(placement & noOverwrite))
                        throw GuestMemoryError(45, "Unsupported guest memory fixed replacement policy");
                    error(17, "Guest memory address is occupied");
                }
                cursor = aligned(range.second, align);
            }
        }
        if (!cursor || cursor >= virtualLimit || size > virtualLimit - cursor) error(12, "Guest virtual address space is exhausted");
        return cursor;
    }
    void output(std::uint64_t address, std::uint64_t start, std::uint64_t stop, std::uint32_t protection) const {
        if (!address) return;
        try { machine.CheckAccess(address, 8, Permission::Write); }
        catch (const std::exception&) { error(14, "Guest memory output is not writable"); }
        if (overlaps(address, end(address, 8), start, stop) && !(protection & 2)) error(14, "Guest memory output loses write permission");
    }
    std::vector<Region> covered(std::uint64_t start, std::uint64_t stop) const {
        std::vector<Region> result;
        auto cursor = start;
        for (const auto& region : regions) {
            if (region.last <= cursor) continue;
            if (region.first > cursor) break;
            auto part = region;
            part.first = cursor; part.last = std::min(stop, region.last);
            part.offset += cursor - region.first;
            if (part.kind == Kind::Direct) part.physical += cursor - region.first;
            result.push_back(std::move(part));
            cursor = std::min(stop, region.last);
            if (cursor == stop) return result;
        }
        error(13, "Guest memory range is not owned by the runtime");
    }
    static std::vector<Region> remove(const std::vector<Region>& source, std::uint64_t start, std::uint64_t stop) {
        std::vector<Region> result;
        result.reserve(source.size() + 1);
        for (const auto& region : source) {
            if (!overlaps(start, stop, region.first, region.last)) { result.push_back(region); continue; }
            if (region.first < start) { auto left = region; left.last = start; result.push_back(std::move(left)); }
            if (region.last > stop) {
                auto right = region; right.offset += stop - region.first;
                if (right.kind == Kind::Direct) right.physical += stop - region.first;
                right.first = stop; result.push_back(std::move(right));
            }
        }
        return result;
    }
    static void sort(std::vector<Region>& values) { std::sort(values.begin(), values.end(), [](const Region& a, const Region& b) { return a.first < b.first; }); }
};

GuestMemoryRuntime::GuestMemoryRuntime(Machine& machine, std::uint64_t capacity, Transaction transaction)
    : impl(std::make_unique<Impl>(machine, capacity, std::move(transaction))) {}
GuestMemoryRuntime::~GuestMemoryRuntime() { try { Shutdown(); } catch (...) { if (!impl->closed) std::terminate(); } }
std::uint64_t GuestMemoryRuntime::DirectMemorySize() const { impl->live(); return impl->capacity; }
GuestPhysicalExtent GuestMemoryRuntime::AvailableDirect(std::int64_t start, std::int64_t stop, std::uint64_t align) const {
    impl->live(); return impl->available(start, stop, alignment(align));
}
std::uint64_t GuestMemoryRuntime::AllocateDirect(std::int64_t start, std::int64_t stop, std::uint64_t bytes,
                                                std::uint64_t align, std::int32_t type) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); align = alignment(align);
    if (type != 0 && type != 12) throw GuestMemoryError(22, "Unsupported guest memory type: " + std::to_string(type));
    const auto bounds = impl->search(start, stop, align);
    auto cursor = bounds.first;
    for (const auto& block : impl->physical) {
        if (block.last <= cursor) continue;
        if (block.first >= bounds.second || (cursor <= block.first && bytes <= block.first - cursor)) break;
        cursor = aligned(block.last, align);
    }
    if (cursor >= bounds.second || bytes > bounds.second - cursor) error(35, "Guest direct memory is exhausted");
    auto owner = std::make_shared<Storage>(bytes);
    auto candidate = impl->physical;
    candidate.push_back({cursor, cursor + bytes, 0, ++impl->nextId, type, std::move(owner)});
    std::sort(candidate.begin(), candidate.end(), [](const Impl::Physical& a, const Impl::Physical& b) { return a.first < b.first; });
    impl->commit(impl->regions, std::move(candidate), [] {});
    return cursor;
}
std::uint64_t GuestMemoryRuntime::MapDirect(std::uint64_t hint, std::uint64_t bytes, std::uint32_t protection,
                                          std::uint32_t placement, std::int64_t physical, std::uint64_t align,
                                          std::uint64_t outputAddress) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); const auto cpu = permissions(protection); align = alignment(align);
    if (protection & 4) error(22, "Guest direct memory cannot be mapped executable");
    if (physical < 0 || static_cast<std::uint64_t>(physical) % page) error(22, "Invalid guest physical mapping address");
    const auto first = static_cast<std::uint64_t>(physical), stop = end(first, bytes);
    if (stop > impl->capacity) error(22, "Guest physical mapping exceeds capacity");
    const auto address = impl->place(hint, bytes, placement, align);
    impl->output(outputAddress, address, address + bytes, protection);
    auto candidate = impl->regions;
    std::vector<Impl::Region> added;
    auto cursor = first;
    for (const auto& block : impl->physical) {
        if (block.last <= cursor) continue;
        if (block.first > cursor) break;
        const auto last = std::min(stop, block.last);
        const auto va = address + cursor - first;
        added.push_back({va, va + last - cursor, address, address + bytes, block.offset + cursor - block.first,
            cursor, block.id, ++impl->nextIdentity, protection, block.type, Impl::Kind::Direct, block.owner});
        cursor = last;
        if (cursor == stop) break;
    }
    if (cursor != stop) error(22, "Guest direct mapping references unallocated physical memory");
    candidate.insert(candidate.end(), added.begin(), added.end()); Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), added, cpu] {
        for (const auto& region : added) state->machine.MapBorrowed(region.first,
            {region.owner->data + region.offset, static_cast<std::size_t>(region.last - region.first)}, cpu,
            {region.owner->data, region.owner->size});
    });
    return address;
}
std::uint64_t GuestMemoryRuntime::MapFlexible(std::uint64_t hint, std::uint64_t bytes, std::uint32_t protection,
                                            std::uint32_t placement, std::uint64_t outputAddress) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); const auto cpu = permissions(protection);
    const auto address = impl->place(hint, bytes, placement, page);
    impl->output(outputAddress, address, address + bytes, protection);
    auto owner = std::make_shared<Storage>(bytes);
    auto candidate = impl->regions;
    candidate.push_back({address, address + bytes, address, address + bytes, 0, 0, ++impl->nextId, ++impl->nextIdentity,
                         protection, 0, Impl::Kind::Flexible, owner}); Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), address, bytes, owner, cpu] {
        state->machine.MapBorrowed(address, {owner->data, static_cast<std::size_t>(bytes)}, cpu, {owner->data, owner->size});
    });
    return address;
}
std::uint64_t GuestMemoryRuntime::Reserve(std::uint64_t hint, std::uint64_t bytes, std::uint32_t placement,
                                        std::uint64_t align, std::uint64_t outputAddress) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); const auto address = impl->place(hint, bytes, placement, alignment(align));
    impl->output(outputAddress, address, address + bytes, 0);
    auto candidate = impl->regions;
    candidate.push_back({address, address + bytes, address, address + bytes, 0, 0, 0, ++impl->nextIdentity, 0, 0, Impl::Kind::Reserved, {}});
    Impl::sort(candidate); impl->commit(std::move(candidate), impl->physical, [] {}); return address;
}
void GuestMemoryRuntime::Protect(std::uint64_t address, std::uint64_t bytes, std::uint32_t protection) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    if (!address) error(22, "Invalid guest protection address");
    const auto first = address & ~(page - 1);
    const auto stop = aligned(end(address, bytes), page);
    const auto size = stop - first;
    if (size > std::numeric_limits<std::size_t>::max()) error(22, "Invalid guest protection size");
    const auto cpu = permissions(protection);
    auto affected = impl->covered(first, stop);
    auto candidate = Impl::remove(impl->regions, first, stop);
    for (auto& region : affected) {
        if (!region.owner) throw GuestMemoryError(45, "Unsupported guest memory protection of uncommitted reservation");
        region.protection = protection; region.identity = ++impl->nextIdentity; candidate.push_back(region);
    }
    Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), first, size, cpu] {
        state->machine.Protect(first, static_cast<std::size_t>(size), cpu);
    });
}
void GuestMemoryRuntime::Unmap(std::uint64_t address, std::uint64_t bytes) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); if (!address || address % page) error(22, "Invalid guest unmap address");
    const auto stop = end(address, bytes); const auto affected = impl->covered(address, stop);
    auto candidate = Impl::remove(impl->regions, address, stop);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), affected] {
        for (const auto& region : affected) if (region.owner)
            state->machine.Unmap(region.first, static_cast<std::size_t>(region.last - region.first));
    });
}
void GuestMemoryRuntime::ReleaseDirect(std::int64_t physical, std::uint64_t bytes) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes);
    if (physical < 0 || static_cast<std::uint64_t>(physical) % page) error(22, "Invalid guest physical release address");
    const auto first = static_cast<std::uint64_t>(physical), stop = end(first, bytes);
    for (const auto& region : impl->regions) if (region.kind == Impl::Kind::Direct &&
        overlaps(first, stop, region.physical, region.physical + region.last - region.first))
        throw GuestMemoryError(16, "Unsupported guest memory release of mapped physical allocation");
    auto cursor = first; std::vector<Impl::Physical> candidate;
    candidate.reserve(impl->physical.size() + 1);
    for (const auto& block : impl->physical) {
        if (!overlaps(first, stop, block.first, block.last)) { candidate.push_back(block); continue; }
        if (block.first > cursor) error(22, "Guest physical release contains an unallocated gap");
        cursor = std::min(stop, block.last);
        if (block.first < first) { auto left = block; left.last = first; candidate.push_back(std::move(left)); }
        if (block.last > stop) { auto right = block; right.offset += stop - block.first; right.first = stop; candidate.push_back(std::move(right)); }
    }
    if (cursor != stop) error(22, "Guest physical release is not allocated");
    impl->commit(impl->regions, std::move(candidate), [] {});
}
GuestMemoryQuery GuestMemoryRuntime::Query(std::uint64_t address, bool findNext) const {
    impl->live();
    std::optional<GuestMemoryQuery> best;
    for (const auto& region : impl->regions) {
        if (address >= region.first && address < region.last)
            return {region.first, region.last, region.kind == Impl::Kind::Direct ? region.physical : 0,
                region.protection, region.type, static_cast<std::uint32_t>((region.kind == Impl::Kind::Flexible ? 1 : 0) |
                    (region.kind == Impl::Kind::Direct ? 2 : 0) | (region.owner ? 16 : 0))};
        if (findNext && region.first > address && (!best || region.first < best->Start))
            best = GuestMemoryQuery{region.first, region.last, region.kind == Impl::Kind::Direct ? region.physical : 0,
                region.protection, region.type, static_cast<std::uint32_t>((region.kind == Impl::Kind::Flexible ? 1 : 0) |
                    (region.kind == Impl::Kind::Direct ? 2 : 0) | (region.owner ? 16 : 0))};
    }
    const auto mappings = impl->machine.Mappings();
    for (const auto& mapping : mappings) {
        const auto last = end(mapping.Address, mapping.Size);
        if (address >= mapping.Address && address < last)
            return {mapping.Address, last, 0, static_cast<unsigned>(mapping.Permissions), 0, 16};
        if (findNext && mapping.Address > address && (!best || mapping.Address < best->Start))
            best = GuestMemoryQuery{mapping.Address, last, 0, static_cast<unsigned>(mapping.Permissions), 0, 16};
    }
    if (best) return *best;
    error(13, "Guest virtual query address is unmapped");
}
GuestMemorySnapshot GuestMemoryRuntime::Snapshot() const {
    impl->machine.Mappings();
    auto result = impl->snapshot(impl->regions, impl->physical, impl->generation);
    result.Owners.insert(result.Owners.end(), impl->terminalOwners.begin(), impl->terminalOwners.end());
    return result;
}
void GuestMemoryRuntime::Shutdown() {
    impl->machine.Mappings(); Impl::MutationScope operation(impl->transactionInProgress);
    if (impl->closed) return;
    auto owned = impl->regions;
    owned.insert(owned.end(), impl->terminalRegions.begin(), impl->terminalRegions.end());
    const auto mappings = impl->machine.Mappings();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> remove;
    for (const auto& mapping : mappings) for (const auto& region : owned) {
        if (!region.owner || !overlaps(mapping.Address, mapping.Address + mapping.Size, region.first, region.last)) continue;
        remove.emplace_back(std::max(mapping.Address, region.first), std::min(mapping.Address + mapping.Size, region.last));
    }
    std::sort(remove.begin(), remove.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> merged;
    for (const auto& range : remove) {
        if (!merged.empty() && range.first <= merged.back().second) merged.back().second = std::max(merged.back().second, range.second);
        else merged.push_back(range);
    }
    auto previous = Snapshot();
    if (impl->nextGeneration == std::numeric_limits<std::uint64_t>::max()) throw std::runtime_error("Guest memory generation exhausted");
    GuestMemorySnapshot next{++impl->nextGeneration, {}, {}, impl->scope, impl->ownedMachineScope};
    struct Invocation { bool active = true, called = false, finished = false; };
    auto invocation = std::make_shared<Invocation>();
    std::function<void()> mutation = [state = impl.get(), merged, invocation] {
        if (!invocation->active || invocation->called) throw std::logic_error("Guest memory shutdown CPU mutation must execute once synchronously");
        invocation->called = true;
        for (const auto& range : merged) state->machine.Unmap(range.first, static_cast<std::size_t>(range.second - range.first));
        invocation->finished = true;
    };
    std::exception_ptr failure;
    try {
        if (impl->transaction && !impl->failed) impl->transaction(previous, next, mutation); else mutation();
        if (!invocation->finished) throw std::logic_error("Guest memory shutdown did not execute CPU mutation");
    } catch (...) {
        failure = std::current_exception();
        if (!invocation->called) mutation();
        if (!invocation->finished) { invocation->active = false; std::rethrow_exception(failure); }
    }
    invocation->active = false;
    impl->regions.clear(); impl->physical.clear(); impl->terminalRegions.clear(); impl->terminalOwners.clear();
    impl->generation = next.Generation; impl->closed = true;
    if (failure) std::rethrow_exception(failure);
}

}
