#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Cpu {

namespace {
constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t StackSize = 1024 * 1024;
constexpr std::uint64_t ArenaBase = 0x7ff900000000;
constexpr std::uint64_t SlotSize = 32 * 1024 * 1024;
constexpr std::uint64_t SlotCount = 256;
constexpr std::uint64_t ReturnGate = 0x7ff8ffff0000;
constexpr std::uint64_t HandleBase = 0x4150533500000000;
constexpr std::uint64_t Quantum = 4096;
constexpr std::uint32_t Invalid = 0x80020016;
constexpr std::uint32_t NotSupported = 0x8002002d;
// Process-wide monotonic identities prevent destroyed slots or replacement
// schedulers from accepting a previous scheduler's opaque attribute token.
std::atomic<std::uint64_t> NextAttributeToken{0xa006000000000003ULL};

std::uint64_t attributeToken() {
    auto value = NextAttributeToken.load();
    for (;;) {
        if (value == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("Translated guest thread attribute identity exhausted");
        if (NextAttributeToken.compare_exchange_weak(value, value + 1)) return value;
    }
}

[[noreturn]] void fail(const char* message) {
    throw std::runtime_error(std::string("Translated guest threads: ") + message);
}

bool overlaps(std::uint64_t left, std::uint64_t length, const Mapping& right) {
    return left < right.Address + right.Size && right.Address < left + length;
}

void write64(Machine& machine, std::uint64_t address, std::uint64_t value) {
    machine.CheckAccess(address, 8, Permission::Write);
    machine.Write(address, std::as_bytes(std::span(&value, 1)));
}
}

struct GuestThreads::Impl {
    enum class State { Runnable, Running, BlockedJoining, BlockedWaiting, Finishing, Finished, Reaped, Control, Cancelled };
    enum class Action { Create, Yield, Join, Wait, ExitThread, TerminateEntry, ExitProcess };
    enum class Phase { None, Entry, Module };
    struct Pending {
        Action kind;
        std::array<std::uint64_t, 5> args{};
        Machine::SuspendedCall token;
        bool awakened = false;
    };
    struct Record {
        GuestThreadHandle id;
        bool initial = false;
        bool finalizing = false;
        bool stoppedNotified = false;
        State state = State::Runnable;
        Machine::Context context;
        std::shared_ptr<SceTls> tls;
        Mapping stack{};
        std::uint64_t errnoAddress = 0;
        std::uint64_t result = 0;
        std::int32_t basePriority = GuestThreadPriorityDefault;
        std::int32_t effectivePriority = GuestThreadPriorityDefault;
        std::uint64_t returnStack = 0;
        std::optional<GuestThreadHandle> joiner;
        std::unique_ptr<Pending> pending;
        std::vector<Mapping> owned;
    };

    Machine& machine;
    const std::thread::id owner = std::this_thread::get_id();
    std::map<GuestThreadHandle, std::unique_ptr<Record>> records;
    struct Attribute {
        std::uint64_t token;
        std::int32_t priority = GuestThreadPriorityDefault;
        std::int32_t inherit = 4;
    };
    std::map<std::uint64_t, Attribute> attributes;
    std::deque<GuestThreadHandle> runnable;
    GuestThreadHandle initial = 0;
    GuestThreadHandle active = 0;
    std::uint64_t nextSlot = 0;
    std::uint64_t nextWaitDomain = 1;
    struct Domain {
        std::function<void(GuestThreadHandle)> stopped;
        std::map<std::uint64_t, GuestThreadHandle> inheritanceOwners;
    };
    std::map<std::uint64_t, Domain> waitDomains;
    std::function<std::shared_ptr<SceTls>(std::uint64_t)> tlsFactory;
    std::uint64_t dtors = 0, count = 0, report = 0;
    bool withdrawn = false;
    bool driving = false;
    bool gateMapped = false;
    Phase phase = Phase::None;
    std::optional<StopReason> terminal;
    GuestEntryControl control;
    std::optional<Machine::SuspendedCall> controlToken;
    GuestThreadHandle controlOwner = 0;
    GuestThreadHandle moduleOwner = 0;
    std::uint64_t moduleUntil = 0;

    explicit Impl(Machine& value) : machine(value) {}

