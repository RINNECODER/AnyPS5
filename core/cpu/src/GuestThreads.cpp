#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <array>
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
    enum class State { Runnable, Running, BlockedJoining, Finishing, Finished, Reaped, Control, Cancelled };
    enum class Action { Create, Yield, Join, ExitThread, TerminateEntry, ExitProcess };
    enum class Phase { None, Entry, Module };
    struct Pending {
        Action kind;
        std::array<std::uint64_t, 5> args{};
        Machine::SuspendedCall token;
    };
    struct Record {
        GuestThreadHandle id;
        bool initial = false;
        bool finalizing = false;
        State state = State::Runnable;
        Machine::Context context;
        std::shared_ptr<SceTls> tls;
        Mapping stack{};
        std::uint64_t errnoAddress = 0;
        std::uint64_t result = 0;
        std::uint64_t returnStack = 0;
        std::optional<GuestThreadHandle> joiner;
        std::unique_ptr<Pending> pending;
        std::vector<Mapping> owned;
    };

    Machine& machine;
    const std::thread::id owner = std::this_thread::get_id();
    std::map<GuestThreadHandle, std::unique_ptr<Record>> records;
    std::deque<GuestThreadHandle> runnable;
    GuestThreadHandle initial = 0;
    GuestThreadHandle active = 0;
    std::uint64_t nextSlot = 0;
    std::function<std::shared_ptr<SceTls>(std::uint64_t)> tlsFactory;
    std::uint64_t dtors = 0, count = 0, report = 0;
    bool withdrawn = false;
    bool driving = false;
    bool gateMapped = false;
    Phase phase = Phase::None;
    std::optional<StopReason> terminal;
    GuestEntryControl control;
    std::optional<Machine::SuspendedCall> controlToken;
    std::uint64_t initialUntil = 0;

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
        auto pending = std::make_unique<Pending>();
        pending->kind = action;
        pending->args = args;
        pending->token = machine.PauseHostCall();
        record.pending = std::move(pending);
    }
    void complete(Record& record, bool status) {
        if (!record.pending) fail("guest host call has no pending operation");
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
        if (args[1]) fail("nondefault guest thread attributes are unsupported");
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
        case Action::ExitThread: beginFinish(record, record.pending->args[0]); break;
        case Action::TerminateEntry:
        case Action::ExitProcess:
            if (!record.initial || phase != Phase::Entry || control.Kind != GuestEntryControlKind::None)
                fail("process entry control is unsupported outside initial entry execution");
            control.Kind = record.pending->kind == Action::TerminateEntry
                ? GuestEntryControlKind::TerminationCallback : GuestEntryControlKind::ProcessExit;
            if (control.Kind == GuestEntryControlKind::ProcessExit)
                control.ExitCode = static_cast<int>(static_cast<std::int32_t>(record.pending->args[0]));
            controlToken.emplace(std::move(record.pending->token));
            record.pending.reset();
            record.state = State::Control;
            break;
        }
    }
    StopReason cancel(StopReason reason) {
        terminal = reason;
        runnable.clear();
        for (auto& [id, record] : records)
            if (record->state != State::Finished && record->state != State::Reaped) record->state = State::Cancelled;
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
            if (runnable.empty()) fail("guest join graph has no runnable work and cannot make progress");
            auto& record = lookup(runnable.front());
            runnable.pop_front();
            if (record.state != State::Runnable && record.state != State::Finishing)
                fail("guest run queue contains an ineligible thread");
            activate(record);
            record.state = State::Running;
            const auto until = record.initial ? initialUntil : ReturnGate;
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
                ready(record, true);
                if (!budget.Remaining()) return reason;
            } else if (reason == StopReason::Paused) {
                process(record);
                if (control.Kind != GuestEntryControlKind::None && phase == Phase::Entry) return StopReason::Paused;
            } else if (reason == StopReason::Address) {
                if (machine.Get(Register::Rsp) != record.returnStack)
                    fail("guest callback reached its return sentinel without unwinding its call frame");
                if (record.initial) {
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
        initialUntil = 0;
        if (record.state == State::Runnable) enqueue(record, true);
        return drive(budget);
    }
    GuestCallResult invokeModule(const GuestModuleCall& call, GuestPhaseBudget& budget) {
        Execution execution(*this, Phase::Module);
        if (terminal) return {*terminal, std::nullopt};
        auto& record = lookup(initial);
        if (record.state != State::Runnable && record.state != State::Control)
            fail("module callback requires an idle initial continuation");
        if (control.Kind != GuestEntryControlKind::None && call.Kind != GuestModuleCallKind::Finalize)
            fail("pending entry control permits only module finalization");
        activate(record);
        auto saved = machine.CaptureContext();
        const auto previousState = record.state;
        const auto previousUntil = initialUntil;
        const auto previousReturnStack = record.returnStack;
        record.returnStack = callFrame(call.Entry, call.ReturnGate, call.Arguments);
        machine.SaveContext(record.context);
        initialUntil = call.ReturnGate;
        record.state = State::Runnable;
        enqueue(record, true);
        const auto reason = drive(budget);
        if (reason != StopReason::Address) return {reason, std::nullopt};
        const auto result = machine.Get(Register::Rax);
        machine.RestoreContext(saved);
        machine.SaveContext(record.context);
        record.state = previousState;
        initialUntil = previousUntil;
        record.returnStack = previousReturnStack;
        return {reason, result};
    }
    void completeControl() {
        checkOwner();
        if (driving || terminal || !controlToken || control.Kind == GuestEntryControlKind::None)
            fail("entry control cannot complete in the current runtime state");
        auto& record = lookup(initial);
        if (record.state != State::Control) fail("entry control lost its initial continuation");
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
        control = {};
    }
    void withdraw() {
        checkOwner();
        if (driving) fail("cannot withdraw a running guest scheduler");
        controlToken.reset();
        runnable.clear();
        for (auto& [id, record] : records) {
            retire(*record);
            if (record->state != State::Finished && record->state != State::Reaped) record->state = State::Cancelled;
        }
        records.clear();
        if (gateMapped) machine.Unmap(ReturnGate, PageSize);
        gateMapped = false;
        active = 0;
        tlsFactory = {};
        withdrawn = true;
    }
};

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
