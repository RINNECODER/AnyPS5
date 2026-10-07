#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t Code = 0x1000, DataA = 0x4000, DataB = 0x4100, Extra = 0x6000;

void Require(bool condition, const char* diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}

template<class Function> void Rejects(Function&& function, const char* diagnostic) {
    try { function(); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing context rejection: ") + diagnostic);
}

template<class T> void Write(Machine& machine, std::uint64_t address, const T& value) {
    machine.Write(address, std::as_bytes(std::span(&value, 1)));
}

std::uint64_t Read(Machine& machine, std::uint64_t address) {
    std::uint64_t value = 0;
    machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}

constexpr std::array Observed{Register::Rax, Register::Rbx, Register::Rdi, Register::Rsi,
    Register::Rsp, Register::Rip, Register::Rflags};
using State = std::array<std::uint64_t, Observed.size()>;

State Current(Machine& machine) {
    State result{};
    for (std::size_t i = 0; i < Observed.size(); ++i) result[i] = machine.Get(Observed[i]);
    return result;
}

template<class Function> void RejectsUnchanged(Machine& machine, Function&& function, const char* diagnostic) {
    const auto previous = Current(machine);
    Rejects(std::forward<Function>(function), diagnostic);
    Require(Current(machine) == previous, "Rejected context operation changed live guest state");
}

void Initialize(Machine& machine, std::uint64_t value, std::uint64_t counter, std::uint64_t data, std::uint64_t stack) {
    machine.Set(Register::Rax, value);
    machine.Set(Register::Rbx, counter);
    machine.Set(Register::Rdi, data);
    machine.Set(Register::Rsi, Extra);
    machine.Set(Register::Rsp, stack);
    machine.Set(Register::Rip, Code);
    machine.Set(Register::Rflags, 2);
}

void Resume(Machine& machine, std::uint64_t entry, std::uint64_t until,
            std::uint64_t value, std::uint64_t counter, std::uint64_t output) {
    Require(machine.Get(Register::Rip) == entry, "Restored context lost the actual guest continuation PC");
    Require(machine.Run(machine.Get(Register::Rip), until, 100) == Cpu::StopReason::Address,
        "Restored context did not execute its real x86 continuation");
    Require(machine.Get(Register::Rip) == until && machine.Get(Register::Rax) == value &&
        machine.Get(Register::Rbx) == counter && Read(machine, output) == value,
        "Resumed guest register, PC or shared-memory output differs from independent arithmetic");
}

void ProgressingContinuations() {
    Machine machine;
    machine.Map(Code, 4096, rx);
    machine.Map(DataA, 4096, rw);
    machine.Map(0x8000, 4096, rw);
    constexpr std::array<std::uint8_t, 32> progress{
        0x48,0x83,0xc0,0x03, 0x48,0x89,0x07, 0x48,0xff,0xc3,
        0x48,0x83,0xc0,0x05, 0x48,0x89,0x47,0x08, 0x48,0xff,0xc3,
        0x48,0x83,0xc0,0x07, 0x48,0x89,0x47,0x10, 0x48,0xff,0xc3};
    machine.Write(Code, std::as_bytes(std::span(progress)));
    constexpr std::array<std::uint8_t, 20> caller{
        0x48,0xb8,0,0x11,0,0,0,0,0,0, 0xff,0xd0,
        0x48,0x83,0xc0,0x09, 0x48,0x89,0x47,0x18};
    machine.Write(0x1080, std::as_bytes(std::span(caller)));
    constexpr std::array<std::uint8_t, 3> store{0x48,0x89,0x06};
    machine.Write(0x10c0, std::as_bytes(std::span(store)));

    Initialize(machine, 10, 100, DataA, 0x8ff0);
    auto contextA = machine.CaptureContext();
    Initialize(machine, 50, 200, DataB, 0x8fd0);
    auto contextB = machine.CaptureContext();
    Machine::Context empty;
    RejectsUnchanged(machine, [&] { machine.SaveContext(empty); }, "empty");
    RejectsUnchanged(machine, [&] { machine.RestoreContext(empty); }, "empty");
    Machine::Context movedA;
    movedA = std::move(contextA);
    auto movedB = std::move(contextB);
    RejectsUnchanged(machine, [&] { machine.SaveContext(contextA); }, "empty");
    RejectsUnchanged(machine, [&] { machine.RestoreContext(contextA); }, "empty");
    RejectsUnchanged(machine, [&] { machine.SaveContext(contextB); }, "empty");
    RejectsUnchanged(machine, [&] { machine.RestoreContext(contextB); }, "empty");

    machine.RestoreContext(movedA);
    Resume(machine, Code, Code + 10, 13, 101, DataA);
    machine.SaveContext(movedA);
    machine.RestoreContext(movedB);
    Require(Read(machine, DataA) == 13, "Context restore rolled back another guest continuation's shared RAM");
    Resume(machine, Code, Code + 10, 53, 201, DataB);
    machine.SaveContext(movedB);

    machine.Map(Extra, 4096, rw);
    Write(machine, Extra, std::uint64_t{0x1122334455667788});
    machine.Protect(Extra, 4096, Permission::Read);
    machine.RestoreContext(movedA);
    machine.CheckAccess(Extra, 8, Permission::Read);
    Rejects([&] { machine.CheckAccess(Extra, 8, Permission::Write); }, "Guest access denied");
    Require(Read(machine, Extra) == 0x1122334455667788 && Read(machine, DataB) == 53,
        "Context restore reverted later mappings, permissions or another continuation's bytes");
    Resume(machine, Code + 10, Code + 21, 18, 102, DataA + 8);
    machine.SaveContext(movedA);
    machine.RestoreContext(movedB);
    Resume(machine, Code + 10, Code + 21, 58, 202, DataB + 8);
    machine.SaveContext(movedB);

    const auto beforeThread = Current(machine);
    Machine::Context unexpected;
    std::exception_ptr foreignFailure;
    std::thread foreign([&] {
        try {
            Rejects([&] { unexpected = machine.CaptureContext(); }, "owner thread");
            Rejects([&] { machine.SaveContext(movedA); }, "owner thread");
            Rejects([&] { machine.RestoreContext(movedA); }, "owner thread");
#if ANYPS5_CPU_MODERN_TCG
            Rejects([&] { (void)machine.PinOwnedMappings(); }, "owner thread");
#endif
        } catch (...) { foreignFailure = std::current_exception(); }
    });
    foreign.join();
    if (foreignFailure) std::rethrow_exception(foreignFailure);
    Require(Current(machine) == beforeThread, "Nonowner context operation changed live CPU state");

    unsigned calls = 0;
    machine.AddHostCall(0x1100, [&](Machine& running) {
        RejectsUnchanged(running, [&] { unexpected = running.CaptureContext(); }, "idle");
        RejectsUnchanged(running, [&] { running.SaveContext(movedA); }, "idle");
        RejectsUnchanged(running, [&] { running.RestoreContext(movedB); }, "idle");
#if ANYPS5_CPU_MODERN_TCG
        RejectsUnchanged(running, [&] { (void)running.PinOwnedMappings(); }, "idle");
#endif
        running.Set(Register::Rax, 7);
        ++calls;
    });
    machine.RestoreContext(movedA);
    Require(machine.Run(0x1080, 0x1094, 100) == Cpu::StopReason::Address && calls == 1 &&
        machine.Get(Register::Rax) == 16 && Read(machine, DataA + 24) == 16 &&
        machine.Get(Register::Rsp) == 0x8ff0,
        "Context rejection inside an actual CALL gate damaged its guest return or continuation");
    machine.RestoreContext(movedA);
    Require(machine.Get(Register::Rax) == 18 && machine.Get(Register::Rbx) == 102,
        "Rejected running/nonowner SaveContext replaced the saved A continuation");
    Rejects([&] { machine.Run(0x10c0, 0x10c3, 10); }, "protected write at 0x6000");
    Require(Read(machine, Extra) == 0x1122334455667788, "Restored context weakened a later protection boundary");
    machine.RestoreContext(movedA);
    Resume(machine, Code + 21, Code + 32, 25, 103, DataA + 16);
    machine.RestoreContext(movedB);
    Resume(machine, Code + 21, Code + 32, 65, 203, DataB + 16);
    Require(Read(machine, DataA) == 13 && Read(machine, DataA + 8) == 18 && Read(machine, DataA + 16) == 25 &&
        Read(machine, DataA + 24) == 16 && Read(machine, DataB) == 53 && Read(machine, DataB + 8) == 58 &&
        Read(machine, DataB + 16) == 65,
        "A/B continuation restore copied RAM or lost independently expected shared results");
}

void ExpiredAcrossMachines() {
    Machine::Context expired;
    {
        Machine previous;
        Initialize(previous, 37, 77, DataA, 0x8ff0);
        expired = previous.CaptureContext();
    }
    Machine replacement;
    Initialize(replacement, 91, 301, DataB, 0x8fd0);
    RejectsUnchanged(replacement, [&] { replacement.SaveContext(expired); }, "expired");
    RejectsUnchanged(replacement, [&] { replacement.RestoreContext(expired); }, "expired");
    const auto beforeDestruction = Current(replacement);
    expired = Machine::Context{};
    Require(Current(replacement) == beforeDestruction, "Late expired context destruction touched a replacement CPU");
    auto live = replacement.CaptureContext();
    replacement.Set(Register::Rax, 17);
    replacement.RestoreContext(live);
    Require(replacement.Get(Register::Rax) == 91, "Expired context retirement damaged the new Machine's own live context");
}

#if ANYPS5_CPU_MODERN_TCG
const Cpu::OwnedMappingView& OwnedView(const Cpu::OwnedMappingSnapshot& snapshot, std::uint64_t address) {
    for (const auto& view : snapshot.Views)
        if (view.Region.Address == address) return view;
    throw std::runtime_error("Owned backing snapshot omitted an expected mapped span");
}

bool OwnsAddress(const Cpu::OwnedMappingSnapshot& snapshot, std::uint64_t address) {
    for (const auto& view : snapshot.Views)
        if (address >= view.Region.Address && address - view.Region.Address < view.Region.Size) return true;
    return false;
}

std::uint64_t OwnedWord(std::span<std::byte> bytes, std::size_t offset) {
    Require(offset <= bytes.size() && sizeof(std::uint64_t) <= bytes.size() - offset,
        "Owned backing word escaped its mapped span");
    std::uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void OwnedBackingContinuations() {
    constexpr std::uint64_t data = 0x200004000, alias = 0x300008000, externalAddress = 0x400010000;
    alignas(16384) std::array<std::byte, 16384> external{};
    external.fill(std::byte{0xcc});
    Cpu::OwnedMappingSnapshot retained;
    std::weak_ptr<void> allocationLifetime;
    std::weak_ptr<const void> scopeLifetime;
    {
        Machine machine;
        const auto empty = machine.PinOwnedMappings();
        Require(empty.Scope && empty.Views.empty(), "Idle empty Machine snapshot lacked a scope or invented owned memory");
        machine.Map(Code, 4096, rx);
        machine.Map(data, 12288, rw);
        const auto original = machine.PinOwnedMappings();
        const auto& originalView = OwnedView(original, data);
        Require(original.Scope == empty.Scope && original.Generation > empty.Generation &&
            originalView.Region.Size == 12288 && originalView.Region.Permissions == rw &&
            !originalView.Region.Borrowed && originalView.Bytes.size() == 12288 && originalView.Owner &&
            originalView.BackingIdentity != 0 && originalView.Allocation.data() == originalView.Bytes.data() &&
            originalView.Allocation.size() >= 12288 &&
            OwnedView(original, Code).BackingIdentity != originalView.BackingIdentity,
            "Owned mapping snapshot lost current bounds, permissions, allocation owner or distinct backing identity");
        std::array<std::byte, 12288> guards;
        guards.fill(std::byte{0xa5});
        machine.Write(data, guards);
        Write(machine, data, std::uint64_t{17});
        constexpr std::array<std::uint8_t, 11> arithmetic{
            0x48,0x8b,0x07, 0x48,0x83,0xc0,0x04, 0x48,0x89,0x47,0x08};
        machine.Write(Code, std::as_bytes(std::span(arithmetic)));
        machine.Set(Register::Rdi, data);
        Require(machine.Run(Code, Code + arithmetic.size(), 20) == Cpu::StopReason::Address &&
            machine.Get(Register::Rax) == 21 && Read(machine, data + 8) == 21 &&
            OwnedWord(originalView.Bytes, 0) == 17 && OwnedWord(originalView.Bytes, 8) == 21,
            "Pinned loader backing copied bytes instead of observing an actual translated x86 write");
        const std::uint64_t nativeInput = 98;
        std::memcpy(originalView.Bytes.data(), &nativeInput, sizeof(nativeInput));
        Require(machine.Run(Code, Code + arithmetic.size(), 20) == Cpu::StopReason::Address &&
            machine.Get(Register::Rax) == 102 && Read(machine, data + 8) == 102 &&
            OwnedWord(originalView.Bytes, 8) == 102,
            "Actual translated guest did not observe a native write through its pinned allocation");
        for (std::size_t i = 16; i < originalView.Bytes.size(); ++i)
            Require(originalView.Bytes[i] == std::byte{0xa5}, "Pinned guest/native coherence damaged an allocation guard");
        Require(machine.PinOwnedMappings().Generation == original.Generation,
            "Byte writes or translated execution changed structural mapping generation");

        machine.MapBorrowed(alias, originalView.Bytes.subspan(4096, 4096), rw, originalView.Allocation);
        const auto aliased = machine.PinOwnedMappings();
        const auto& aliasView = OwnedView(aliased, alias);
        Require(aliased.Generation > original.Generation && aliasView.Region.Size == 4096 &&
            aliasView.Region.Permissions == rw && aliasView.Region.Borrowed && aliasView.Bytes.size() == 4096 &&
            aliasView.Bytes.data() == originalView.Bytes.data() + 4096 &&
            aliasView.Allocation.data() == originalView.Allocation.data() &&
            aliasView.Allocation.size() == originalView.Allocation.size() &&
            aliasView.BackingIdentity == originalView.BackingIdentity && aliasView.Owner &&
            !aliasView.Owner.owner_before(originalView.Owner) && !originalView.Owner.owner_before(aliasView.Owner),
            "Pinned borrowed alias lost actual allocation offset, identity or shared owner");
        Write(machine, data + 4096, std::uint64_t{38});
        machine.Set(Register::Rdi, alias);
        Require(machine.Run(Code, Code + arithmetic.size(), 20) == Cpu::StopReason::Address &&
            Read(machine, data + 4104) == 42 && OwnedWord(aliasView.Bytes, 8) == 42 &&
            OwnedWord(originalView.Bytes, 4104) == 42,
            "Translated borrowed alias did not update its original pinned allocation");
        for (std::size_t i = 16; i < aliasView.Bytes.size(); ++i)
            Require(aliasView.Bytes[i] == std::byte{0xa5}, "Borrowed alias write escaped its independently guarded output");

        machine.MapBorrowed(externalAddress, external, rw);
        const auto withExternal = machine.PinOwnedMappings();
        Require(withExternal.Generation > aliased.Generation && !OwnsAddress(withExternal, externalAddress) &&
            OwnedView(withExternal, alias).BackingIdentity == originalView.BackingIdentity,
            "External borrowed allocation was fabricated as Machine-owned storage");
        Rejects([&] { machine.Map(data, 4096, rw); }, "overlap");
        Rejects([&] { machine.Protect(data + 12288, 4096, Permission::Read); }, "Guest access denied");
        Require(machine.PinOwnedMappings().Generation == withExternal.Generation,
            "Rejected mapping transaction changed published mapping generation");

        machine.Protect(data, 4096, Permission::Read);
        const auto protectedPage = machine.PinOwnedMappings();
        Require(protectedPage.Generation > withExternal.Generation &&
            OwnedView(protectedPage, data).Region.Size == 4096 &&
            OwnedView(protectedPage, data).Region.Permissions == Permission::Read &&
            OwnedView(protectedPage, data).BackingIdentity == originalView.BackingIdentity &&
            OwnedView(protectedPage, alias).Region.Permissions == rw &&
            originalView.Region.Size == 12288 && originalView.Region.Permissions == rw,
            "Protection changed backing identity, peer permissions or a captured snapshot's metadata");
        Rejects([&] { machine.CheckAccess(data, 1, Permission::Write); }, "Guest access denied");
        machine.CheckAccess(alias, 4096, Permission::Write);
        machine.ProtectFragment(data + 4112, 8, Permission::Read);
        const auto fragment = machine.PinOwnedMappings();
        const auto& fragmentView = OwnedView(fragment, data + 4112);
        Require(fragment.Generation > protectedPage.Generation && fragmentView.Region.Size == 8 &&
            fragmentView.Region.Permissions == Permission::Read && fragmentView.Bytes.size() == 8 &&
            fragmentView.Bytes.data() == originalView.Bytes.data() + 4112 &&
            fragmentView.BackingIdentity == originalView.BackingIdentity &&
            fragmentView.Allocation.size() == originalView.Allocation.size() &&
            OwnedView(fragment, alias).Region.Permissions == rw,
            "Fragment protection lost exact pinned interval, actual offset or original allocation identity");
        Rejects([&] { machine.CheckAccess(data + 4112, 8, Permission::Write); }, "Guest access denied");
        machine.CheckAccess(data + 4104, 8, Permission::Write);
        machine.CheckAccess(data + 4120, 8, Permission::Write);
        machine.Unmap(data + 4096, 4096);
        const auto split = machine.PinOwnedMappings();
        Require(split.Generation > fragment.Generation && !OwnsAddress(split, data + 4096) &&
            OwnedView(split, data).Region.Size == 4096 && OwnedView(split, data + 8192).Region.Size == 4096 &&
            OwnedView(split, data + 8192).Bytes.data() == originalView.Bytes.data() + 8192 &&
            OwnedView(split, data + 8192).BackingIdentity == originalView.BackingIdentity &&
            Read(machine, alias + 8) == 42 && OwnedWord(originalView.Bytes, 4104) == 42,
            "Partial unmap retained a guest binding or lost split backing/alias bytes");
        machine.Unmap(data, 4096);
        machine.Unmap(data + 8192, 4096);
        machine.Unmap(alias, 4096);
        const auto retired = machine.PinOwnedMappings();
        Require(retired.Generation > split.Generation && !OwnsAddress(retired, data) && !OwnsAddress(retired, alias) &&
            OwnedWord(originalView.Bytes, 8) == 102 && OwnedWord(originalView.Bytes, 4104) == 42,
            "Last guest unmap freed pinned bytes or kept their guest address bindings");
        Rejects([&] { machine.CheckAccess(data, 1, Permission::Read); }, "Guest access denied");
        machine.Map(data, 12288, rw);
        const auto remapped = machine.PinOwnedMappings();
        const auto& remappedView = OwnedView(remapped, data);
        Write(machine, data + 8, std::uint64_t{731});
        Require(remapped.Generation > retired.Generation &&
            remappedView.BackingIdentity != originalView.BackingIdentity &&
            remappedView.Bytes.data() != originalView.Bytes.data() && OwnedWord(remappedView.Bytes, 8) == 731 &&
            OwnedWord(originalView.Bytes, 8) == 102,
            "New allocation reused a retired backing identity or overwrote retained original bytes");
        machine.ReplaceBorrowed(data, std::span(external).first(12288), rw);
        const auto replaced = machine.PinOwnedMappings();
        Require(replaced.Generation > remapped.Generation && !OwnsAddress(replaced, data) &&
            Read(machine, data) == 0xccccccccccccccccULL && OwnedWord(remappedView.Bytes, 8) == 731 &&
            OwnedWord(originalView.Bytes, 8) == 102,
            "Replacement published an external owner or invalidated a retained previous allocation");
        retained = original;
        allocationLifetime = originalView.Owner;
        scopeLifetime = original.Scope;
    }
    Require(!allocationLifetime.expired() && !scopeLifetime.expired() &&
        OwnedWord(OwnedView(retained, data).Bytes, 8) == 102 &&
        OwnedWord(OwnedView(retained, data).Bytes, 4104) == 42 &&
        OwnedView(retained, data).Bytes[8192] == std::byte{0xa5},
        "Machine destruction invalidated a retained owner or its independently computed byte oracle");
    {
        Machine replacement;
        const auto newScope = replacement.PinOwnedMappings();
        Require(newScope.Scope && newScope.Scope != retained.Scope && newScope.Views.empty(),
            "New Machine inherited a retired scope or retained guest address publication");
    }
    retained = {};
    Require(allocationLifetime.expired() && scopeLifetime.expired(),
        "Last snapshot retirement leaked an allocation or scope after Machine destruction");
}
#endif

constexpr std::uint64_t CallCode = 0x100001000, CallEntry = 0x100001200, CallGate = 0x100003000;
constexpr std::uint64_t CallReturn = 0x10000120c, CallEnd = 0x100001216;
constexpr std::uint64_t CallStack = 0x100008000, CallerStack = 0x100008ff0, ReturnSlot = CallerStack - 8;
constexpr std::uint64_t OtherEntry = 0x100001400, OtherAdd = 0x100001404, OtherEnd = 0x10000140b;
constexpr std::uint64_t OtherStack = 0x100009ff0;

void SuspendedFixture(Machine& machine) {
    machine.Map(CallCode, 4096, rx);
    machine.Map(CallGate, 4096, rx);
    machine.Map(DataA, 4096, rw);
    machine.Map(CallStack, 8192, rw);
    std::array<std::uint8_t, 4096> guard;
    guard.fill(0xa5);
    machine.Write(CallStack, std::as_bytes(std::span(guard)));
    constexpr std::array<std::uint8_t, 22> caller{
        0x48,0xb8,0,0x30,0,0,1,0,0,0, 0xff,0xd0,
        0x48,0x83,0xc0,0x09, 0x48,0x89,0x07, 0x48,0xff,0xc3};
    machine.Write(CallEntry, std::as_bytes(std::span(caller)));
    constexpr std::array<std::uint8_t, 11> other{
        0x48,0x83,0xc0,0x03, 0x48,0x89,0x07, 0x48,0xff,0xc3, 0x90};
    machine.Write(OtherEntry, std::as_bytes(std::span(other)));
    Initialize(machine, 19, 61, DataA, CallerStack);
    machine.Set(Register::Rip, CallEntry);
}

void CallerGuards(Machine& machine) {
    std::array<std::uint8_t, 4096> actual{};
    machine.Read(CallStack, std::as_writable_bytes(std::span(actual)));
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const auto position = CallStack + i;
        const auto expected = position >= ReturnSlot && position < CallerStack
            ? static_cast<std::uint8_t>(CallReturn >> ((position - ReturnSlot) * 8)) : 0xa5;
        Require(actual[i] == expected, "Suspended CALL or completion corrupted its independently guarded stack");
    }
}

void SuspendedContinuations() {
    Machine machine;
    SuspendedFixture(machine);
    Initialize(machine, 50, 71, DataB, OtherStack);
    machine.Set(Register::Rip, OtherEntry);
    auto other = machine.CaptureContext();
    Initialize(machine, 19, 61, DataA, CallerStack);
    machine.Set(Register::Rip, CallEntry);
    Machine::SuspendedCall paused, unexpected, empty;
    unsigned calls = 0;
    RejectsUnchanged(machine, [&] { unexpected = machine.PauseHostCall(); }, "active host callback");
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(empty); }, "empty");
    RejectsUnchanged(machine, [&] { machine.RunSlice(CallEntry + 1, CallEnd, 100); }, "current continuation PC");
    machine.AddHostCall(CallGate, [&](Machine& running) {
        ++calls;
        Require(running.Get(Register::Rip) == CallGate && running.Get(Register::Rsp) == ReturnSlot &&
            Read(running, ReturnSlot) == CallReturn, "Actual x86 CALL did not establish the independently expected frame");
        running.Set(Register::Rax, 7);
        paused = running.PauseHostCall();
        RejectsUnchanged(running, [&] { unexpected = running.PauseHostCall(); }, "already paused");
        RejectsUnchanged(running, [&] { running.CompleteHostCall(paused); }, "idle");
        RejectsUnchanged(running, [&] { running.RunSlice(CallGate, CallEnd, 100); }, "already running");
    });
    Require(machine.RunSlice(CallEntry, CallEnd, 100) == Cpu::StopReason::Paused && calls == 1 &&
        machine.Get(Register::Rip) == CallGate && machine.Get(Register::Rsp) == ReturnSlot &&
        machine.Get(Register::Rax) == 7 && Read(machine, DataA) == 0 && machine.LastRunInstructions() == 3,
        "Pause performed an automatic RET, guest continuation or repeated gate accounting");
    CallerGuards(machine);
    auto originating = machine.CaptureContext();
    auto moved = std::move(paused);
    Machine::SuspendedCall call;
    call = std::move(moved);
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(paused); }, "empty");
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(moved); }, "empty");
    RejectsUnchanged(machine, [&] { machine.Run(CallGate, CallEnd, 100); }, "completion of all suspended host calls");
    RejectsUnchanged(machine, [&] { machine.RunSlice(CallGate, CallEnd, 100); }, "completion before redispatch");
    Require(calls == 1, "Pending host call was redispatched before completion");

    machine.RestoreContext(other);
    Require(machine.RunSlice(OtherEntry, OtherEnd, 1) == Cpu::StopReason::InstructionLimit &&
        machine.Get(Register::Rip) == OtherAdd && machine.Get(Register::Rax) == 53 &&
        machine.Get(Register::Rbx) == 71 && Read(machine, DataB) == 0 && machine.LastRunInstructions() == 1,
        "A bounded slice did not preserve the actual arithmetic continuation");
    Require(machine.RunSlice(OtherAdd, OtherEnd, 100) == Cpu::StopReason::Address &&
        machine.Get(Register::Rip) == OtherEnd && machine.Get(Register::Rax) == 53 &&
        machine.Get(Register::Rbx) == 72 && machine.Get(Register::Rsp) == OtherStack && Read(machine, DataB) == 53,
        "Other guest continuation failed while the original host call remained suspended");
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "restored caller frame");
    machine.RestoreContext(originating);
    machine.Set(Register::Rip, CallGate + 1);
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "restored caller frame");
    machine.RestoreContext(originating);
    machine.Set(Register::Rsp, CallerStack);
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "restored caller frame");
    machine.RestoreContext(originating);
    Write(machine, ReturnSlot, std::uint64_t{0x100001210});
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "return word changed");
    Require(Read(machine, ReturnSlot) == 0x100001210, "Rejected completion rewrote the caller's changed return word");
    Write(machine, ReturnSlot, CallReturn);
    machine.Protect(CallGate, 4096, Permission::Read);
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "Guest access denied at 0x100003000");
    machine.Protect(CallGate, 4096, rx);
    machine.Protect(CallStack, 4096, static_cast<Permission>(0));
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "Guest access denied at 0x100008fe8");
    machine.Protect(CallStack, 4096, rw);
    machine.Protect(CallCode, 4096, Permission::Read);
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "Guest access denied at 0x10000120c");
    machine.Protect(CallCode, 4096, rx);
    CallerGuards(machine);

    const auto beforeThread = Current(machine);
    std::exception_ptr failure;
    std::thread nonowner([&] {
        try {
            Rejects([&] { machine.CompleteHostCall(call); }, "owner thread");
            Rejects([&] { unexpected = machine.PauseHostCall(); }, "owner thread");
            Rejects([&] { machine.RunSlice(CallGate, CallEnd, 100); }, "owner thread");
        } catch (...) { failure = std::current_exception(); }
    });
    nonowner.join();
    if (failure) std::rethrow_exception(failure);
    Require(Current(machine) == beforeThread, "Nonowner suspended-call operation changed guest state");
    machine.CompleteHostCall(call);
    Require(machine.Get(Register::Rip) == CallReturn && machine.Get(Register::Rsp) == CallerStack &&
        machine.Get(Register::Rax) == 7 && machine.Get(Register::Rbx) == 61 && Read(machine, DataB) == 53,
        "Completion did not return through the originating restored context exactly once");
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(call); }, "already completed");
    Require(machine.RunSlice(CallReturn, CallEnd, 100) == Cpu::StopReason::Address &&
        machine.Get(Register::Rip) == CallEnd && machine.Get(Register::Rsp) == CallerStack &&
        machine.Get(Register::Rax) == 16 && machine.Get(Register::Rbx) == 62 && Read(machine, DataA) == 16 &&
        Read(machine, DataB) == 53 && calls == 1 && machine.LastRunInstructions() == 3,
        "Completed host call redispatched, popped twice or lost the independent guest continuation");
    CallerGuards(machine);
}

