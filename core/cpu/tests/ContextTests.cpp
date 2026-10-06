#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
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
}

int main() {
    static_assert(!std::is_copy_constructible_v<Machine::Context> && !std::is_copy_assignable_v<Machine::Context>);
    static_assert(std::is_nothrow_move_constructible_v<Machine::Context> && std::is_nothrow_move_assignable_v<Machine::Context>);
    try {
        ProgressingContinuations();
        ExpiredAcrossMachines();
        std::cout << "PASS C++ context move/lifetime/owner/idle contracts; actual x86 A/B continuation PCs and arithmetic; "
            "shared RAM and later mapping protection persist; expired context retirement is safe\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
