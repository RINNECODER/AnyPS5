#include <cpu/Cpu.hpp>
#include <qemu/anyps5-cpu.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

namespace Cpu {
namespace {
AnyPS5QemuRegister registerId(Register reg) {
    switch (reg) {
    case Register::Rax: return ANYPS5_QEMU_RAX;
    case Register::Rbx: return ANYPS5_QEMU_RBX;
    case Register::Rcx: return ANYPS5_QEMU_RCX;
    case Register::Rdx: return ANYPS5_QEMU_RDX;
    case Register::Rsi: return ANYPS5_QEMU_RSI;
    case Register::Rdi: return ANYPS5_QEMU_RDI;
    case Register::Rbp: return ANYPS5_QEMU_RBP;
    case Register::Rsp: return ANYPS5_QEMU_RSP;
    case Register::R8: return ANYPS5_QEMU_R8;
    case Register::R9: return ANYPS5_QEMU_R9;
    case Register::R10: return ANYPS5_QEMU_R10;
    case Register::R11: return ANYPS5_QEMU_R11;
    case Register::R12: return ANYPS5_QEMU_R12;
    case Register::R13: return ANYPS5_QEMU_R13;
    case Register::R14: return ANYPS5_QEMU_R14;
    case Register::R15: return ANYPS5_QEMU_R15;
    case Register::Rip: return ANYPS5_QEMU_RIP;
    case Register::Rflags: return ANYPS5_QEMU_RFLAGS;
    case Register::FsBase: return ANYPS5_QEMU_FS_BASE;
    case Register::GsBase: return ANYPS5_QEMU_GS_BASE;
    }
    throw std::invalid_argument("Unknown guest register");
}
void checkRange(std::uint64_t address, std::size_t size) {
    if (!size || (address & 4095) || (size & 4095) || size > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Guest mapping must be a nonempty 4 KiB aligned range without overflow");
}
unsigned permissionBits(Permission permissions) {
    const auto bits = static_cast<unsigned>(permissions);
    if (bits & ~7u) throw std::invalid_argument("Invalid guest mapping permissions");
    return bits;
}
std::size_t hostPageSize() {
    const auto size = sysconf(_SC_PAGESIZE);
    if (size <= 0) throw std::runtime_error("Cannot determine the modern guest CPU host page size");
    return static_cast<std::size_t>(size);
}
struct Free { void operator()(std::byte* value) const { std::free(value); } };
struct MappingScope {};
struct ContextLifetime {
    const std::thread::id owner = std::this_thread::get_id();
    std::atomic<AnyPS5QemuCpu*> engine{nullptr};
    bool running = false;
};
struct SuspendedFrame {
    std::uint64_t gate;
    std::uint64_t stack;
    std::uint64_t destination;
    std::uint64_t epoch;
    bool completed = false;
    bool abandoned = false;
};
}

struct Machine::Context::Payload {
    std::weak_ptr<ContextLifetime> lifetime;
    AnyPS5QemuContext* value = nullptr;
    std::uint64_t epoch = 0;
    ~Payload() {
        const auto live = lifetime.lock();
        if (!live || !value) return;
        const auto engine = live->engine.load();
        if (!engine) return;
        if (std::this_thread::get_id() != live->owner || live->running ||
            anyps5_qemu_cpu_context_destroy(engine, value)) std::terminate();
    }
};

Machine::Context::Context() = default;
Machine::Context::~Context() = default;
Machine::Context::Context(Context&&) noexcept = default;
Machine::Context& Machine::Context::operator=(Context&&) noexcept = default;
Machine::Context::Context(std::unique_ptr<Payload> value) : payload(std::move(value)) {}

struct Machine::SuspendedCall::Payload {
    std::weak_ptr<ContextLifetime> lifetime;
    std::shared_ptr<SuspendedFrame> frame;
    ~Payload() {
        const auto live = lifetime.lock();
        if (!live || !live->engine.load()) return;
        if (std::this_thread::get_id() != live->owner || live->running) std::terminate();
        if (frame && !frame->completed) frame->abandoned = true;
    }
};

Machine::SuspendedCall::SuspendedCall() = default;
Machine::SuspendedCall::~SuspendedCall() = default;
Machine::SuspendedCall::SuspendedCall(SuspendedCall&&) noexcept = default;
Machine::SuspendedCall& Machine::SuspendedCall::operator=(SuspendedCall&&) noexcept = default;
Machine::SuspendedCall::SuspendedCall(std::unique_ptr<Payload> value) : payload(std::move(value)) {}

struct Machine::Impl {
    struct ActiveCall {
        std::uint64_t gate;
        std::uint64_t stack;
        std::shared_ptr<SuspendedFrame> paused;
    };
    struct Range {
        std::uint64_t address;
        std::size_t size;
        std::byte* backing;
        unsigned permissions;
        bool borrowed;
    };
    struct Backing {
        std::byte* pointer;
        std::size_t size;
        std::uint64_t id;
        std::uint64_t identity;
        std::shared_ptr<std::byte> storage;
    };
    AnyPS5QemuCpu* engine = nullptr;
    const std::thread::id owner = std::this_thread::get_id();
    std::shared_ptr<ContextLifetime> contextLifetime = std::make_shared<ContextLifetime>();
    std::vector<Range> ranges;
    std::vector<Backing> backings;
    std::shared_ptr<const void> mappingScope = std::make_shared<const MappingScope>();
    std::uint64_t mappingGeneration = 0;
    std::uint64_t lastBackingIdentity = 0;
    struct OwnedTransactionFrame {
        const std::thread::id owner = std::this_thread::get_id();
        const OwnedMappingSnapshot previous;
        const OwnedMappingSnapshot candidate;
        std::atomic<bool> active{true};
        std::atomic<bool> violation{false};
        bool started = false, finished = false;
        OwnedTransactionFrame(OwnedMappingSnapshot before, OwnedMappingSnapshot after)
            : previous(std::move(before)), candidate(std::move(after)) {}
    };
    OwnedMappingTransaction ownedTransaction;
    std::shared_ptr<OwnedTransactionFrame> activeOwnedTransaction;
    std::shared_ptr<OwnedTransactionFrame> failedOwnedTransaction;
    std::unordered_map<std::uint64_t, std::function<void(Machine&)>> calls;
    std::vector<std::shared_ptr<SuspendedFrame>> pendingCalls;
    std::vector<std::shared_ptr<SuspendedFrame>> retiredCalls;
    std::uint64_t epoch = 0;
    std::uint64_t lastEpoch = 0;
    ActiveCall* activeCall = nullptr;
    std::function<void(Machine&)> syscall;
    std::atomic<bool> requested{false};
    bool exited = false;
    int exitCode = 0;
    std::uint64_t lastRunInstructions = 0;