void StickyTerminalSlices(bool exit) {
    Machine machine;
    SuspendedFixture(machine);
    Machine::SuspendedCall paused;
    unsigned calls = 0;
    machine.AddHostCall(CallGate, [&](Machine& running) {
        ++calls;
        if (calls == 1) {
            paused = running.PauseHostCall();
            if (exit) running.Exit(73); else running.RequestStop();
        } else {
            Require(calls == 2 && running.Get(Register::Rip) == CallGate &&
                running.Get(Register::Rsp) == ReturnSlot && Read(running, ReturnSlot) == CallReturn,
                "Fresh session did not reach the same actual CALL gate and stack layout");
            running.Set(Register::Rax, 33);
        }
    });
    const auto reason = exit ? Cpu::StopReason::Exit : Cpu::StopReason::Requested;
    auto abandoned = machine.CaptureContext();
    Require(machine.Run(CallEntry, CallEnd, 100) == reason && calls == 1 &&
        machine.Get(Register::Rip) == CallGate && machine.Get(Register::Rsp) == ReturnSlot &&
        machine.LastRunInstructions() == 3, "Terminal paused host call returned or consumed its frame");
    if (exit) Require(machine.ExitCode() == 73, "Terminal slice lost the actual guest exit code");
    // Both capture and overwriting a saved context must retain the originating
    // session identity when restoring an abandoned continuation later.
    if (exit) machine.SaveContext(abandoned);
    else abandoned = machine.CaptureContext();
    const auto stopped = Current(machine);
    Require(machine.RunSlice(CallEntry, CallEnd, 100) == reason && machine.LastRunInstructions() == 0 &&
        Current(machine) == stopped && calls == 1 && Read(machine, DataA) == 0,
        "Terminal RunSlice cleared stop/exit or advanced guest state");
    RejectsUnchanged(machine, [&] { machine.Run(CallEntry, CallEnd, 100); }, "completion of all suspended host calls");
    Require(machine.RunSlice(CallEntry, CallEnd, 100) == reason && machine.LastRunInstructions() == 0 &&
        Current(machine) == stopped && calls == 1, "Rejected session reset cleared terminal state before rejecting its pending call");
    RejectsUnchanged(machine, [&] { machine.CompleteHostCall(paused); }, "terminal stop or exit");
    constexpr std::uint64_t freshEntry = 0x100001600, freshEnd = 0x100001617;
    constexpr std::array<std::uint8_t, 23> fresh{
        0xb8,0x29,0,0,0, 0x83,0xc0,0x01, 0x83,0xf8,0x2a, 0x75,0x05,
        0x48,0x89,0x07, 0xeb,0x05, 0xb8,0x63,0,0,0};
    machine.Write(freshEntry, std::as_bytes(std::span(fresh)));
    RejectsUnchanged(machine, [&] { machine.Run(freshEntry, freshEnd, 100); },
        "completion of all suspended host calls");
    Require(machine.RunSlice(CallEntry, CallEnd, 100) == reason && machine.LastRunInstructions() == 0 &&
        Current(machine) == stopped && calls == 1,
        "Unrelated session reset with a live token cleared the sticky terminal state");
    paused = Machine::SuspendedCall{};
    Require(machine.RunSlice(CallGate, CallEnd, 100) == reason && machine.LastRunInstructions() == 0 &&
        Current(machine) == stopped && calls == 1 && Read(machine, DataA) == 0,
        "Abandoning a terminal host call prevented zero-instruction observation or replayed its frame");
    if (exit) Require(machine.ExitCode() == 73, "Abandoned terminal observation lost the exit code");
    RejectsUnchanged(machine, [&] { machine.Run(CallGate, CallEnd, 100); }, "completion before redispatch");
    Require(calls == 1 && Read(machine, DataA) == 0,
        "An abandoned actual CALL frame replayed its host callback or guest continuation");
    Initialize(machine, 0, 83, DataB, OtherStack);
    machine.Set(Register::Rip, freshEntry);
    const auto freshStopped = Current(machine);
    Require(machine.RunSlice(freshEntry, freshEnd, 100) == reason && machine.LastRunInstructions() == 0 &&
        Current(machine) == freshStopped && Read(machine, DataB) == 0 && calls == 1,
        "Abandoned terminal observation cleared state before a fresh session reset");
    if (exit) Require(machine.ExitCode() == 73, "Abandoned terminal observation cleared the sticky exit code");
    Require(machine.Run(freshEntry, freshEnd, 100) == Cpu::StopReason::Address &&
        machine.Get(Register::Rip) == freshEnd && machine.Get(Register::Rsp) == OtherStack &&
        machine.Get(Register::Rax) == 42 && machine.Get(Register::Rbx) == 83 &&
        machine.Get(Register::Rflags) == 0x46 && machine.LastRunInstructions() == 6 &&
        Read(machine, DataB) == 42 && Read(machine, DataA) == 0 && calls == 1,
        "Abandoned host call prevented a fresh session or corrupted independent x86 arithmetic, flags or control flow");
    Initialize(machine, 0, 83, DataA, CallerStack);
    machine.Set(Register::Rip, CallEntry);
    Require(machine.Run(CallEntry, CallEnd, 100) == Cpu::StopReason::Address &&
        machine.Get(Register::Rip) == CallEnd && machine.Get(Register::Rsp) == CallerStack &&
        machine.Get(Register::Rax) == 42 && machine.Get(Register::Rbx) == 84 &&
        machine.Get(Register::Rflags) == 2 && machine.LastRunInstructions() == 6 &&
        Read(machine, DataA) == 42 && Read(machine, DataB) == 42 && calls == 2,
        "Fresh actual CALL reusing an abandoned layout failed or corrupted its independent result and return frame");
    CallerGuards(machine);
    machine.RestoreContext(abandoned);
    RejectsUnchanged(machine, [&] { machine.RunSlice(CallGate, CallEnd, 100); }, "completion before redispatch");
    RejectsUnchanged(machine, [&] { machine.Run(CallGate, CallEnd, 100); }, "completion before redispatch");
    Require(calls == 2 && Read(machine, DataB) == 42 && Read(machine, DataA) == 42,
        "A fresh session forgot the abandoned frame or rolled back shared guest memory");
    CallerGuards(machine);
}