    void checkOwner() const {
        if (std::this_thread::get_id() != owner) fail("runtime requires its owner thread");
        if (withdrawn) fail("runtime was withdrawn");
    }
    Record& lookup(GuestThreadHandle id) const {
        const auto found = records.find(id);
        if (found == records.end() || found->second->state == State::Reaped)
            fail("unknown or retired guest thread handle");
        return *found->second;
    }
    Record& current() const {
        checkOwner();
        if (!active) fail("there is no active guest thread");
        return lookup(active);
    }
    Attribute* attribute(std::uint64_t slot8) {
        if (!slot8) return nullptr;
        if (slot8 > std::numeric_limits<std::uint64_t>::max() - 8)
            fail("guest thread attribute slot overflows");
        std::uint64_t token = 0;
        machine.Read(slot8, std::as_writable_bytes(std::span(&token, 1)));
        const auto found = attributes.find(slot8);
        if (found == attributes.end() || found->second.token != token) return nullptr;
        return &found->second;
    }
    void checkParameter(std::uint64_t parameter4, Permission permission) const {
        if (!parameter4 || parameter4 > std::numeric_limits<std::uint64_t>::max() - 4)
            fail("guest thread scheduling parameter span is invalid");
        machine.CheckAccess(parameter4, 4, permission);
    }
    void checkFree(std::uint64_t address, std::uint64_t size) const {
        for (const auto& mapping : machine.Mappings())
            if (overlaps(address, size, mapping)) fail("guest thread allocation overlaps an existing mapping");
    }
    std::uint64_t slot() {
        if (nextSlot == SlotCount) fail("guest thread allocation arena is exhausted");
        const auto address = ArenaBase + SlotSize * nextSlot++;
        checkFree(address, SlotSize);
        return address;
    }
    void map(Record& record, std::uint64_t address, std::size_t size, Permission permissions) {
        machine.Map(address, size, permissions);
        record.owned.push_back({address, size, permissions, false});
    }
    void retire(Record& record) {
        record.pending.reset();
        record.context = Machine::Context();
        record.tls.reset();
        for (auto mapping = record.owned.rbegin(); mapping != record.owned.rend(); ++mapping)
            machine.Unmap(mapping->Address, mapping->Size);
        record.owned.clear();
    }
    void activate(Record& record) {
        active = record.id;
        machine.RestoreContext(record.context);
        if (machine.Get(Register::FsBase) != (record.tls ? record.tls->FsBase() : 0))
            fail("restored guest thread FS does not match its TLS instance");
    }
    void enqueue(Record& record, bool first = false) {
        if (std::find(runnable.begin(), runnable.end(), record.id) != runnable.end()) return;
        if (first) runnable.push_front(record.id);
        else runnable.push_back(record.id);
    }
    void ready(Record& record, bool first = false) {
        record.state = record.finalizing ? State::Finishing : State::Runnable;
        enqueue(record, first);
    }
    static bool live(const Record& record) {
        return record.state != State::Finished && record.state != State::Reaped &&
               record.state != State::Cancelled;
    }
    void recomputePriorities() {
        // Rebuild from base priorities every time. Monotone propagation then
        // converges even for cycles, without retaining withdrawn donations.
        for (auto& [id, record] : records) record->effectivePriority = record->basePriority;
        bool changed;
        do {
            changed = false;
            for (const auto& [id, waiter] : records) {
                if (waiter->state != State::BlockedWaiting || !waiter->pending ||
                    waiter->pending->kind != Action::Wait || waiter->pending->awakened) continue;
                const auto domain = waitDomains.find(waiter->pending->args[0]);
                if (domain == waitDomains.end()) continue;
                const auto ownership = domain->second.inheritanceOwners.find(waiter->pending->args[1]);
                if (ownership == domain->second.inheritanceOwners.end()) continue;
                const auto found = records.find(ownership->second);
                if (found == records.end() || !live(*found->second)) continue;
                auto& owner = *found->second;
                if (waiter->effectivePriority < owner.effectivePriority) {
                    owner.effectivePriority = waiter->effectivePriority;
                    changed = true;
                }
            }
        } while (changed);
    }
    void setInheritanceOwner(std::uint64_t domain, std::uint64_t key, GuestThreadHandle ownerId) {
        checkOwner();
        if (!key) fail("guest inheritance key is null");
        const auto found = waitDomains.find(domain);
        if (found == waitDomains.end()) fail("guest wait domain was withdrawn");
        if (ownerId) {
            if (!live(lookup(ownerId))) fail("guest inheritance owner has stopped");
            found->second.inheritanceOwners[key] = ownerId;
        } else found->second.inheritanceOwners.erase(key);
        recomputePriorities();
    }
    void notifyStopped(Record& record) {
        if (record.stoppedNotified) return;
        record.stoppedNotified = true;
        // A notification may change provider state. Copy callbacks so domain
        // registration lifetime never invalidates iteration.
        std::vector<std::function<void(GuestThreadHandle)>> callbacks;
        callbacks.reserve(waitDomains.size());
        for (const auto& [id, domain] : waitDomains) callbacks.push_back(domain.stopped);
        for (const auto& callback : callbacks) if (callback) callback(record.id);
        for (auto& [id, domain] : waitDomains)
            std::erase_if(domain.inheritanceOwners, [&](const auto& item) { return item.second == record.id; });
        recomputePriorities();
    }
    void cancelRecord(Record& record) {
        if (record.state == State::Finished || record.state == State::Reaped) return;
        record.state = State::Cancelled;
        if (record.pending && record.pending->kind == Action::Wait) record.pending.reset();
        notifyStopped(record);
    }
    bool isWaiting(std::uint64_t domain, GuestThreadHandle id, std::uint64_t key) const {
        checkOwner();
        if (!waitDomains.contains(domain)) fail("guest wait domain was withdrawn");
        const auto found = records.find(id);
        if (found == records.end()) return false;
        const auto& record = *found->second;
        return record.state == State::BlockedWaiting && record.pending &&
            record.pending->kind == Action::Wait && !record.pending->awakened &&
            record.pending->args[0] == domain && record.pending->args[1] == key;
    }
    bool wake(std::uint64_t domain, GuestThreadHandle id, std::uint64_t key, std::uint32_t result) {
        if (!isWaiting(domain, id, key)) return false;
        auto& record = lookup(id);
        record.pending->args[2] = result;
        record.pending->awakened = true;
        ready(record);
        recomputePriorities();
        return true;
    }
    void withdrawWaitDomain(std::uint64_t domain) {
        if (withdrawn) return;
        checkOwner();
        if (driving) fail("cannot withdraw a guest wait domain during guest execution");
        if (!waitDomains.contains(domain)) return;
        const bool pending = std::any_of(records.begin(), records.end(), [&](const auto& item) {
            const auto& record = *item.second;
            return record.pending && record.pending->kind == Action::Wait &&
                record.pending->args[0] == domain;
        });
        if (pending) {
            machine.RequestStop();
            cancel(StopReason::Requested);
        }
        waitDomains.erase(domain);
        recomputePriorities();
    }
    void registerCallback(std::uint64_t& storage, std::uint64_t pc) {
        checkOwner();
        if (!pc) fail("guest callback PC is null");
        if (pc == ReturnGate) fail("guest callback cannot use the runtime return sentinel as its entry");
        machine.CheckAccess(pc, 1, Permission::Execute);
        if (storage) fail("guest callback registration is duplicated");
        storage = pc;
    }
    void queue(Action action, std::array<std::uint64_t, 5> args = {}) {
        auto& record = current();
        if (!driving || record.state != State::Running || record.pending)
            fail("thread action requires a single active guest host call");
        if (control.Kind == GuestEntryControlKind::ProcessExit &&
            (action == Action::Create || action == Action::Join || action == Action::ExitThread))
            fail("thread creation, join and completion during process finalization are unsupported");
        auto pending = std::make_unique<Pending>();
        pending->kind = action;
        pending->args = args;
        pending->token = machine.PauseHostCall();
        record.pending = std::move(pending);
    }
    void complete(Record& record, bool status) {
        if (!record.pending) fail("guest host call has no pending operation");
        machine.ValidateSuspendedCall(record.pending->token);
        if (status) machine.Set(Register::Rax, 0);
        machine.CompleteHostCall(record.pending->token);
        record.pending.reset();
        machine.SaveContext(record.context);
    }
    void checkName(std::uint64_t address) const {
        if (!address) return;
        for (std::uint64_t offset = 0; offset != 256; ++offset) {
            if (offset > std::numeric_limits<std::uint64_t>::max() - address) fail("guest thread name overflows");
            std::byte value{};
            machine.Read(address + offset, std::span(&value, 1));
            if (value == std::byte{}) return;
        }
        fail("guest thread name exceeds the supported 255-byte bounded policy");
    }
    std::uint64_t callFrame(std::uint64_t pc, std::uint64_t sentinel, const std::array<std::uint64_t, 3>& arguments) {
        if (pc == sentinel || pc == ReturnGate) fail("guest callback entry is a reserved return sentinel");
        machine.CheckAccess(pc, 1, Permission::Execute);
        machine.CheckAccess(sentinel, 1, Permission::Execute);
        const auto oldStack = machine.Get(Register::Rsp);
        if (oldStack < 144) fail("guest callback stack underflows");
        const auto stack = ((oldStack - 128) & ~std::uint64_t{15}) - 8;
        machine.CheckAccess(stack, static_cast<std::size_t>(oldStack - stack), Permission::Read | Permission::Write);
        write64(machine, stack, sentinel);
        machine.Set(Register::Rsp, stack);
        machine.Set(Register::Rip, pc);
        machine.Set(Register::Rdi, arguments[0]);
        machine.Set(Register::Rsi, arguments[1]);
        machine.Set(Register::Rdx, arguments[2]);
        return stack + 8;
    }
    void create(Record& parent) {
        const auto args = parent.pending->args;
        auto priority = parent.basePriority;
        if (args[1]) {
            const auto attr = attribute(args[1]);
            if (!attr)
                fail("nondefault guest thread attributes are unsupported unless initialized by this scheduler");
            if (attr->inherit == 0) priority = attr->priority;
        }
        if (!tlsFactory) fail("guest thread creation requires a frozen TLS factory");
        machine.CheckAccess(args[0], 8, Permission::Write);
        if (overlaps(args[0], 8, {machine.Get(Register::Rsp), 8, Permission::Read, false}))
            fail("guest thread output overlaps its pending return word");
        machine.CheckAccess(args[2], 1, Permission::Execute);
        if (args[2] == ReturnGate) fail("guest thread entry is a reserved return sentinel");
        checkName(args[4]);
        if (machine.Get(Register::GsBase)) fail("guest child GS inheritance is unsupported outside the zero-GS contract");
        const auto base = slot();
        auto child = std::make_unique<Record>();
        child->id = HandleBase + nextSlot;
        child->basePriority = priority;
        child->effectivePriority = child->basePriority;
        child->owned.reserve(5);
        auto saved = machine.CaptureContext();
        bool inserted = false;
        try {
            map(*child, base, PageSize, static_cast<Permission>(0));
            child->stack = {base + PageSize, StackSize, Permission::Read | Permission::Write, false};
            map(*child, child->stack.Address, child->stack.Size, child->stack.Permissions);
            map(*child, base + PageSize + StackSize, PageSize, static_cast<Permission>(0));
            child->errnoAddress = base + StackSize + 2 * PageSize;
            map(*child, child->errnoAddress, PageSize, Permission::Read | Permission::Write);
            const auto previousMappings = machine.Mappings();
            child->tls = tlsFactory(base + 2 * StackSize);
            if (child->tls) {
                const auto allocation = child->tls->Allocation();
                if (allocation.Address != base + 2 * StackSize || !allocation.Size ||
                    allocation.Size % PageSize || allocation.Size > std::numeric_limits<std::uint64_t>::max() - allocation.Address ||
                    allocation.Borrowed || std::any_of(previousMappings.begin(), previousMappings.end(), [&](const Mapping& mapping) {
                        return overlaps(allocation.Address, allocation.Size, mapping);
                    })) fail("guest TLS factory did not return a new owned allocation at the requested address");
                const auto currentMappings = machine.Mappings();
                if (std::none_of(currentMappings.begin(), currentMappings.end(), [&](const Mapping& mapping) {
                    return mapping.Address == allocation.Address && mapping.Size == allocation.Size &&
                        !mapping.Borrowed && mapping.Permissions == allocation.Permissions;
                })) fail("guest TLS factory allocation does not match its new mapped extent");
                if (allocation.Size > SlotSize - 2 * StackSize || allocation.Permissions != (Permission::Read | Permission::Write)) {
                    machine.Unmap(allocation.Address, allocation.Size);
                    child->tls.reset();
                    fail("guest TLS instance exceeds its thread allocation slot");
                }
                child->owned.push_back(allocation);
            }
            constexpr std::array registers{Register::Rax, Register::Rbx, Register::Rcx, Register::Rdx,
                Register::Rsi, Register::Rdi, Register::Rbp, Register::Rsp, Register::R8, Register::R9,
                Register::R10, Register::R11, Register::R12, Register::R13, Register::R14, Register::R15};
            for (const auto reg : registers) machine.Set(reg, 0);
            machine.Set(Register::Rflags, 2);
            machine.Set(Register::GsBase, 0);
            if (child->tls) child->tls->Activate();
            else machine.Set(Register::FsBase, 0);
            const auto stack = child->stack.Address + child->stack.Size - 8;
            child->returnStack = stack + 8;
            write64(machine, stack, ReturnGate);
            machine.Set(Register::Rsp, stack);
            machine.Set(Register::Rdi, args[3]);
            machine.Set(Register::Rip, args[2]);
            child->context = machine.CaptureContext();
            machine.RestoreContext(saved);
            machine.ValidateSuspendedCall(parent.pending->token);
            const auto id = child->id;
            auto [position, created] = records.emplace(id, std::move(child));
            if (!created) fail("guest thread identity collision");
            inserted = true;
            try {
                enqueue(*position->second);
                enqueue(parent, true);
                write64(machine, args[0], id);
            } catch (...) {
                std::erase(runnable, id);
                std::erase(runnable, parent.id);
                child = std::move(position->second);
                records.erase(position);
                inserted = false;
                throw;
            }
            complete(parent, true);
            ready(parent, true);
        } catch (...) {
            machine.RestoreContext(saved);
            if (!inserted && child) retire(*child);
            throw;
        }
    }
    void finishJoin(Record& joiner, Record& target) {
        activate(joiner);
        const auto output = joiner.pending->args[1];
        if (output) machine.CheckAccess(output, 8, Permission::Write);
        machine.ValidateSuspendedCall(joiner.pending->token);
        if (output) write64(machine, output, target.result);
        complete(joiner, true);
        ready(joiner);
        retire(target);
        target.state = State::Reaped;
    }
    void join(Record& joiner) {
        const auto args = joiner.pending->args;
        auto& target = lookup(args[0]);
        if (target.id == joiner.id) fail("guest thread cannot join itself");
        if (target.initial) fail("joining the initial process thread is unsupported");
        if (target.joiner) fail("guest thread already has a join owner");
        if (args[1]) {
            machine.CheckAccess(args[1], 8, Permission::Write);
            if (overlaps(args[1], 8, {machine.Get(Register::Rsp), 8, Permission::Read, false}))
                fail("guest join output overlaps its pending return word");
            if (std::any_of(target.owned.begin(), target.owned.end(), [&](const Mapping& mapping) {
                return overlaps(args[1], 8, mapping);
            })) fail("guest join output overlaps the target's retiring allocation");
        }
        if (target.state == State::Cancelled) fail("cancelled guest thread cannot report successful join");
        target.joiner = joiner.id;
        if (target.state == State::Finished) finishJoin(joiner, target);
        else joiner.state = State::BlockedJoining;
    }
    void publishFinished(Record& record) {
        record.state = State::Finished;
        notifyStopped(record);
        if (record.joiner) finishJoin(lookup(*record.joiner), record);
    }
    void beginFinish(Record& record, std::uint64_t result) {
        if (record.initial) fail("initial process thread cannot use pthread completion");
        if (record.finalizing) fail("thread exit during its finalizer is unsupported");
        record.result = result;
        record.pending.reset();
        if (!dtors) {
            publishFinished(record);
            return;
        }
        record.returnStack = callFrame(dtors, ReturnGate, {});
        record.finalizing = true;
        machine.SaveContext(record.context);
        ready(record, true);
    }
    void process(Record& record) {
        if (!record.pending) fail("paused guest has no scheduled action");
        switch (record.pending->kind) {
        case Action::Create: create(record); break;
        case Action::Yield:
            complete(record, false);
            ready(record);
            break;
        case Action::Join: join(record); break;
        case Action::Wait:
            if (!waitDomains.contains(record.pending->args[0])) fail("guest wait domain was withdrawn before suspension");
            record.state = State::BlockedWaiting;
            recomputePriorities();
            break;
        case Action::ExitThread: beginFinish(record, record.pending->args[0]); break;
        case Action::TerminateEntry:
        case Action::ExitProcess:
            if (phase != Phase::Entry || control.Kind != GuestEntryControlKind::None ||
                (record.pending->kind == Action::TerminateEntry && !record.initial))
                fail("process entry control is unsupported in the current guest execution");
            control.Kind = record.pending->kind == Action::TerminateEntry
                ? GuestEntryControlKind::TerminationCallback : GuestEntryControlKind::ProcessExit;
            controlOwner = record.id;
            if (control.Kind == GuestEntryControlKind::ProcessExit) {
                control.ExitCode = static_cast<int>(static_cast<std::int32_t>(record.pending->args[0]));
                // Process exit abandons every other continuation without
                // publishing pthread completion, destructors or join results.
                runnable.clear();
                for (auto& [id, other] : records)
                    if (id != record.id) cancelRecord(*other);
            }
            controlToken.emplace(std::move(record.pending->token));
            record.pending.reset();
            record.state = State::Control;
            break;
        }
    }
    StopReason cancel(StopReason reason) {
        terminal = reason;
        runnable.clear();
        for (auto& [id, record] : records) cancelRecord(*record);
        return reason;
    }
    std::optional<StopReason> observeStop() {
        auto saved = machine.CaptureContext();
        try {
            machine.Set(Register::Rip, ReturnGate);
            const auto reason = machine.RunSlice(ReturnGate, ReturnGate, 1);
            if (machine.LastRunInstructions()) fail("idle terminal observation executed guest instructions");
            machine.RestoreContext(saved);
            if (reason == StopReason::Requested || reason == StopReason::Exit) return reason;
            if (reason != StopReason::Address) fail("idle terminal observation did not stop at its sentinel");
            return std::nullopt;
        } catch (...) {
            machine.RestoreContext(saved);
            throw;
        }
    }
    StopReason drive(GuestPhaseBudget& budget) {
        for (;;) {
            if (terminal) return *terminal;
            if (const auto stopped = observeStop()) return cancel(*stopped);
            if (!budget.Remaining()) return StopReason::InstructionLimit;
            if (runnable.empty()) fail("guest wait/join graph has no runnable work and cannot make progress");
            // Stable selection leaves equal-priority FIFO order intact while
            // ABI continuation front insertions cannot outrank donated owners.
            const auto selected = std::min_element(runnable.begin(), runnable.end(), [&](auto left, auto right) {
                return lookup(left).effectivePriority < lookup(right).effectivePriority;
            });
            auto& record = lookup(*selected);
            runnable.erase(selected);
            if (record.state != State::Runnable && record.state != State::Finishing)
                fail("guest run queue contains an ineligible thread");
            activate(record);
            if (record.pending && record.pending->kind == Action::Wait) {
                if (!record.pending->awakened || !waitDomains.contains(record.pending->args[0]))
                    fail("runnable guest wait lacks a matching live wake");
                try {
                    machine.ValidateSuspendedCall(record.pending->token);
                    machine.Set(Register::Rax, record.pending->args[2]);
                    complete(record, false);
                } catch (...) {
                    // Ownership may already be reserved by the provider. A
                    // rejected return must abandon every continuation, never
                    // leave an awakened waiter outside the runnable queue.
                    machine.RequestStop();
                    cancel(StopReason::Requested);
                    throw;
                }
            }
            record.state = State::Running;
            const auto until = phase == Phase::Module && record.id == moduleOwner
                ? moduleUntil : (record.initial ? 0 : ReturnGate);
            StopReason reason;
            try {
                reason = machine.RunSlice(machine.Get(Register::Rip), until, std::min(Quantum, budget.Remaining()));
            } catch (...) {
                budget.Charge(machine.LastRunInstructions());
                machine.SaveContext(record.context);
                throw;
            }
            budget.Charge(machine.LastRunInstructions());
            machine.SaveContext(record.context);
            if (reason == StopReason::Requested || reason == StopReason::Exit) return cancel(reason);
            if (const auto stopped = observeStop()) return cancel(*stopped);
            if (reason == StopReason::InstructionLimit) {
                // Internal translation slices do not turn guest FIFO into
                // round-robin scheduling. The current continuation precedes
                // equal-priority arrivals until it yields, blocks or finishes.
                ready(record, true);
                if (!budget.Remaining()) return reason;
            } else if (reason == StopReason::Paused) {
                process(record);
                if (control.Kind != GuestEntryControlKind::None && phase == Phase::Entry) return StopReason::Paused;
            } else if (reason == StopReason::Address) {
                if (machine.Get(Register::Rsp) != record.returnStack)
                    fail("guest callback reached its return sentinel without unwinding its call frame");
                if (record.initial || (phase == Phase::Module && record.id == moduleOwner)) {
                    record.state = State::Runnable;
                    return reason;
                }
                if (record.finalizing) publishFinished(record);
                else beginFinish(record, machine.Get(Register::Rax));
            } else fail("guest scheduler received an unsupported stop reason");
        }
    }
    struct Execution {
        Impl& impl;
        Phase previous;
        Execution(Impl& value, Phase selected) : impl(value), previous(value.phase) {
            value.checkOwner();
            if (value.driving) fail("nested public guest execution is unsupported");
            if (!value.initial) fail("guest execution requires an adopted initial thread");
            value.driving = true;
            value.phase = selected;
        }
        ~Execution() { impl.phase = previous; impl.driving = false; }
    };
    StopReason runEntry(GuestPhaseBudget& budget) {
        Execution execution(*this, Phase::Entry);
        if (terminal) {
            const auto reason = machine.RunSlice(machine.Get(Register::Rip), 0, 1);
            const auto instructions = machine.LastRunInstructions();
            if (reason != *terminal || instructions) fail("terminal guest continuation did not preserve its sticky stop");
            budget.Charge(instructions);
            return reason;
        }
        if (control.Kind != GuestEntryControlKind::None) return StopReason::Paused;
        auto& record = lookup(initial);
        if (record.state == State::Runnable) enqueue(record, true);
        return drive(budget);
    }
    GuestCallResult invokeModule(const GuestModuleCall& call, GuestPhaseBudget& budget) {
        Execution execution(*this, Phase::Module);
        if (terminal) return {*terminal, std::nullopt};
        auto& record = lookup(control.Kind == GuestEntryControlKind::None ? initial : controlOwner);
        if (record.state != State::Runnable && record.state != State::Control)
            fail("module callback requires an idle execution continuation");
        if (control.Kind != GuestEntryControlKind::None && call.Kind != GuestModuleCallKind::Finalize)
            fail("pending entry control permits only module finalization");
        activate(record);
        auto saved = machine.CaptureContext();
        const auto previousState = record.state;
        const auto previousOwner = moduleOwner;
        const auto previousUntil = moduleUntil;
        const auto previousReturnStack = record.returnStack;
        record.returnStack = callFrame(call.Entry, call.ReturnGate, call.Arguments);
        machine.SaveContext(record.context);
        moduleOwner = record.id;
        moduleUntil = call.ReturnGate;
        record.state = State::Runnable;
        enqueue(record, true);
        const auto reason = drive(budget);
        if (reason != StopReason::Address) return {reason, std::nullopt};
        const auto result = machine.Get(Register::Rax);
        machine.RestoreContext(saved);
        machine.SaveContext(record.context);
        record.state = previousState;
        moduleOwner = previousOwner;
        moduleUntil = previousUntil;
        record.returnStack = previousReturnStack;
        return {reason, result};
    }
    void completeControl() {
        checkOwner();
        if (driving || terminal || !controlToken || control.Kind == GuestEntryControlKind::None)
            fail("entry control cannot complete in the current runtime state");
        auto& record = lookup(controlOwner);
        if (record.state != State::Control) fail("entry control lost its paused continuation");
        activate(record);
        if (const auto stopped = observeStop()) {
            cancel(*stopped);
            fail("entry control cannot complete after a terminal guest stop");
        }
        if (control.Kind == GuestEntryControlKind::ProcessExit) {
            if (!control.ExitCode) fail("process exit has no exit code");
            machine.Exit(*control.ExitCode);
            cancel(StopReason::Exit);
        } else {
            machine.CompleteHostCall(*controlToken);
            machine.SaveContext(record.context);
            ready(record, true);
        }
        controlToken.reset();
        controlOwner = 0;
        control = {};
    }
    void withdraw() {
        checkOwner();
        if (driving) fail("cannot withdraw a running guest scheduler");
        controlToken.reset();
        controlOwner = moduleOwner = 0;
        runnable.clear();
        for (auto& [id, record] : records) {
            cancelRecord(*record);
            retire(*record);
        }
        waitDomains.clear();
        attributes.clear();
        records.clear();
        if (gateMapped) machine.Unmap(ReturnGate, PageSize);
        gateMapped = false;
        active = 0;
        tlsFactory = {};
        withdrawn = true;
    }
};

struct GuestThreads::WaitDomain::State {
    std::weak_ptr<GuestThreads::Impl> scheduler;
    std::uint64_t id;

