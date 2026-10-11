#include <cpu/GuestMemoryRuntime.hpp>
#include <algorithm>
#include <limits>
#include <map>
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
constexpr std::uint32_t noCoalesce = 0x400000;
// MAP_ALIGNED(n) keeps log2 of the requested alignment in the top byte; n = 1 is MAP_ALIGNED_SUPER.
constexpr std::uint32_t alignedMask = 0xff000000u;
// CPU read/write/execute, GPU read/write and the remaining documented protection attribute bits.
constexpr std::uint32_t protectionMask = 0x3f7;
using Span = std::pair<std::uint64_t, std::uint64_t>;
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
    // cannot express is a request the guest can handle. The raw bits are kept for queries;
    // only the CPU view is enforced (0x100/0x200 are CPU read/write aliases, as upstream).
    if (protection & ~protectionMask)
        throw GuestMemoryError(22, "Unsupported guest memory protection bits: " + std::to_string(protection));
    auto cpu = protection & 7;
    if (protection & 0x100) cpu |= 1;
    if (protection & 0x200) cpu |= 2;
    return static_cast<Permission>(cpu | ((cpu & 2) ? 1 : 0));
}
// Validates placement flags and returns the alignment MAP_ALIGNED asks for (0 for none).
// NO_OVERWRITE without FIXED leaves the address a plain hint; NO_COALESCE needs no action
// because the runtime never merges neighbouring mappings.
std::uint64_t flags(std::uint32_t value) {
    if (value & ~(fixed | noOverwrite | noCoalesce | alignedMask))
        throw GuestMemoryError(22, "Unsupported guest memory mapping flags: " + std::to_string(value));
    const auto shift = value >> 24;
    if (!shift) return 0;
    if (shift == 1) return 0x200000;
    if (shift < 14 || shift > 46)
        throw GuestMemoryError(22, "Unsupported guest memory MAP_ALIGNED request: " + std::to_string(shift));
    return std::uint64_t{1} << shift;
}
bool overlaps(std::uint64_t a, std::uint64_t b, std::uint64_t c, std::uint64_t d) { return a < d && c < b; }
// The parts of each span in `from` (sorted, disjoint) that no span in `minus` covers.
std::vector<Span> subtract(const std::vector<Span>& from, std::vector<Span> minus) {
    std::sort(minus.begin(), minus.end());
    std::vector<Span> result;
    for (const auto& [start, stop] : from) {
        auto cursor = start;
        for (const auto& [low, high] : minus) {
            if (high <= cursor) continue;
            if (low >= stop) break;
            if (low > cursor) result.emplace_back(cursor, low);
            cursor = std::max(cursor, high);
            if (cursor >= stop) break;
        }
        if (cursor < stop) result.emplace_back(cursor, stop);
    }
    return result;
}
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
    enum class Kind { Direct, Flexible, Reserved, Pooled, PoolReserved };
    struct Region {
        std::uint64_t first, last, allocationFirst, allocationLast, offset, physical, id, identity;
        std::uint32_t protection;
        std::int32_t type;
        Kind kind;
        std::shared_ptr<Storage> owner;
    };
    struct Name { std::uint64_t last; std::string text; };
    Machine& machine;
    const std::shared_ptr<const void> scope = std::make_shared<const RuntimeScope>();
    const std::shared_ptr<const void> ownedMachineScope;
    const std::uint64_t capacity;
    const std::uint64_t flexibleCapacity;
    // Physical pages handed to the memory pool by PoolExpand (sorted, disjoint).
    std::vector<Span> poolPhysical;
    Transaction transaction;
    std::vector<Physical> physical;
    std::vector<Region> regions;
    std::vector<Region> terminalRegions;
    std::vector<std::shared_ptr<void>> terminalOwners;
    std::map<std::uint64_t, Name> names;
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

    Impl(Machine& value, std::uint64_t bytes, Transaction callback, std::uint64_t flexibleBytes)
        : machine(value), ownedMachineScope(machineScope(value)), capacity(bytes), flexibleCapacity(flexibleBytes),
          transaction(std::move(callback)) {
        length(bytes);
        if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) error(22, "Invalid guest physical capacity");
        if (flexibleBytes % page) error(22, "Invalid guest flexible memory capacity");
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
        // MEM-14: the largest free run, measured from its first aligned page, not the first gap.
        const auto bounds = search(start, stop, align);
        GuestPhysicalExtent best{0, 0};
        const auto consider = [&](std::uint64_t gapFirst, std::uint64_t gapLast) {
            gapLast = std::min(gapLast, bounds.second);
            if (std::max(gapFirst, bounds.first) >= gapLast) return;
            const auto from = aligned(std::max(gapFirst, bounds.first), align);
            if (from >= gapLast) return;
            const auto bytes = (gapLast - from) & ~(page - 1);
            if (bytes > best.Size) best = {from, bytes};
        };
        std::uint64_t cursor = 0;
        for (const auto& block : physical) {
            if (block.first > cursor) consider(cursor, block.first);
            cursor = std::max(cursor, block.last);
        }
        consider(cursor, capacity);
        if (!best.Size) error(35, "Guest direct memory is exhausted");
        return best;
    }
    std::uint64_t poolCapacity() const {
        std::uint64_t total = 0;
        for (const auto& [low, high] : poolPhysical) total += high - low;
        return total;
    }
    std::uint64_t used(Kind kind) const {
        std::uint64_t total = 0;
        for (const auto& region : regions) if (region.kind == kind) total += region.last - region.first;
        return total;
    }
    // Every region overlapping [start, stop), clipped to it; holes are simply absent.
    std::vector<Region> parts(std::uint64_t start, std::uint64_t stop) const {
        std::vector<Region> result;
        for (const auto& region : regions) {
            if (!overlaps(start, stop, region.first, region.last)) continue;
            auto part = region;
            const auto low = std::max(start, region.first);
            part.offset += low - region.first;
            if (part.kind == Kind::Direct) part.physical += low - region.first;
            part.first = low; part.last = std::min(stop, region.last);
            result.push_back(std::move(part));
        }
        return result;
    }
    std::vector<Span> machineSpans(std::uint64_t start, std::uint64_t stop) const {
        std::vector<Span> result;
        for (const auto& mapping : machine.Mappings()) {
            const auto last = end(mapping.Address, mapping.Size);
            if (overlaps(start, stop, mapping.Address, last))
                result.emplace_back(std::max(start, mapping.Address), std::min(stop, last));
        }
        std::sort(result.begin(), result.end());
        return result;
    }
    // Machine mappings in [start, stop) the runtime does not own: module images, stacks, gate pages.
    std::vector<Span> foreign(std::uint64_t start, std::uint64_t stop) const {
        std::vector<Span> owned;
        for (const auto& region : parts(start, stop)) if (region.owner) owned.emplace_back(region.first, region.last);
        return subtract(machineSpans(start, stop), std::move(owned));
    }
    void unmapCommitted(const std::vector<Region>& values) {
        for (const auto& region : values) if (region.owner)
            machine.Unmap(region.first, static_cast<std::size_t>(region.last - region.first));
    }
    void eraseNames(std::uint64_t start, std::uint64_t stop) {
        auto it = names.lower_bound(start);
        if (it != names.begin() && std::prev(it)->second.last > start) --it;
        while (it != names.end() && it->first < stop) {
            const auto first = it->first;
            const auto value = it->second;
            it = names.erase(it);
            if (first < start) names.emplace(first, Name{start, value.text});
            if (value.last > stop) it = names.emplace(stop, Name{value.last, value.text}).first;
        }
    }
    // Names split query extents the way upstream's range names do.
    GuestMemoryQuery named(GuestMemoryQuery query, std::uint64_t address) const {
        const auto at = std::max(address, query.Start), original = query.Start;
        const auto finish = [&] {
            if (query.Flags & 2) query.Offset += query.Start - original;
            return query;
        };
        const auto next = names.upper_bound(at);
        if (next != names.begin()) {
            const auto containing = std::prev(next);
            if (at < containing->second.last) {
                query.Start = std::max(query.Start, containing->first);
                query.End = std::min(query.End, containing->second.last);
                const auto& text = containing->second.text;
                std::copy_n(text.begin(), std::min(text.size(), query.Name.size() - 1), query.Name.begin());
                return finish();
            }
            query.Start = std::max(query.Start, containing->second.last);
        }
        if (next != names.end()) query.End = std::min(query.End, next->first);
        return finish();
    }
    static GuestMemoryQuery describe(const Region& region) {
        return {region.first, region.last, region.kind == Kind::Direct ? region.physical : 0, region.protection, region.type,
            static_cast<std::uint32_t>((region.kind == Kind::Flexible ? 1 : 0) | (region.kind == Kind::Direct ? 2 : 0) |
                (region.kind == Kind::Pooled || region.kind == Kind::PoolReserved ? 8 : 0) | (region.owner ? 16 : 0))};
    }
    static void retype(std::vector<Physical>& blocks, std::uint64_t start, std::uint64_t stop, std::int32_t type) {
        std::vector<Physical> result;
        result.reserve(blocks.size() + 2);
        for (const auto& block : blocks) {
            if (!overlaps(start, stop, block.first, block.last)) { result.push_back(block); continue; }
            if (block.first < start) { auto left = block; left.last = start; result.push_back(std::move(left)); }
            auto middle = block;
            middle.first = std::max(start, block.first); middle.last = std::min(stop, block.last);
            middle.offset += middle.first - block.first; middle.type = type;
            result.push_back(std::move(middle));
            if (block.last > stop) { auto right = block; right.offset += stop - block.first; right.first = stop; result.push_back(std::move(right)); }
        }
        blocks.swap(result);
    }
    struct Placement { std::uint64_t address; std::vector<Region> replaced; };
    // MEM-10: a fixed placement may land in a reservation, and without NO_OVERWRITE it replaces
    // whatever the runtime has mapped there; the caller removes `replaced` in the same commit.
    // Memory the runtime does not own (module images, stacks, gate pages) is never replaced.
    Placement place(std::uint64_t hint, std::uint64_t size, std::uint32_t placement, std::uint64_t align) const {
        align = std::max(align, flags(placement));
        if (placement & fixed) {
            if (!hint || hint % align) error(22, "Invalid fixed guest memory address");
            const auto stop = end(hint, size);
            if (hint >= virtualLimit || size > virtualLimit - hint) error(12, "Guest virtual address space is exhausted");
            if (!foreign(hint, stop).empty()) error(17, "Guest memory address is occupied");
            auto replaced = parts(hint, stop);
            if (placement & noOverwrite)
                for (const auto& region : replaced) if (region.owner) error(17, "Guest memory address is occupied");
            return {hint, std::move(replaced)};
        }
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
            if (overlaps(cursor, end(cursor, size), range.first, range.second)) cursor = aligned(range.second, align);
        }
        if (!cursor || cursor >= virtualLimit || size > virtualLimit - cursor) error(12, "Guest virtual address space is exhausted");
        return {cursor, {}};
    }
    void output(std::uint64_t address, std::uint64_t start, std::uint64_t stop, std::uint32_t protection) const {
        if (!address) return;
        try { machine.CheckAccess(address, 8, Permission::Write); }
        catch (const std::exception&) { error(14, "Guest memory output is not writable"); }
        if (overlaps(address, end(address, 8), start, stop) && !(protection & 2)) error(14, "Guest memory output loses write permission");
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

GuestMemoryRuntime::GuestMemoryRuntime(Machine& machine, std::uint64_t capacity, Transaction transaction, std::uint64_t flexibleCapacity)
    : impl(std::make_unique<Impl>(machine, capacity, std::move(transaction), flexibleCapacity)) {}
GuestMemoryRuntime::~GuestMemoryRuntime() { try { Shutdown(); } catch (...) { if (!impl->closed) std::terminate(); } }
std::uint64_t GuestMemoryRuntime::DirectMemorySize() const { impl->live(); return impl->capacity; }
std::uint64_t GuestMemoryRuntime::FlexibleMemorySize() const { impl->live(); return impl->flexibleCapacity; }
std::uint64_t GuestMemoryRuntime::AvailableFlexible() const {
    impl->live();
    return impl->flexibleCapacity - std::min(impl->flexibleCapacity, impl->used(Impl::Kind::Flexible));
}
GuestPhysicalExtent GuestMemoryRuntime::AvailableDirect(std::int64_t start, std::int64_t stop, std::uint64_t align) const {
    impl->live(); return impl->available(start, stop, alignment(align));
}
std::optional<GuestDirectMemoryQuery> GuestMemoryRuntime::QueryDirect(std::int64_t offset, bool findNext) const {
    impl->live();
    if (offset < 0) error(22, "Invalid guest physical query offset");
    const auto at = static_cast<std::uint64_t>(offset);
    for (const auto& block : impl->physical) {
        if ((at >= block.first && at < block.last) || (findNext && block.first > at))
            return GuestDirectMemoryQuery{block.first, block.last, block.type};
    }
    return std::nullopt;
}
std::uint64_t GuestMemoryRuntime::AllocateDirect(std::int64_t start, std::int64_t stop, std::uint64_t bytes,
                                                std::uint64_t align, std::int32_t type) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); align = alignment(align);
    // MEM-09: every memory type is an attribute of the allocation; only negative values are invalid.
    if (type < 0) throw GuestMemoryError(22, "Unsupported guest memory type: " + std::to_string(type));
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
                                          std::uint64_t outputAddress, std::int32_t memoryType) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); const auto cpu = permissions(protection); align = alignment(align);
    if (protection & 4) error(22, "Guest direct memory cannot be mapped executable");
    if (physical < 0 || static_cast<std::uint64_t>(physical) % page) error(22, "Invalid guest physical mapping address");
    const auto first = static_cast<std::uint64_t>(physical), stop = end(first, bytes);
    if (stop > impl->capacity) error(22, "Guest physical mapping exceeds capacity");
    auto placed = impl->place(hint, bytes, placement, align);
    const auto address = placed.address;
    impl->output(outputAddress, address, address + bytes, protection);
    auto candidate = Impl::remove(impl->regions, address, address + bytes);
    std::vector<Impl::Region> added;
    auto cursor = first;
    for (const auto& block : impl->physical) {
        if (block.last <= cursor) continue;
        if (block.first > cursor) break;
        const auto last = std::min(stop, block.last);
        const auto va = address + cursor - first;
        added.push_back({va, va + last - cursor, address, address + bytes, block.offset + cursor - block.first,
            cursor, block.id, ++impl->nextIdentity, protection, memoryType >= 0 ? memoryType : block.type, Impl::Kind::Direct, block.owner});
        cursor = last;
        if (cursor == stop) break;
    }
    if (cursor != stop) error(22, "Guest direct mapping references unallocated physical memory");
    auto allocations = impl->physical;
    if (memoryType >= 0) Impl::retype(allocations, first, stop, memoryType);
    candidate.insert(candidate.end(), added.begin(), added.end()); Impl::sort(candidate);
    impl->commit(std::move(candidate), std::move(allocations), [state = impl.get(), replaced = std::move(placed.replaced), added, cpu] {
        state->unmapCommitted(replaced);
        for (const auto& region : added) state->machine.MapBorrowed(region.first,
            {region.owner->data + region.offset, static_cast<std::size_t>(region.last - region.first)}, cpu,
            {region.owner->data, region.owner->size});
    });
    impl->eraseNames(address, address + bytes);
    return address;
}
std::uint64_t GuestMemoryRuntime::MapFlexible(std::uint64_t hint, std::uint64_t bytes, std::uint32_t protection,
                                            std::uint32_t placement, std::uint64_t outputAddress) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes); const auto cpu = permissions(protection);
    auto placed = impl->place(hint, bytes, placement, page);
    const auto address = placed.address;
    // SEC-05: flexible memory comes out of a fixed budget; a fixed replacement returns what it replaces.
    std::uint64_t replacedFlexible = 0;
    for (const auto& region : placed.replaced) if (region.kind == Impl::Kind::Flexible) replacedFlexible += region.last - region.first;
    const auto inUse = impl->used(Impl::Kind::Flexible) - replacedFlexible;
    if (inUse > impl->flexibleCapacity || bytes > impl->flexibleCapacity - inUse) error(12, "Guest flexible memory budget is exhausted");
    impl->output(outputAddress, address, address + bytes, protection);
    auto owner = std::make_shared<Storage>(bytes);
    auto candidate = Impl::remove(impl->regions, address, address + bytes);
    candidate.push_back({address, address + bytes, address, address + bytes, 0, 0, ++impl->nextId, ++impl->nextIdentity,
                         protection, 0, Impl::Kind::Flexible, owner}); Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), replaced = std::move(placed.replaced), address, bytes, owner, cpu] {
        state->unmapCommitted(replaced);
        state->machine.MapBorrowed(address, {owner->data, static_cast<std::size_t>(bytes)}, cpu, {owner->data, owner->size});
    });
    impl->eraseNames(address, address + bytes);
    return address;
}
std::uint64_t GuestMemoryRuntime::Reserve(std::uint64_t hint, std::uint64_t bytes, std::uint32_t placement,
                                        std::uint64_t align, std::uint64_t outputAddress, bool pool) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes);
    if (pool && (bytes % PoolBlockSize || (align && align < PoolBlockSize)))
        error(22, "Guest memory pool ranges are whole 64 KiB blocks");
    auto placed = impl->place(hint, bytes, placement, std::max(alignment(align), pool ? PoolBlockSize : page));
    const auto address = placed.address;
    impl->output(outputAddress, address, address + bytes, 0);
    auto candidate = Impl::remove(impl->regions, address, address + bytes);
    candidate.push_back({address, address + bytes, address, address + bytes, 0, 0, 0, ++impl->nextIdentity, 0, 0,
                         pool ? Impl::Kind::PoolReserved : Impl::Kind::Reserved, {}});
    Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), replaced = std::move(placed.replaced)] {
        state->unmapCommitted(replaced);
    });
    impl->eraseNames(address, address + bytes);
    return address;
}
void GuestMemoryRuntime::Protect(std::uint64_t address, std::uint64_t bytes, std::uint32_t protection, std::int32_t memoryType) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    if (!address) error(22, "Invalid guest protection address");
    const auto first = address & ~(page - 1);
    const auto stop = aligned(end(address, bytes), page);
    const auto size = stop - first;
    if (size > std::numeric_limits<std::size_t>::max()) error(22, "Invalid guest protection size");
    const auto cpu = permissions(protection);
    auto affected = impl->parts(first, stop);
    // MEM-29: pinned Machine mappings (module images, stacks) are protected directly; only a
    // range with an unmapped hole is refused.
    auto known = impl->foreign(first, stop);
    for (const auto& region : affected) known.emplace_back(region.first, region.last);
    if (!subtract({{first, stop}}, std::move(known)).empty()) error(13, "Guest memory range is not owned by the runtime");
    auto candidate = Impl::remove(impl->regions, first, stop);
    auto allocations = impl->physical;
    for (auto& region : affected) {
        if (!region.owner) throw GuestMemoryError(45, "Unsupported guest memory protection of uncommitted reservation");
        region.protection = protection; region.identity = ++impl->nextIdentity;
        if (memoryType >= 0) {
            region.type = memoryType;
            if (region.kind == Impl::Kind::Direct)
                Impl::retype(allocations, region.physical, region.physical + region.last - region.first, memoryType);
        }
        candidate.push_back(region);
    }
    Impl::sort(candidate);
    impl->commit(std::move(candidate), std::move(allocations), [state = impl.get(), first, size, cpu] {
        state->machine.Protect(first, static_cast<std::size_t>(size), cpu);
    });
}
void GuestMemoryRuntime::Unmap(std::uint64_t address, std::uint64_t bytes) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    if (!address || address % page) error(22, "Invalid guest unmap address");
    if (!bytes) error(22, "Invalid guest memory size");
    // MEM-29: the length is rounded up to whole pages and holes in the range are not an error;
    // memory the runtime does not own is still refused.
    const auto stop = aligned(end(address, bytes), page);
    if (!impl->foreign(address, stop).empty()) error(13, "Guest memory range is not owned by the runtime");
    const auto affected = impl->parts(address, stop);
    if (!affected.empty()) {
        auto candidate = Impl::remove(impl->regions, address, stop);
        impl->commit(std::move(candidate), impl->physical, [state = impl.get(), affected] { state->unmapCommitted(affected); });
    }
    impl->eraseNames(address, stop);
}
void GuestMemoryRuntime::ReleaseDirect(std::int64_t physical, std::uint64_t bytes, bool checked) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    length(bytes);
    if (physical < 0 || static_cast<std::uint64_t>(physical) % page) error(22, "Invalid guest physical release address");
    const auto first = static_cast<std::uint64_t>(physical), stop = end(first, bytes);
    auto cursor = first; bool hole = false, any = false;
    std::vector<Impl::Physical> allocations;
    allocations.reserve(impl->physical.size() + 1);
    for (const auto& block : impl->physical) {
        if (!overlaps(first, stop, block.first, block.last)) { allocations.push_back(block); continue; }
        any = true;
        if (block.first > cursor) hole = true;
        cursor = std::min(stop, block.last);
        if (block.first < first) { auto left = block; left.last = first; allocations.push_back(std::move(left)); }
        if (block.last > stop) { auto right = block; right.offset += stop - block.first; right.first = stop; allocations.push_back(std::move(right)); }
    }
    if (cursor != stop) hole = true;
    if (hole && checked) error(2, "Guest physical release is not allocated");
    if (!any) return;
    // MEM-11: release implicitly unmaps every virtual alias of the released pages.
    std::vector<Span> aliases;
    for (const auto& region : impl->regions) {
        if (region.kind != Impl::Kind::Direct) continue;
        const auto physicalEnd = region.physical + (region.last - region.first);
        const auto low = std::max(first, region.physical), high = std::min(stop, physicalEnd);
        if (low < high) aliases.emplace_back(region.first + (low - region.physical), region.first + (high - region.physical));
    }
    auto candidate = impl->regions;
    for (const auto& [start, finish] : aliases) candidate = Impl::remove(candidate, start, finish);
    impl->commit(std::move(candidate), std::move(allocations), [state = impl.get(), aliases] {
        for (const auto& [start, finish] : aliases) state->machine.Unmap(start, static_cast<std::size_t>(finish - start));
    });
    for (const auto& [start, finish] : aliases) impl->eraseNames(start, finish);
    // Released pool pages no longer back pool commits.
    impl->poolPhysical = subtract(impl->poolPhysical, {{first, stop}});
}
void GuestMemoryRuntime::SetName(std::uint64_t address, std::uint64_t bytes, std::string_view name) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    if (!address) error(22, "Invalid guest range name address");
    // Names cover whole pages, so query extents stay page-aligned.
    const auto first = address & ~(page - 1);
    const auto stop = aligned(end(address, bytes), page);
    impl->eraseNames(first, stop);
    impl->names.emplace(first, Impl::Name{stop, std::string(name.substr(0, 31))});
}
void GuestMemoryRuntime::CheckMapped(std::uint64_t address, std::uint64_t bytes) const {
    impl->live();
    if (!bytes) return;
    const auto first = address & ~(page - 1);
    const auto stop = aligned(end(address, bytes), page);
    if (!subtract({{first, stop}}, impl->machineSpans(first, stop)).empty()) error(12, "Guest memory range is not mapped");
}
// Pool lengths, addresses and alignments are whole 64 KiB blocks, so the block statistics
// describe exactly what commit and decommit allow.
static void poolBlocks(std::uint64_t address, std::uint64_t bytes) {
    length(bytes);
    if (!address || address % GuestMemoryRuntime::PoolBlockSize || bytes % GuestMemoryRuntime::PoolBlockSize)
        error(22, "Guest memory pool ranges are whole 64 KiB blocks");
}
std::uint64_t GuestMemoryRuntime::PoolExpand(std::int64_t start, std::int64_t stop, std::uint64_t bytes, std::uint64_t align) {
    if (bytes % PoolBlockSize || (align && align % PoolBlockSize)) error(22, "Guest memory pool ranges are whole 64 KiB blocks");
    const auto address = AllocateDirect(start, stop, bytes, align ? align : PoolBlockSize, 0);
    impl->poolPhysical.emplace_back(address, address + bytes);
    std::sort(impl->poolPhysical.begin(), impl->poolPhysical.end());
    return address;
}
void GuestMemoryRuntime::PoolCommit(std::uint64_t address, std::uint64_t bytes, std::int32_t memoryType, std::uint32_t protection) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    poolBlocks(address, bytes);
    if (memoryType < 0) error(22, "Invalid guest pool memory type");
    const auto cpu = permissions(protection);
    const auto stop = end(address, bytes);
    std::vector<Span> reserved;
    for (const auto& region : impl->parts(address, stop)) {
        if (region.kind != Impl::Kind::PoolReserved) error(22, "Guest pool commit outside an uncommitted pool reservation");
        reserved.emplace_back(region.first, region.last);
    }
    if (!subtract({{address, stop}}, std::move(reserved)).empty()) error(22, "Guest pool commit outside a pool reservation");
    const auto committed = impl->used(Impl::Kind::Pooled), capacity = impl->poolCapacity();
    if (committed > capacity || bytes > capacity - committed) error(12, "Guest memory pool is exhausted");
    auto owner = std::make_shared<Storage>(bytes);
    auto candidate = Impl::remove(impl->regions, address, stop);
    candidate.push_back({address, stop, address, stop, 0, 0, ++impl->nextId, ++impl->nextIdentity, protection, memoryType,
                         Impl::Kind::Pooled, owner});
    Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), address, bytes, owner, cpu] {
        state->machine.MapBorrowed(address, {owner->data, static_cast<std::size_t>(bytes)}, cpu, {owner->data, owner->size});
    });
    impl->eraseNames(address, stop);
}
void GuestMemoryRuntime::PoolDecommit(std::uint64_t address, std::uint64_t bytes) {
    impl->live(); Impl::MutationScope mutation(impl->transactionInProgress);
    poolBlocks(address, bytes);
    const auto stop = end(address, bytes);
    auto affected = impl->parts(address, stop);
    std::vector<Span> pooled;
    for (const auto& region : affected) {
        if (region.kind != Impl::Kind::Pooled && region.kind != Impl::Kind::PoolReserved)
            error(22, "Guest pool decommit outside a pool reservation");
        pooled.emplace_back(region.first, region.last);
    }
    if (!subtract({{address, stop}}, std::move(pooled)).empty()) error(22, "Guest pool decommit outside a pool reservation");
    auto candidate = Impl::remove(impl->regions, address, stop);
    for (const auto& region : affected)
        candidate.push_back({region.first, region.last, region.allocationFirst, region.allocationLast, 0, 0, 0, ++impl->nextIdentity,
                             0, 0, Impl::Kind::PoolReserved, {}});
    Impl::sort(candidate);
    impl->commit(std::move(candidate), impl->physical, [state = impl.get(), affected] { state->unmapCommitted(affected); });
    impl->eraseNames(address, stop);
}
GuestMemoryPoolStats GuestMemoryRuntime::PoolStats() const {
    impl->live();
    const auto clamp = [](std::uint64_t value) {
        return static_cast<std::int32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::int32_t>::max()));
    };
    const auto total = impl->poolCapacity() / PoolBlockSize;
    const auto used = (impl->used(Impl::Kind::Pooled) + PoolBlockSize - 1) / PoolBlockSize;
    return {clamp(total > used ? total - used : 0), 0, clamp(used), 0};
}
GuestMemoryQuery GuestMemoryRuntime::Query(std::uint64_t address, bool findNext, bool splitNames) const {
    impl->live();
    const auto answer = [&](const GuestMemoryQuery& query) { return splitNames ? impl->named(query, address) : query; };
    std::optional<GuestMemoryQuery> best;
    for (const auto& region : impl->regions) {
        if (address >= region.first && address < region.last) return answer(Impl::describe(region));
        if (findNext && region.first > address && (!best || region.first < best->Start)) best = Impl::describe(region);
    }
    const auto mappings = impl->machine.Mappings();
    for (const auto& mapping : mappings) {
        const auto last = end(mapping.Address, mapping.Size);
        if (address >= mapping.Address && address < last)
            return answer({mapping.Address, last, 0, static_cast<unsigned>(mapping.Permissions), 0, 16});
        if (findNext && mapping.Address > address && (!best || mapping.Address < best->Start))
            best = GuestMemoryQuery{mapping.Address, last, 0, static_cast<unsigned>(mapping.Permissions), 0, 16};
    }
    if (best) return answer(*best);
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
    impl->names.clear();
    impl->generation = next.Generation; impl->closed = true;
    if (failure) std::rethrow_exception(failure);
}

}