void ExpiredSuspendedCall() {
    Machine::SuspendedCall expired;
    {
        Machine previous;
        SuspendedFixture(previous);
        previous.AddHostCall(CallGate, [&](Machine& running) { expired = running.PauseHostCall(); });
        Require(previous.RunSlice(CallEntry, CallEnd, 100) == Cpu::StopReason::Paused,
            "Could not obtain an actual suspended frame for lifetime validation");
    }
    Machine replacement;
    Initialize(replacement, 91, 301, DataB, 0x8fd0);
    RejectsUnchanged(replacement, [&] { replacement.CompleteHostCall(expired); }, "expired");
    const auto previous = Current(replacement);
    expired = Machine::SuspendedCall{};
    Require(Current(replacement) == previous, "Expired suspended-call destruction touched a replacement Machine");
}
}

int main() {
    static_assert(!std::is_copy_constructible_v<Machine::Context> && !std::is_copy_assignable_v<Machine::Context>);
    static_assert(std::is_nothrow_move_constructible_v<Machine::Context> && std::is_nothrow_move_assignable_v<Machine::Context>);
    static_assert(!std::is_copy_constructible_v<Machine::SuspendedCall> && !std::is_copy_assignable_v<Machine::SuspendedCall>);
    static_assert(std::is_nothrow_move_constructible_v<Machine::SuspendedCall> && std::is_nothrow_move_assignable_v<Machine::SuspendedCall>);
    try {
        ProgressingContinuations();
        ExpiredAcrossMachines();
#if ANYPS5_CPU_MODERN_TCG
        OwnedBackingContinuations();
#endif
        SuspendedContinuations();
        StickyTerminalSlices(false);
        StickyTerminalSlices(true);
        ExpiredSuspendedCall();
        std::cout << "PASS C++ context move/lifetime/owner/idle contracts; actual x86 A/B continuation PCs and arithmetic; "
            "shared RAM and later mapping protection persist; expired context retirement is safe; "
            "suspended CALL frame validation and exactly-once completion; sticky terminal slices make no progress\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