    Impl() {
        std::array<char, 512> error{};
        engine = anyps5_qemu_cpu_create(error.data(), error.size());
        if (!engine) throw std::runtime_error(std::string("Create modern x86-64 translator: ") + error.data());
        contextLifetime->engine = engine;
    }
    ~Impl() {
        contextLifetime->engine.store(nullptr);
        contextLifetime.reset();
        if (anyps5_qemu_cpu_destroy(engine)) std::terminate();
    }
    void check(int result, const char* operation) const {
        if (result) throw std::runtime_error(std::string(operation) + ": " + anyps5_qemu_cpu_error(engine));
    }
    void checkOwner() const {
        if (std::this_thread::get_id() != owner) throw std::logic_error("Modern guest CPU requires its owner thread");
    }
    void checkContextIdle() const {
        checkOwner();
        if (contextLifetime->running) throw std::logic_error("Guest execution context requires an idle Machine");
    }
    void checkMappingMutation(bool teardown = false) const {
        checkOwner();
        if (activeOwnedTransaction) {
            activeOwnedTransaction->violation.store(true);
            throw std::logic_error("Owned guest mapping transaction forbids recursive mapping mutation");
        }
        if (failedOwnedTransaction && (!teardown || ownedTransaction))
            throw std::runtime_error("Owned mapping transaction failed; graphics-drained teardown required");
        if (mappingGeneration == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Guest mapping generation exhausted");
    }
    static bool sameOwner(const std::shared_ptr<const void>& left, const std::shared_ptr<const void>& right) {
        return left.get() == right.get() && !left.owner_before(right) && !right.owner_before(left);
    }
    static bool sameView(const OwnedMappingView& left, const OwnedMappingView& right) {
        return left.Region.Address == right.Region.Address && left.Region.Size == right.Region.Size &&
            left.Region.Permissions == right.Region.Permissions && left.Region.Borrowed == right.Region.Borrowed &&
            left.Bytes.data() == right.Bytes.data() && left.Bytes.size() == right.Bytes.size() &&
            left.Allocation.data() == right.Allocation.data() && left.Allocation.size() == right.Allocation.size() &&
            left.BackingIdentity == right.BackingIdentity && sameOwner(left.Owner, right.Owner);
    }
    static bool sameViews(const OwnedMappingSnapshot& left, const OwnedMappingSnapshot& right) {
        return sameOwner(left.Scope, right.Scope) && left.Views.size() == right.Views.size() &&
            std::equal(left.Views.begin(), left.Views.end(), right.Views.begin(), sameView);
    }
    OwnedMappingSnapshot ownedSnapshot(const std::vector<Range>& source, const std::vector<Backing>& allocations,
                                       std::uint64_t generation) const {
        OwnedMappingSnapshot result{mappingScope, generation, {}};
        result.Views.reserve(source.size());
        for (const auto& range : source) {
            const auto pointer = reinterpret_cast<std::uintptr_t>(range.backing);
            const auto backing = std::find_if(allocations.begin(), allocations.end(), [&](const auto& value) {
                const auto begin = reinterpret_cast<std::uintptr_t>(value.pointer);
                return value.storage && pointer >= begin && pointer - begin <= value.size &&
                    range.size <= value.size - (pointer - begin);
            });
            if (backing != allocations.end())
                result.Views.push_back({{range.address, range.size, static_cast<Permission>(range.permissions), range.borrowed},
                    {range.backing, range.size}, {backing->pointer, backing->size}, backing->identity, backing->storage});
        }
        std::sort(result.Views.begin(), result.Views.end(), [](const auto& left, const auto& right) {
            return left.Region.Address < right.Region.Address;
        });
        return result;
    }
    void transactOwned(const std::vector<Range>& candidateRanges, const std::vector<Backing>& candidateBackings,
                       std::function<void()> action) {
        if (!ownedTransaction) { action(); return; }
        auto previous = ownedSnapshot(ranges, backings, mappingGeneration);
        auto candidate = ownedSnapshot(candidateRanges, candidateBackings, mappingGeneration + 1);
        // Runtime-managed external borrowed storage has its own transaction.
        // A CPU generation change alone must not nest native publication.
        if (sameViews(previous, candidate)) { action(); return; }
        // A mixed candidate must preserve external bindings in BOTH directions:
        // the owned compositor can neither retire nor introduce Runtime storage.
        const auto backingFor = [](const Range& range, const std::vector<Backing>& allocations) {
            const auto pointer = reinterpret_cast<std::uintptr_t>(range.backing);
            return std::find_if(allocations.begin(), allocations.end(), [&](const auto& value) {
                const auto begin = reinterpret_cast<std::uintptr_t>(value.pointer);
                return pointer >= begin && pointer - begin <= value.size && range.size <= value.size - (pointer - begin);
            });
        };
        const auto unchangedExternal = [&](const std::vector<Range>& source, const std::vector<Backing>& sourceBackings,
                                           const std::vector<Range>& destination, const std::vector<Backing>& destinationBackings) {
            for (const auto& range : source) {
                const auto allocation = backingFor(range, sourceBackings);
                if (allocation == sourceBackings.end())
                    throw std::logic_error("Owned mapping transaction has an unregistered backing");
                if (allocation->storage) continue; // Includes borrowed aliases into genuinely owned storage.
                auto cursor = range.address;
                const auto end = range.address + range.size;
                while (cursor < end) {
                    const auto mapped = std::find_if(destination.begin(), destination.end(), [&](const auto& value) {
                        return cursor >= value.address && cursor - value.address < value.size;
                    });
                    if (mapped == destination.end())
                        throw std::runtime_error("Owned mapping transaction cannot change mixed external borrowed bindings");
                    const auto mappedAllocation = backingFor(*mapped, destinationBackings);
                    if (mappedAllocation == destinationBackings.end() || mappedAllocation->storage ||
                        mappedAllocation->identity != allocation->identity || mappedAllocation->pointer != allocation->pointer ||
                        mappedAllocation->size != allocation->size || mapped->permissions != range.permissions ||
                        mapped->borrowed != range.borrowed ||
                        reinterpret_cast<std::uintptr_t>(mapped->backing) + (cursor - mapped->address) !=
                            reinterpret_cast<std::uintptr_t>(range.backing) + (cursor - range.address))
                        throw std::runtime_error("Owned mapping transaction cannot change mixed external borrowed bindings");
                    cursor = std::min(end, mapped->address + mapped->size);
                }
            }
        };
        unchangedExternal(ranges, backings, candidateRanges, candidateBackings);
        unchangedExternal(candidateRanges, candidateBackings, ranges, backings);
        auto frame = std::make_shared<OwnedTransactionFrame>(std::move(previous), std::move(candidate));
        std::function<void()> commit = [frame, action = std::move(action)] {
            if (std::this_thread::get_id() != frame->owner) {
                frame->violation.store(true);
                throw std::logic_error("Owned guest mapping CPU commit requires its owner thread");
            }
            if (!frame->active.load() || frame->started || frame->violation.load()) {
                frame->violation.store(true);
                throw std::logic_error("Owned guest mapping CPU commit must execute exactly once synchronously");
            }
            frame->started = true;
            action();
            frame->finished = true;
        };
        activeOwnedTransaction = frame;
        try {
            ownedTransaction(frame->previous, frame->candidate, commit);
            if (!frame->finished || frame->violation.load())
                throw std::logic_error("Owned guest mapping transaction did not commit exactly once synchronously");
        } catch (...) {
            frame->active.store(false);
            activeOwnedTransaction.reset();
            if (frame->started) {
                failedOwnedTransaction = frame;
                requested.store(true);
                anyps5_qemu_cpu_stop(engine);
            }
            throw;
        }
        frame->active.store(false);
        activeOwnedTransaction.reset();
    }
    void checkExecutionMutation() const {
        checkOwner();
        if (activeOwnedTransaction) {
            activeOwnedTransaction->violation.store(true);
            throw std::logic_error("Owned guest mapping transaction forbids recursive guest execution");
        }
        if (failedOwnedTransaction)
            throw std::runtime_error("Owned mapping transaction failed; guest execution is poisoned");
    }
    void checkContext(const Context& context) const {
        checkContextIdle();
        if (!context.payload) throw std::invalid_argument("Guest execution context is empty");
        const auto lifetime = context.payload->lifetime.lock();
        if (!lifetime || !lifetime->engine.load()) throw std::invalid_argument("Guest execution context has expired");
        if (lifetime != contextLifetime) throw std::invalid_argument("Guest execution context belongs to another Machine");
    }
    void checkSuspendedCall(const SuspendedCall& call) const {
        checkOwner();
        if (contextLifetime->running) throw std::logic_error("Suspended guest host call requires an idle Machine");
        if (!call.payload) throw std::invalid_argument("Suspended guest host call is empty");
        const auto lifetime = call.payload->lifetime.lock();
        if (!lifetime || !lifetime->engine.load()) throw std::invalid_argument("Suspended guest host call has expired");
        if (lifetime != contextLifetime) throw std::invalid_argument("Suspended guest host call belongs to another Machine");
        if (call.payload->frame->completed) throw std::logic_error("Suspended guest host call is already completed");
        if (call.payload->frame->abandoned) throw std::logic_error("Suspended guest host call was abandoned");
    }
    void retireAbandonedCalls() {
        checkContextIdle();
        const auto count = static_cast<std::size_t>(std::count_if(pendingCalls.begin(), pendingCalls.end(),
            [](const auto& frame) { return frame->abandoned; }));
        if (!count) return;
        // Retain frame identities for the Machine lifetime: a saved context can
        // still restore an abandoned caller after an unrelated session starts.
        retiredCalls.reserve(retiredCalls.size() + count);
        for (const auto& frame : pendingCalls)
            if (frame->abandoned) retiredCalls.push_back(frame);
        std::erase_if(pendingCalls, [](const auto& frame) { return frame->abandoned; });
    }
    void checkRetiredFrame(std::uint64_t gate, std::uint64_t stack) const {
        if (std::any_of(retiredCalls.begin(), retiredCalls.end(), [&](const auto& frame) {
            return frame->epoch == epoch && frame->gate == gate && frame->stack == stack;
        })) throw std::logic_error("Suspended guest host call requires completion before redispatch");
    }
    void checkPendingFrame(std::uint64_t gate, std::uint64_t stack) const {
        checkRetiredFrame(gate, stack);
        if (std::any_of(pendingCalls.begin(), pendingCalls.end(), [&](const auto& frame) {
            return frame->epoch == epoch && frame->gate == gate && frame->stack == stack;
        })) throw std::logic_error("Suspended guest host call requires completion before redispatch");
    }
    void checkMapped(std::uint64_t address, std::size_t size, unsigned required = 0) const {
        checkOwner();
        if (!size) return;
        if (size > std::numeric_limits<std::uint64_t>::max() - address)
            throw std::invalid_argument("Guest access range overflows");
        auto cursor = address;
        const auto end = address + size;
        while (cursor < end) {
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& range) {
                return cursor >= range.address && cursor - range.address < range.size &&
                    (range.permissions & required) == required;
            });
            if (found == ranges.end()) {
                std::ostringstream message;
                message << "Guest access denied at 0x" << std::hex << cursor << " for permissions " << required;
                throw std::runtime_error(message.str());
            }
            cursor = std::min(end, found->address + found->size);
        }
    }
    void transfer(std::uint64_t address, std::byte* bytes, std::size_t size, bool write) const {
        while (size) {
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& range) {
                return address >= range.address && address - range.address < range.size;
            });
            if (found == ranges.end()) throw std::logic_error("Guest memory preflight did not cover the transfer");
            const auto offset = address - found->address;
            const auto chunk = std::min<std::size_t>(size, found->size - offset);
            if (write) std::memcpy(found->backing + offset, bytes, chunk);
            else std::memcpy(bytes, found->backing + offset, chunk);
            address += chunk;
            bytes += chunk;
            size -= chunk;
        }
    }
    void map(std::uint64_t address, std::span<std::byte> memory, unsigned permissions,
             bool borrowed, std::size_t allocationSize, std::shared_ptr<std::byte> storage = {},
             std::span<std::byte> fullBacking = {}) {
        checkOwner();
        const auto hostPage = hostPageSize();
        for (const auto& range : ranges)
            if (address < range.address + range.size && range.address < address + memory.size())
                throw std::invalid_argument("Map guest memory: guest mappings overlap");
        const auto pointer = reinterpret_cast<std::uintptr_t>(memory.data());
        const auto allocation = fullBacking.empty() ? memory.data() : fullBacking.data();
        const auto allocationPointer = reinterpret_cast<std::uintptr_t>(allocation);
        if (allocationSize > std::numeric_limits<std::uintptr_t>::max() - allocationPointer)
            throw std::invalid_argument("Guest backing memory range overflows");
        std::uint64_t backingId = 0;
        std::size_t offset = pointer - allocationPointer;
        const auto searchedPointer = fullBacking.empty() ? pointer : allocationPointer;
        const auto searchedSize = fullBacking.empty() ? memory.size() : allocationSize;
        for (const auto& backing : backings) {
            const auto begin = reinterpret_cast<std::uintptr_t>(backing.pointer);
            if (searchedPointer >= begin && searchedPointer - begin <= backing.size && searchedSize <= backing.size - (searchedPointer - begin)) {
                backingId = backing.id;
                offset = pointer - begin;
                break;
            }
            if (searchedPointer < begin + backing.size && begin < searchedPointer + searchedSize)
                throw std::runtime_error("Guest borrowed mapping crosses a registered backing allocation");
        }
        const auto newBacking = backingId == 0;
        if (newBacking && (allocationSize % hostPage || allocationPointer % hostPage))
            throw std::runtime_error("Modern guest CPU borrowed mapping requires a complete aligned host-page backing or a slice of a registered backing");
        if (offset & 4095) throw std::runtime_error("Modern guest CPU backing aliases require a 4 KiB aligned offset");
        checkMappingMutation();
        if (newBacking && lastBackingIdentity == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Guest backing identity exhausted");
        ranges.reserve(ranges.size() + 1);
        auto candidateRanges = ranges;
        candidateRanges.push_back({address, memory.size(), memory.data(), permissions, borrowed});
        auto candidateBackings = backings;
        if (newBacking) candidateBackings.push_back({allocation, allocationSize, 0, lastBackingIdentity + 1, std::move(storage)});
        transactOwned(candidateRanges, candidateBackings, [&] {
            if (newBacking)
                check(anyps5_qemu_cpu_register_backing(engine, allocation, allocationSize, &backingId), "Register modern guest backing");
            try {
                check(anyps5_qemu_cpu_map_alias(engine, address, backingId, offset, memory.size(), permissions), "Map modern guest backing alias");
            } catch (...) {
                if (newBacking && anyps5_qemu_cpu_release_backing(engine, backingId)) std::terminate();
                throw;
            }
            ++mappingGeneration;
            if (newBacking) { candidateBackings.back().id = backingId; ++lastBackingIdentity; }
            ranges.swap(candidateRanges);
            backings.swap(candidateBackings);
        });
    }
    void refresh() const {
        check(anyps5_qemu_cpu_invalidate(engine), "Invalidate modern translated guest code");
    }
    std::vector<Range> withoutRange(std::uint64_t address, std::size_t size) const {
        std::vector<Range> result;
        result.reserve(ranges.size() + 2);
        const auto end = address + size;
        for (const auto& range : ranges) {
            const auto rangeEnd = range.address + range.size;
            if (range.address >= end || rangeEnd <= address) result.push_back(range);
            else {
                if (range.address < address)
                    result.push_back({range.address, static_cast<std::size_t>(address - range.address), range.backing, range.permissions, range.borrowed});
                if (rangeEnd > end)
                    result.push_back({end, static_cast<std::size_t>(rangeEnd - end), range.backing + (end - range.address), range.permissions, range.borrowed});
            }
        }
        return result;
    }
    void removeCalls(std::uint64_t address, std::size_t size) {
        const auto end = address + size;
        for (auto call = calls.begin(); call != calls.end(); ) {
            if (call->first >= address && call->first < end) call = calls.erase(call);
            else ++call;
        }
    }
    void releaseUnused() {
        for (auto backing = backings.begin(); backing != backings.end(); ) {
            const auto begin = reinterpret_cast<std::uintptr_t>(backing->pointer);
            const auto used = std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) {
                const auto pointer = reinterpret_cast<std::uintptr_t>(range.backing);
                return pointer >= begin && pointer - begin < backing->size;
            });
            if (used) ++backing;
            else {
                check(anyps5_qemu_cpu_release_backing(engine, backing->id), "Release unused modern guest backing");
                backing = backings.erase(backing);
            }
        }
    }
    void rejectPrivileged(std::uint64_t address, std::uint64_t port) const {
        std::array<std::uint8_t, 15> bytes{};
        std::size_t size = 0;
        for (; size < bytes.size() && size <= std::numeric_limits<std::uint64_t>::max() - address; ++size) {
            const auto cursor = address + size;
            const auto found = std::find_if(ranges.begin(), ranges.end(), [&](const Range& range) {
                return cursor >= range.address && cursor - range.address < range.size && (range.permissions & 4);
            });
            if (found == ranges.end()) break;
            bytes[size] = std::to_integer<std::uint8_t>(found->backing[cursor - found->address]);
        }
        std::size_t offset = 0;
        for (; offset < size; ++offset) {
            const auto byte = bytes[offset];
            if ((byte >= 0x40 && byte <= 0x4f) || byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
                byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 || byte == 0x65) continue;
            break;
        }
        if (offset == size) return;
        const auto opcode = bytes[offset];
        const auto input = opcode == 0xe4 || opcode == 0xe5 || opcode == 0xec || opcode == 0xed || opcode == 0x6c || opcode == 0x6d;
        const auto output = opcode == 0xe6 || opcode == 0xe7 || opcode == 0xee || opcode == 0xef || opcode == 0x6e || opcode == 0x6f;
        if (input || output) {
            if (opcode >= 0xe4 && opcode <= 0xe7) {
                if (offset + 1 >= size) return;
                port = bytes[offset + 1];
            } else port &= 0xffff;
            throw std::runtime_error(std::string(input ? "Unsupported guest port input " : "Unsupported guest port output ") + std::to_string(port));
        }
        bool unsupported = opcode == 0xf4 || opcode == 0xfa || opcode == 0xfb;
        if (opcode == 0x0f && offset + 1 < size) {
            if (bytes[offset + 1] == 0x34) throw std::runtime_error("Unsupported guest SYSENTER service");
            switch (bytes[offset + 1]) {
            case 0x06: case 0x07: case 0x08: case 0x09:
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x30: case 0x32: case 0x35:
                unsupported = true;
                break;
            case 0x00:
                if (offset + 2 < size) {
                    const auto group = (bytes[offset + 2] >> 3) & 7;
                    unsupported = group == 2 || group == 3;
                }
                break;
            case 0x01:
                if (offset + 2 < size) {
                    const auto modrm = bytes[offset + 2];
                    const auto group = (modrm >> 3) & 7;
                    const auto memory = (modrm & 0xc0) != 0xc0;
                    unsupported = (memory && (group == 2 || group == 3 || group == 7)) || group == 6 ||
                        modrm == 0xf8 || modrm == 0xc8 || modrm == 0xc9;
                }
                break;
            default: break;
            }
        }
        if (unsupported) {
            std::ostringstream message;
            message << "Unsupported guest privileged service instruction at 0x" << std::hex << address;
            throw std::runtime_error(message.str());
        }
    }
};