    std::shared_ptr<GuestThreads::Impl> lock() const {
        auto live = scheduler.lock();
        if (!live) fail("guest wait domain scheduler has expired");
        live->checkOwner();
        if (!live->waitDomains.contains(id)) fail("guest wait domain was withdrawn");
        return live;
    }
};

GuestThreads::WaitDomain::WaitDomain() = default;
GuestThreads::WaitDomain::WaitDomain(std::unique_ptr<State> value) : state(std::move(value)) {}
GuestThreads::WaitDomain::~WaitDomain() {
    try { Withdraw(); }
    catch (...) { std::terminate(); }
}
GuestThreads::WaitDomain::WaitDomain(WaitDomain&&) noexcept = default;
GuestThreads::WaitDomain& GuestThreads::WaitDomain::operator=(WaitDomain&& other) noexcept {
    if (this != &other) {
        try { Withdraw(); }
        catch (...) { std::terminate(); }
        state = std::move(other.state);
    }
    return *this;
}
GuestThreadHandle GuestThreads::WaitDomain::ActiveThread() const {
    if (!state) fail("guest wait domain is empty");
    return state->lock()->current().id;
}
void GuestThreads::WaitDomain::BlockFromHostCall(std::uint64_t key) {
    if (!state) fail("guest wait domain is empty");
    if (!key) fail("guest wait key is null");
    state->lock()->queue(Impl::Action::Wait, {state->id, key});
}
bool GuestThreads::WaitDomain::IsWaiting(GuestThreadHandle thread, std::uint64_t key) const {
    if (!state) fail("guest wait domain is empty");
    return state->lock()->isWaiting(state->id, thread, key);
}
bool GuestThreads::WaitDomain::Wake(GuestThreadHandle thread, std::uint64_t key, std::uint32_t result) {
    if (!state) fail("guest wait domain is empty");
    return state->lock()->wake(state->id, thread, key, result);
}
void GuestThreads::WaitDomain::SetInheritanceOwner(std::uint64_t key, GuestThreadHandle owner) {
    if (!state) fail("guest wait domain is empty");
    state->lock()->setInheritanceOwner(state->id, key, owner);
}
std::int32_t GuestThreads::WaitDomain::EffectivePriority(GuestThreadHandle thread) const {
    if (!state) fail("guest wait domain is empty");
    return state->lock()->lookup(thread).effectivePriority;
}
void GuestThreads::WaitDomain::Withdraw() {
    if (!state) return;
    if (auto live = state->scheduler.lock()) live->withdrawWaitDomain(state->id);
    state.reset();
}

GuestThreads::GuestThreads(Machine& machine) : impl(std::make_shared<Impl>(machine)) {}
GuestThreads::~GuestThreads() {
    if (!impl->withdrawn) {
        try { impl->withdraw(); }
        catch (...) { std::terminate(); }
    }
}
GuestThreadHandle GuestThreads::AdoptInitial(GuestInitialThread initial) {
    impl->checkOwner();
    if (impl->initial || impl->driving) fail("initial guest thread is already adopted or executing");
    if (initial.BasePriority < GuestThreadPriorityMin || initial.BasePriority > GuestThreadPriorityMax)
        fail("initial guest priority is outside the qualified FIFO range");
    impl->machine.CheckAccess(initial.Entry, 1, Permission::Execute);
    if (!initial.Stack.Size) fail("initial guest stack is empty");
    impl->machine.CheckAccess(initial.Stack.Address, initial.Stack.Size, Permission::Read | Permission::Write);
    const auto stack = impl->machine.Get(Register::Rsp);
    if (stack < initial.Stack.Address || stack - initial.Stack.Address >= initial.Stack.Size)
        fail("initial guest stack does not contain its current stack pointer");
    if (impl->machine.Get(Register::FsBase) != (initial.Tls ? initial.Tls->FsBase() : 0))
        fail("initial guest TLS does not match Machine FS");
    impl->checkFree(ReturnGate, PageSize);
    const auto base = impl->slot();
    auto record = std::make_unique<Impl::Record>();
    record->owned.reserve(1);
    record->id = HandleBase + impl->nextSlot;
    record->initial = true;
    record->basePriority = record->effectivePriority = initial.BasePriority;
    record->stack = initial.Stack;
    record->tls = std::move(initial.Tls);
    record->errnoAddress = base;
    auto original = impl->machine.CaptureContext();
    try {
        impl->machine.Set(Register::Rip, initial.Entry);
        record->context = impl->machine.CaptureContext();
        impl->machine.Map(ReturnGate, PageSize, Permission::Read | Permission::Execute);
        impl->gateMapped = true;
        impl->map(*record, base, PageSize, Permission::Read | Permission::Write);
        const auto id = record->id;
        impl->records.emplace(id, std::move(record));
        impl->initial = impl->active = id;
        impl->tlsFactory = std::move(initial.CreateThreadTls);
        return id;
    } catch (...) {
        impl->machine.RestoreContext(original);
        if (record) impl->retire(*record);
        if (impl->gateMapped) impl->machine.Unmap(ReturnGate, PageSize);
        impl->gateMapped = false;
        throw;
    }
}
GuestThreadHandle GuestThreads::ActiveThread() const { return impl->current().id; }
std::shared_ptr<SceTls> GuestThreads::ActiveTls() const { return impl->current().tls; }
std::uint64_t GuestThreads::ActiveErrnoAddress() const { return impl->current().errnoAddress; }
std::int32_t GuestThreads::Equal(GuestThreadHandle left, GuestThreadHandle right) const {
    impl->checkOwner();
    impl->lookup(left);
    impl->lookup(right);
    return left == right;
}
std::int32_t GuestThreads::BasePriority(GuestThreadHandle thread) const {
    impl->checkOwner();
    return impl->lookup(thread).basePriority;
}
std::int32_t GuestThreads::EffectivePriority(GuestThreadHandle thread) const {
    impl->checkOwner();
    return impl->lookup(thread).effectivePriority;
}
std::uint32_t GuestThreads::AttributeInit(std::uint64_t slot8) {
    impl->checkOwner();
    if (!slot8) return Invalid;
    if (slot8 > std::numeric_limits<std::uint64_t>::max() - 8)
        fail("guest thread attribute slot overflows");
    impl->machine.CheckAccess(slot8, 8, Permission::Write);
    if (impl->attributes.contains(slot8))
        fail("reinitialization of a live owned guest thread attribute is unsupported");
    const auto token = attributeToken();
    const auto position = impl->attributes.emplace(slot8, Impl::Attribute{token}).first;
    try { write64(impl->machine, slot8, token); }
    catch (...) { impl->attributes.erase(position); throw; }
    return 0;
}
std::uint32_t GuestThreads::AttributeDestroy(std::uint64_t slot8) {
    impl->checkOwner();
    if (!impl->attribute(slot8)) return Invalid;
    write64(impl->machine, slot8, 0);
    impl->attributes.erase(slot8);
    return 0;
}
std::uint32_t GuestThreads::AttributeSetPriority(std::uint64_t slot8, std::uint64_t parameter4) {
    impl->checkOwner();
    auto attr = impl->attribute(slot8);
    if (!attr) return Invalid;
    if (!parameter4) return NotSupported;
    impl->checkParameter(parameter4, Permission::Read);
    std::int32_t priority;
    impl->machine.Read(parameter4, std::as_writable_bytes(std::span(&priority, 1)));
    if (priority < GuestThreadPriorityMin || priority > GuestThreadPriorityMax) return NotSupported;
    attr->priority = priority;
    return 0;
}
std::uint32_t GuestThreads::AttributeGetPriority(std::uint64_t slot8, std::uint64_t output4) {
    impl->checkOwner();
    const auto attr = impl->attribute(slot8);
    if (!attr || !output4) return Invalid;
    impl->checkParameter(output4, Permission::Write);
    impl->machine.Write(output4, std::as_bytes(std::span(&attr->priority, 1)));
    return 0;
}
std::uint32_t GuestThreads::AttributeSetInherit(std::uint64_t slot8, std::int32_t inherit) {
    impl->checkOwner();
    auto attr = impl->attribute(slot8);
    if (!attr) return Invalid;
    if (inherit != 0 && inherit != 4) return NotSupported;
    attr->inherit = inherit;
    return 0;
}
std::uint32_t GuestThreads::AttributeSetPolicy(std::uint64_t slot8, std::int32_t policy) {
    impl->checkOwner();
    auto attr = impl->attribute(slot8);
    if (!attr) return Invalid;
    if (policy < 1 || policy > 3) return NotSupported;
    if (policy != 1) fail("guest thread scheduling policies other than FIFO are unsupported");
    attr->priority = GuestThreadPriorityDefault;
    return 0;
}
GuestThreads::WaitDomain GuestThreads::CreateWaitDomain(Machine& associatedMachine,
                                                       std::function<void(GuestThreadHandle)> callback) {
    impl->checkOwner();
    if (&associatedMachine != &impl->machine) fail("guest wait domain belongs to another Machine");
    if (impl->nextWaitDomain == std::numeric_limits<std::uint64_t>::max())
        fail("guest wait domain identity is exhausted");
    const auto id = impl->nextWaitDomain++;
    auto state = std::make_unique<WaitDomain::State>();
    state->scheduler = impl;
    state->id = id;
    impl->waitDomains.emplace(id, Impl::Domain{std::move(callback), {}});
    return WaitDomain(std::move(state));
}
void GuestThreads::RegisterThreadDtors(std::uint64_t pc) { impl->registerCallback(impl->dtors, pc); }
void GuestThreads::RegisterThreadAtexitCount(std::uint64_t pc) { impl->registerCallback(impl->count, pc); }
void GuestThreads::RegisterThreadAtexitReport(std::uint64_t pc) { impl->registerCallback(impl->report, pc); }
void GuestThreads::CreateFromHostCall(std::uint64_t output, std::uint64_t attr, std::uint64_t entry,
                                    std::uint64_t argument, std::uint64_t name) {
    impl->queue(Impl::Action::Create, {output, attr, entry, argument, name});
}
void GuestThreads::YieldFromHostCall() { impl->queue(Impl::Action::Yield); }
void GuestThreads::JoinFromHostCall(GuestThreadHandle target, std::uint64_t output) {
    impl->queue(Impl::Action::Join, {target, output});
}
void GuestThreads::ExitThreadFromHostCall(std::uint64_t value) { impl->queue(Impl::Action::ExitThread, {value}); }
void GuestThreads::EntryTerminationFromHostCall() { impl->queue(Impl::Action::TerminateEntry); }
void GuestThreads::ProcessExitFromHostCall(int value) {
    impl->queue(Impl::Action::ExitProcess, {static_cast<std::uint32_t>(value)});
}
GuestEntryControl GuestThreads::PendingEntryControl() const { impl->checkOwner(); return impl->control; }
void GuestThreads::CompleteEntryControl() { impl->completeControl(); }
StopReason GuestThreads::RunEntry(GuestPhaseBudget& budget) { return impl->runEntry(budget); }
GuestCallResult GuestThreads::InvokeModule(const GuestModuleCall& call, GuestPhaseBudget& budget) {
    return impl->invokeModule(call, budget);
}
SceModuleExecutor GuestThreads::ModuleExecutor() {
    impl->checkOwner();
    if (!impl->initial) fail("module executor requires an adopted initial guest");
    const std::weak_ptr<Impl> weak = impl;
    const auto lock = [weak] {
        const auto live = weak.lock();
        if (!live) fail("guest module executor runtime has expired");
        live->checkOwner();
        return live;
    };
    return {&impl->machine, impl->initial,
        [lock](const GuestModuleCall& call, GuestPhaseBudget& budget) { return lock()->invokeModule(call, budget); },
        [lock](GuestPhaseBudget& budget) { return lock()->runEntry(budget); },
        [lock] { lock()->queue(Impl::Action::TerminateEntry); },
        [lock] { return lock()->control; },
        [lock] { lock()->completeControl(); }};
}
void GuestThreads::Withdraw() { impl->withdraw(); }

}