Machine::Machine() : impl(std::make_unique<Impl>()) {}
Machine::~Machine() = default;
void Machine::Map(std::uint64_t address, std::size_t size, Permission permissions) {
    checkRange(address, size);
    const auto bits = permissionBits(permissions);
    const auto hostPage = hostPageSize();
    if (size > std::numeric_limits<std::size_t>::max() - (hostPage - 1)) throw std::invalid_argument("Guest backing allocation size overflows");
    const auto allocationSize = ((size + hostPage - 1) / hostPage) * hostPage;
    void* pointer = nullptr;
    if (posix_memalign(&pointer, hostPage, allocationSize)) throw std::bad_alloc();
    std::shared_ptr<std::byte> storage(static_cast<std::byte*>(pointer), Free{});
    std::memset(pointer, 0, allocationSize);
    const auto memory = std::span(storage.get(), size);
    impl->map(address, memory, bits, false, allocationSize, std::move(storage));
}
void Machine::MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions) {
    checkRange(address, memory.size());
    if (!memory.data()) throw std::invalid_argument("Guest borrowed mapping requires backing memory");
    impl->map(address, memory, permissionBits(permissions), true, memory.size());
}
void Machine::MapBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions,
                          std::span<std::byte> fullBacking) {
    checkRange(address, memory.size());
    const auto bits = permissionBits(permissions);
    const auto pointer = reinterpret_cast<std::uintptr_t>(memory.data());
    const auto begin = reinterpret_cast<std::uintptr_t>(fullBacking.data());
    const auto hostPage = hostPageSize();
    if (!memory.data() || !fullBacking.data() || fullBacking.empty() ||
        memory.size() > std::numeric_limits<std::uintptr_t>::max() - pointer ||
        fullBacking.size() > std::numeric_limits<std::uintptr_t>::max() - begin ||
        (pointer & 4095) || begin % hostPage || fullBacking.size() % hostPage ||
        pointer < begin || pointer - begin > fullBacking.size() ||
        memory.size() > fullBacking.size() - (pointer - begin))
        throw std::invalid_argument("Guest borrowed mapping requires an aligned complete backing containing the mapped span without overflow");
    impl->map(address, memory, bits, true, fullBacking.size(), {}, fullBacking);
}
void Machine::Unmap(std::uint64_t address, std::size_t size) {
    checkRange(address, size);
    impl->checkMapped(address, size);
    auto replacement = impl->withoutRange(address, size);
    impl->checkMappingMutation(true);
    impl->transactOwned(replacement, impl->backings, [&] {
        impl->check(anyps5_qemu_cpu_unmap_range(impl->engine, address, size), "Unmap modern guest memory");
        ++impl->mappingGeneration;
        impl->ranges.swap(replacement);
        impl->removeCalls(address, size);
        impl->releaseUnused();
    });
}
void Machine::ReplaceBorrowed(std::uint64_t address, std::span<std::byte> memory, Permission permissions) {
    checkRange(address, memory.size());
    if (!memory.data()) throw std::invalid_argument("Guest borrowed replacement requires backing memory");
    const auto bits = permissionBits(permissions);
    impl->checkMapped(address, memory.size());
    const auto pointer = reinterpret_cast<std::uintptr_t>(memory.data());
    if (memory.size() > std::numeric_limits<std::uintptr_t>::max() - pointer)
        throw std::invalid_argument("Guest backing memory range overflows");
    auto replacement = impl->withoutRange(address, memory.size());
    replacement.push_back({address, memory.size(), memory.data(), bits, true});
    std::uint64_t backingId = 0;
    std::size_t offset = 0;
    for (const auto& backing : impl->backings) {
        const auto begin = reinterpret_cast<std::uintptr_t>(backing.pointer);
        if (pointer >= begin && pointer - begin <= backing.size && memory.size() <= backing.size - (pointer - begin)) {
            backingId = backing.id;
            offset = pointer - begin;
            break;
        }
        if (pointer < begin + backing.size && begin < pointer + memory.size())
            throw std::runtime_error("Guest borrowed replacement crosses a registered backing allocation");
    }
    const auto newBacking = backingId == 0;
    const auto hostPage = hostPageSize();
    if (newBacking && (memory.size() % hostPage || pointer % hostPage))
        throw std::runtime_error("Modern guest CPU borrowed replacement requires a complete aligned host-page backing or a slice of a registered backing");
    if (offset & 4095) throw std::runtime_error("Modern guest CPU backing aliases require a 4 KiB aligned offset");
    impl->checkMappingMutation();
    if (newBacking && impl->lastBackingIdentity == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Guest backing identity exhausted");
    auto candidateBackings = impl->backings;
    if (newBacking) candidateBackings.push_back({memory.data(), memory.size(), 0, impl->lastBackingIdentity + 1, {}});
    impl->transactOwned(replacement, candidateBackings, [&] {
        if (newBacking)
            impl->check(anyps5_qemu_cpu_register_backing(impl->engine, memory.data(), memory.size(), &backingId), "Register modern replacement backing");
        try {
            impl->check(anyps5_qemu_cpu_replace_alias(impl->engine, address, backingId, offset, memory.size(), bits), "Replace modern guest backing alias");
        } catch (...) {
            if (newBacking && anyps5_qemu_cpu_release_backing(impl->engine, backingId)) std::terminate();
            throw;
        }
        ++impl->mappingGeneration;
        if (newBacking) { candidateBackings.back().id = backingId; ++impl->lastBackingIdentity; }
        impl->backings.swap(candidateBackings);
        impl->ranges.swap(replacement);
        impl->removeCalls(address, memory.size());
        impl->releaseUnused();
    });
}
void Machine::Protect(std::uint64_t address, std::size_t size, Permission permissions) {
    checkRange(address, size);
    const auto bits = permissionBits(permissions);
    impl->checkMapped(address, size);
    std::vector<Impl::Range> replacement;
    replacement.reserve(impl->ranges.size() + 2);
    const auto end = address + size;
    for (const auto& range : impl->ranges) {
        const auto rangeEnd = range.address + range.size;
        if (range.address >= end || rangeEnd <= address) { replacement.push_back(range); continue; }
        const auto begin = std::max(address, range.address);
        const auto overlapEnd = std::min(end, rangeEnd);
        if (range.address < begin)
            replacement.push_back({range.address, static_cast<std::size_t>(begin - range.address), range.backing, range.permissions, range.borrowed});
        replacement.push_back({begin, static_cast<std::size_t>(overlapEnd - begin), range.backing + (begin - range.address), bits, range.borrowed});
        if (overlapEnd < rangeEnd)
            replacement.push_back({overlapEnd, static_cast<std::size_t>(rangeEnd - overlapEnd), range.backing + (overlapEnd - range.address), range.permissions, range.borrowed});
    }
    impl->checkMappingMutation();
    impl->transactOwned(replacement, impl->backings, [&] {
        impl->check(anyps5_qemu_cpu_protect_range(impl->engine, address, size, bits), "Protect modern guest memory");
        ++impl->mappingGeneration;
        impl->ranges.swap(replacement);
    });
}
void Machine::ProtectFragment(std::uint64_t address, std::size_t size, Permission permissions) {
    impl->checkOwner();
    if (impl->contextLifetime->running) throw std::logic_error("Guest fragment protection requires the idle CPU owner");
    if (!size || size > 4096 - (address & 4095) || size > std::numeric_limits<std::uint64_t>::max() - address)
        throw std::invalid_argument("Guest fragment protection requires a nonempty interval within one 4 KiB page without overflow");
    const auto bits = permissionBits(permissions);
    if (bits & 4) throw std::invalid_argument("Guest fragment protection supports only data permissions");
    const auto page = address & ~std::uint64_t{4095};
    impl->checkMapped(page, 4096);
    for (const auto& range : impl->ranges)
        if (range.address < page + 4096 && page < range.address + range.size && (range.permissions & 4))
            throw std::runtime_error("Guest fragment protection does not support executable shared pages");
    auto replacement = impl->withoutRange(address, size);
    replacement.reserve(impl->ranges.size() + 2);
    const auto end = address + size;
    for (const auto& range : impl->ranges) {
        const auto begin = std::max(address, range.address);
        const auto overlapEnd = std::min(end, range.address + range.size);
        if (begin < overlapEnd)
            replacement.push_back({begin, static_cast<std::size_t>(overlapEnd - begin),
                range.backing + (begin - range.address), bits, range.borrowed});
    }
    impl->checkMappingMutation();
    impl->transactOwned(replacement, impl->backings, [&] {
        impl->check(anyps5_qemu_cpu_protect_fragment(impl->engine, address, size, bits), "Protect modern guest data fragment");
        ++impl->mappingGeneration;
        impl->ranges.swap(replacement);
    });
}
std::vector<Mapping> Machine::Mappings() const {
    impl->checkOwner();
    std::vector<Mapping> result;
    result.reserve(impl->ranges.size());
    for (const auto& range : impl->ranges)
        result.push_back({range.address, range.size, static_cast<Permission>(range.permissions), range.borrowed});
    std::sort(result.begin(), result.end(), [](const Mapping& left, const Mapping& right) { return left.Address < right.Address; });
    return result;
}
OwnedMappingSnapshot Machine::PinOwnedMappings() const {
    impl->checkContextIdle();
    return impl->ownedSnapshot(impl->ranges, impl->backings, impl->mappingGeneration);
}
std::shared_ptr<const void> Machine::OwnedMappingScope() const {
    impl->checkOwner();
    return impl->mappingScope;
}
void Machine::SetOwnedMappingTransaction(OwnedMappingTransaction transaction) {
    impl->checkOwner();
    if (impl->activeOwnedTransaction) {
        impl->activeOwnedTransaction->violation.store(true);
        throw std::logic_error("Owned guest mapping transaction forbids replacing its active hook");
    }
    impl->checkContextIdle();
    if (impl->failedOwnedTransaction && transaction)
        throw std::runtime_error("Owned mapping transaction failed; only graphics-drained hook removal is allowed");
    impl->ownedTransaction = std::move(transaction);
}
void Machine::ValidateOwnedMappingCandidate(const OwnedMappingSnapshot& candidate) const {
    impl->checkOwner();
    const auto& frame = impl->activeOwnedTransaction;
    if (!frame || !frame->active.load() || frame->started || frame->violation.load() ||
        candidate.Generation != frame->candidate.Generation || !Impl::sameViews(candidate, frame->candidate))
        throw std::invalid_argument("Owned guest mapping candidate is not the exact active staged candidate");
}
void Machine::ValidateOwnedMappingCandidate(const OwnedMappingSnapshot& previous,
                                           const OwnedMappingSnapshot& candidate) const {
    ValidateOwnedMappingCandidate(candidate);
    const auto& staged = impl->activeOwnedTransaction->previous;
    if (previous.Generation != staged.Generation || !Impl::sameViews(previous, staged))
        throw std::invalid_argument("Owned guest mapping previous snapshot is not the exact active staged snapshot");
}
void Machine::ValidateOwnedMappings(const OwnedMappingSnapshot& snapshot) const {
    impl->checkOwner();
    if (snapshot.Scope != impl->mappingScope || snapshot.Scope.owner_before(impl->mappingScope) ||
        impl->mappingScope.owner_before(snapshot.Scope))
        throw std::invalid_argument("Owned guest mapping snapshot belongs to another Machine");
    for (std::size_t index = 0; index < snapshot.Views.size(); ++index) {
        const auto& view = snapshot.Views[index];
        const auto& region = view.Region;
        if (!region.Size || region.Size > std::numeric_limits<std::uint64_t>::max() - region.Address ||
            !view.BackingIdentity || !view.Owner || !view.Bytes.data() || view.Bytes.size() != region.Size ||
            (static_cast<unsigned>(region.Permissions) & ~7u))
            throw std::invalid_argument("Malformed owned guest mapping view");
        const auto end = region.Address + region.Size;
        for (std::size_t previous = 0; previous < index; ++previous) {
            const auto& other = snapshot.Views[previous].Region;
            if (region.Address < other.Address + other.Size && other.Address < end)
                throw std::invalid_argument("Owned guest mapping selection overlaps");
        }
        const auto backing = std::find_if(impl->backings.begin(), impl->backings.end(), [&](const auto& value) {
            return value.storage && value.identity == view.BackingIdentity;
        });
        if (backing == impl->backings.end() || view.Owner.get() != backing->storage.get() ||
            view.Owner.owner_before(backing->storage) || backing->storage.owner_before(view.Owner) ||
            view.Allocation.data() != backing->pointer || view.Allocation.size() != backing->size)
            throw std::invalid_argument("Owned guest mapping allocation is retired or does not match its owner");
        const auto bytes = reinterpret_cast<std::uintptr_t>(view.Bytes.data());
        const auto begin = reinterpret_cast<std::uintptr_t>(backing->pointer);
        if (bytes < begin || bytes - begin > backing->size || region.Size > backing->size - (bytes - begin))
            throw std::invalid_argument("Owned guest mapping view exceeds its allocation");
        auto cursor = region.Address;
        while (cursor < end) {
            const auto mapped = std::find_if(impl->ranges.begin(), impl->ranges.end(), [&](const auto& range) {
                return cursor >= range.address && cursor - range.address < range.size;
            });
            if (mapped == impl->ranges.end() || mapped->permissions != static_cast<unsigned>(region.Permissions) ||
                mapped->borrowed != region.Borrowed ||
                reinterpret_cast<std::uintptr_t>(mapped->backing) + (cursor - mapped->address) !=
                    bytes + (cursor - region.Address))
                throw std::invalid_argument("Owned guest mapping binding was retired, rebound or changed");
            cursor = std::min(end, mapped->address + mapped->size);
        }
    }
}
void Machine::CheckAccess(std::uint64_t address, std::size_t size, Permission permissions) const {
    if (!size) return;
    const auto bits = permissionBits(permissions);
    if (!bits) throw std::invalid_argument("Invalid guest access permissions");
    impl->checkMapped(address, size, bits);
}
void Machine::Read(std::uint64_t address, std::span<std::byte> output) const {
    CheckAccess(address, output.size(), Permission::Read);
    impl->transfer(address, output.data(), output.size(), false);
}
void Machine::Write(std::uint64_t address, std::span<const std::byte> input) {
    if (input.empty()) return;
    impl->checkMapped(address, input.size());
    impl->transfer(address, const_cast<std::byte*>(input.data()), input.size(), true);
    impl->refresh();
}
std::uint64_t Machine::Get(Register reg) const {
    std::uint64_t value = 0;
    impl->check(anyps5_qemu_cpu_get(impl->engine, registerId(reg), &value), "Read modern guest register");
    return value;
}
void Machine::Set(Register reg, std::uint64_t value) {
    impl->check(anyps5_qemu_cpu_set(impl->engine, registerId(reg), value), "Write modern guest register");
}
Machine::Context Machine::CaptureContext() {
    impl->checkContextIdle();
    auto payload = std::make_unique<Context::Payload>();
    payload->lifetime = impl->contextLifetime;
    impl->check(anyps5_qemu_cpu_context_create(impl->engine, &payload->value), "Capture modern guest execution context");
    payload->epoch = impl->epoch;
    return Context(std::move(payload));
}
void Machine::SaveContext(Context& context) {
    impl->checkContext(context);
    impl->check(anyps5_qemu_cpu_context_save(impl->engine, context.payload->value), "Save modern guest execution context");
    context.payload->epoch = impl->epoch;
}
void Machine::RestoreContext(const Context& context) {
    impl->checkContext(context);
    impl->check(anyps5_qemu_cpu_context_restore(impl->engine, context.payload->value), "Restore modern guest execution context");
    impl->epoch = context.payload->epoch;
}
Machine::SuspendedCall Machine::PauseHostCall() {
    impl->checkOwner();
    if (!impl->contextLifetime->running || !impl->activeCall)
        throw std::logic_error("Guest host-call suspension requires an active host callback");
    auto& active = *impl->activeCall;
    if (active.paused) throw std::logic_error("Guest host callback is already paused");
    if (Get(Register::Rip) != active.gate || Get(Register::Rsp) != active.stack)
        throw std::logic_error("Guest host callback changed its caller frame before suspension");
    if (active.stack > std::numeric_limits<std::uint64_t>::max() - 8)
        throw std::runtime_error("Guest suspended host-call return stack overflows");
    CheckAccess(active.gate, 1, Permission::Execute);
    CheckAccess(active.stack, 8, Permission::Read);
    std::uint64_t destination = 0;
    Read(active.stack, std::as_writable_bytes(std::span(&destination, 1)));
    CheckAccess(destination, 1, Permission::Execute);
    auto frame = std::make_shared<SuspendedFrame>(SuspendedFrame{active.gate, active.stack, destination, impl->epoch});
    auto payload = std::make_unique<SuspendedCall::Payload>();
    impl->pendingCalls.reserve(impl->pendingCalls.size() + 1);
    payload->frame = frame;
    payload->lifetime = impl->contextLifetime;
    impl->pendingCalls.push_back(frame);
    active.paused = std::move(frame);
    return SuspendedCall(std::move(payload));
}
void Machine::ValidateSuspendedCall(const SuspendedCall& call) const {
    impl->checkSuspendedCall(call);
    if (impl->exited || impl->requested.load())
        throw std::logic_error("Suspended guest host call cannot complete after terminal stop or exit");
    const auto& frame = *call.payload->frame;
    if (impl->epoch != frame.epoch || Get(Register::Rip) != frame.gate || Get(Register::Rsp) != frame.stack)
        throw std::logic_error("Suspended guest host call requires its restored caller frame");
    if (!impl->calls.contains(frame.gate)) throw std::logic_error("Suspended guest host-call gate is no longer registered");
    CheckAccess(frame.gate, 1, Permission::Execute);
    CheckAccess(frame.stack, 8, Permission::Read);
    std::uint64_t destination = 0;
    Read(frame.stack, std::as_writable_bytes(std::span(&destination, 1)));
    if (destination != frame.destination) throw std::logic_error("Suspended guest host-call return word changed");
    CheckAccess(destination, 1, Permission::Execute);
}
void Machine::CompleteHostCall(SuspendedCall& call) {
    ValidateSuspendedCall(call);
    auto& frame = *call.payload->frame;
    Set(Register::Rsp, frame.stack + 8);
    Set(Register::Rip, frame.destination);
    frame.completed = true;
    std::erase(impl->pendingCalls, call.payload->frame);
}
void Machine::SetSyscallHandler(std::function<void(Machine&)> handler) {
    impl->checkOwner();
    impl->syscall = std::move(handler);
}
void Machine::AddHostCall(std::uint64_t address, std::function<void(Machine&)> handler) {
    if (!handler) throw std::invalid_argument("Guest host import requires a handler");
    impl->checkOwner();
    if (impl->calls.contains(address)) throw std::invalid_argument("Guest host import address is already registered");
    CheckAccess(address, 1, Permission::Execute);
    impl->calls.emplace(address, std::move(handler));
    try {
        impl->check(anyps5_qemu_cpu_add_gate(impl->engine, address, address), "Register modern guest host gate");
    } catch (...) {
        impl->calls.erase(address);
        throw;
    }
}
StopReason Machine::Run(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit) {
    impl->checkExecutionMutation();
    if (impl->contextLifetime->running) throw std::logic_error("Guest execution is already running");
    if (!instructionLimit) throw std::invalid_argument("Guest execution requires a nonzero instruction limit");
    impl->retireAbandonedCalls();
    if (!impl->pendingCalls.empty()) throw std::logic_error("Guest session reset requires completion of all suspended host calls");
    impl->checkPendingFrame(entry, Get(Register::Rsp));
    if (impl->lastEpoch == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Guest execution session identity is exhausted");
    impl->check(anyps5_qemu_cpu_clear_stop(impl->engine), "Reset modern guest stop request");
    impl->requested.store(false);
    impl->exited = false;
    // Restored contexts keep their original identity; only a fresh session may
    // reuse an abandoned gate and stack layout without replaying that frame.
    impl->epoch = ++impl->lastEpoch;
    Set(Register::Rip, entry);
    return RunSlice(entry, until, instructionLimit);
}
StopReason Machine::RunSlice(std::uint64_t entry, std::uint64_t until, std::uint64_t instructionLimit) {
    impl->checkExecutionMutation();
    if (impl->contextLifetime->running) throw std::logic_error("Guest execution is already running");
    if (!instructionLimit) throw std::invalid_argument("Guest execution requires a nonzero instruction limit");
    // A sticky terminal observation never dispatches or resets a guest frame.
    // Keep abandoned-frame checks on every path that can execute instructions.
    if (impl->exited || impl->requested.load()) {
        impl->lastRunInstructions = 0;
        return impl->exited ? StopReason::Exit : StopReason::Requested;
    }
    impl->retireAbandonedCalls();
    impl->checkRetiredFrame(Get(Register::Rip), Get(Register::Rsp));
    if (Get(Register::Rip) != entry) throw std::invalid_argument("Guest execution slice requires the current continuation PC");
    impl->checkPendingFrame(entry, Get(Register::Rsp));
    impl->contextLifetime->running = true;
    std::uint64_t executed = 0;
    struct Reset {
        Impl& value;
        std::uint64_t& executed;
        ~Reset() { value.lastRunInstructions = executed; value.contextLifetime->running = false; }
    } reset{*impl, executed};
    for (;;) {
        if (impl->exited) return StopReason::Exit;
        if (impl->requested.load()) return StopReason::Requested;
        if (Get(Register::Rip) == until) return StopReason::Address;
        if (executed == instructionLimit) return StopReason::InstructionLimit;
        if (std::any_of(impl->ranges.begin(), impl->ranges.end(), [](const Impl::Range& range) { return range.borrowed; })) impl->refresh();
        AnyPS5QemuRunResult result{};
        impl->check(anyps5_qemu_cpu_run_until(impl->engine, instructionLimit - executed, until, &result), "Execute modern x86-64 guest");
        if (result.instructions > instructionLimit - executed) throw std::runtime_error("Modern guest CPU exceeded its instruction budget");
        executed += result.instructions;
        switch (result.reason) {
        case ANYPS5_QEMU_BUDGET:
            if (!result.instructions) throw std::runtime_error("Modern guest CPU budget stop made no progress");
            break;
        case ANYPS5_QEMU_HOST_GATE: {
            impl->checkPendingFrame(result.rip, Get(Register::Rsp));
            ++executed;
            const auto found = impl->calls.find(result.gate);
            if (found == impl->calls.end()) throw std::runtime_error("Modern guest CPU returned an unknown host gate");
            auto callback = found->second;
            Impl::ActiveCall active{result.rip, Get(Register::Rsp), {}};
            impl->activeCall = &active;
            struct ClearActive { Impl& value; ~ClearActive() { value.activeCall = nullptr; } } clearActive{*impl};
            callback(*this);
            if (active.paused) {
                if (Get(Register::Rip) != active.gate || Get(Register::Rsp) != active.stack)
                    throw std::logic_error("Paused guest host callback changed its caller frame");
                if (impl->exited) return StopReason::Exit;
                if (impl->requested.load()) return StopReason::Requested;
                return StopReason::Paused;
            }
            if (!impl->exited && !impl->requested.load()) {
                const auto stack = Get(Register::Rsp);
                if (stack > std::numeric_limits<std::uint64_t>::max() - 8)
                    throw std::runtime_error("Guest host import return stack overflows");
                try { CheckAccess(stack, 8, Permission::Read); }
                catch (const std::exception&) { throw std::runtime_error("Guest host import return stack is not readable"); }
                std::uint64_t destination = 0;
                Read(stack, std::as_writable_bytes(std::span(&destination, 1)));
                Set(Register::Rsp, stack + sizeof(destination));
                Set(Register::Rip, destination);
            }
            break;
        }
        case ANYPS5_QEMU_SYSCALL: {
            if (!impl->syscall) throw std::runtime_error("Unsupported guest syscall " + std::to_string(Get(Register::Rax)));
            auto callback = impl->syscall;
            callback(*this);
            break;
        }
        case ANYPS5_QEMU_REQUESTED_STOP:
            impl->requested.store(true);
            return StopReason::Requested;
        case ANYPS5_QEMU_STOP_ADDRESS:
            if (result.rip != until) throw std::runtime_error("Modern guest CPU stopped at an unexpected address");
            return StopReason::Address;
        case ANYPS5_QEMU_FAULT: {
            std::ostringstream message;
            message << "Guest " << ((result.error_code & 1) ? "protected " : "unmapped ")
                << ((result.error_code & 16) ? "instruction fetch" : ((result.error_code & 2) ? "write" : "read"))
                << " at 0x" << std::hex << result.address << " (vector " << std::dec << result.vector << ")";
            throw std::runtime_error(message.str());
        }
        case ANYPS5_QEMU_UNSUPPORTED: {
            if (result.vector == 13) impl->rejectPrivileged(result.rip, Get(Register::Rdx));
            std::ostringstream message;
            if (result.vector == 6) message << "Unsupported guest instruction";
            else if (result.vector == 13) message << "Unsupported guest privileged service instruction";
            else message << "Unsupported guest interrupt " << result.vector;
            message << " at 0x" << std::hex << result.rip;
            throw std::runtime_error(message.str());
        }
        default:
            throw std::runtime_error("Modern guest CPU returned an unknown stop reason");
        }
    }
}
std::uint64_t Machine::LastRunInstructions() const {
    impl->checkOwner();
    return impl->lastRunInstructions;
}
void Machine::Exit(int code) {
    impl->checkOwner();
    impl->exitCode = code;
    impl->exited = true;
}
void Machine::RequestStop() {
    impl->requested.store(true);
    anyps5_qemu_cpu_stop(impl->engine);
}
int Machine::ExitCode() const { return impl->exitCode; }
const char* Machine::Backend() { return "Modern QEMU TCG x86-64 dynamic translation"; }
}
