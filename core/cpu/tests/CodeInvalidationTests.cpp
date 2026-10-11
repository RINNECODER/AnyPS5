// Translated-code invalidation stays precise: host data writes keep translations, while host and guest
// code writes, remaps and protection changes are seen by the next execution.
#include <cpu/Cpu.hpp>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <vector>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t Code = 0x100000, Gate = 0x100800, Stack = 0x200000, Data = 0x300000;
constexpr std::uint64_t Shared = 0x400000, SharedAlias = 0x500000, Remapped = 0x600000, BorrowedCode = 0x700000;
constexpr std::size_t HostChunk = 0x4000;

void Require(bool condition, const char* diagnostic) {
    if (!condition) throw std::runtime_error(diagnostic);
}

template<class Function> void Rejects(Function&& function, const char* diagnostic) {
    try { function(); }
    catch (const std::exception& error) {
        Require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing rejection: ") + diagnostic);
}

void Bytes(Machine& machine, std::uint64_t address, const std::vector<std::uint8_t>& bytes) {
    machine.Write(address, std::as_bytes(std::span(bytes)));
}

std::vector<std::uint8_t> MovEax(std::uint32_t value) {
    std::vector<std::uint8_t> bytes{0xb8, 0, 0, 0, 0};
    std::memcpy(bytes.data() + 1, &value, 4);
    return bytes;
}

std::uint64_t RunMov(Machine& machine, std::uint64_t address) {
    machine.Set(Register::Rax, 0);
    Require(machine.Run(address, address + 5, 10) == Cpu::StopReason::Address, "Code fixture did not reach its stop");
    return machine.Get(Register::Rax);
}

std::byte* HostPages(std::size_t size) {
    auto* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    Require(memory != MAP_FAILED, "Host page allocation failed");
    return static_cast<std::byte*>(memory);
}

// A loop that calls an HLE gate each iteration; the gate writes borrowed data through Machine::Write.
// A translated borrowed executable page exists too; rechecking it must not drop owned code.
void DataWritesKeepTranslations(Machine& machine) {
    auto* borrowed = HostPages(HostChunk);
    std::memcpy(borrowed, MovEax(13).data(), 5);
    machine.MapBorrowed(BorrowedCode, {borrowed, HostChunk}, rx);
    Require(RunMov(machine, BorrowedCode) == 13, "Borrowed executable fixture did not run");
    std::vector<std::uint8_t> loop{0xbb, 50, 0, 0, 0,  // mov ebx, 50
        0xb9, 0x10, 0, 0, 0,                          // mov ecx, 16
        0x48, 0x01, 0xc8, 0xff, 0xc9, 0x75, 0xf9,     // add rax, rcx; dec ecx; jnz
        0x48, 0x89, 0x04, 0x25, 0x00, 0x00, 0x30, 0x00, // mov [Data], rax
        0xe8, 0, 0, 0, 0,                             // call Gate
        0xff, 0xcb, 0x75, 0xe3};                      // dec ebx; jnz
    const auto rel = static_cast<std::int32_t>(Gate - (Code + 0x100 + 30));
    std::memcpy(loop.data() + 26, &rel, 4);
    Bytes(machine, Code + 0x100, loop);
    unsigned calls = 0;
    machine.AddHostCall(Gate, [&](Machine& running) {
        const auto value = running.Get(Register::Rax) + ++calls;
        running.Write(Data + 8, std::as_bytes(std::span(&value, 1)));
    });
    const auto runLoop = [&] {
        machine.Set(Register::Rax, 0);
        Require(machine.Run(Code + 0x100, Code + 0x100 + loop.size(), 100000) == Cpu::StopReason::Address,
            "HLE loop did not finish");
    };
    runLoop();
    const auto warmed = machine.CodeCache();
    Require(warmed.Translations > 0, "Translation counter did not observe the HLE loop");
    const std::uint64_t word = 0x1122334455667788;
    machine.Write(Data + 64, std::as_bytes(std::span(&word, 1)));
    calls = 0;
    runLoop();
    const auto after = machine.CodeCache();
    Require(calls == 50, "HLE gate did not run once per loop iteration");
    Require(after.FullFlushes == warmed.FullFlushes, "HLE gates or borrowed data writes flushed the whole code cache");
    Require(after.CodeInvalidations == warmed.CodeInvalidations, "A data write invalidated translated code");
    Require(after.Translations == warmed.Translations, "A data write discarded translations that were then rebuilt");
}

void HostCodeWritesAreSeen(Machine& machine) {
    Bytes(machine, Code, MovEax(1));
    Require(RunMov(machine, Code) == 1, "Initial owned code did not run");
    const auto before = machine.CodeCache();
    Bytes(machine, Code + 1, {2});
    Require(RunMov(machine, Code) == 2, "Host write into executable owned code was not seen");
    const auto after = machine.CodeCache();
    Require(after.CodeInvalidations > before.CodeInvalidations && after.FullFlushes == before.FullFlushes,
        "Host code write did not use a precise invalidation");

    // Borrowed executable memory and a writable alias of the same host page.
    auto* host = HostPages(HostChunk);
    machine.MapBorrowed(Shared, {host, HostChunk}, rx);
    machine.MapBorrowed(SharedAlias, {host, HostChunk}, rw);
    Bytes(machine, Shared, MovEax(3));
    Require(RunMov(machine, Shared) == 3, "Borrowed executable code did not run");
    Bytes(machine, Shared, MovEax(4));
    Require(RunMov(machine, Shared) == 4, "Host write into borrowed executable code was not seen");
    Bytes(machine, SharedAlias, MovEax(5));
    Require(RunMov(machine, Shared) == 5, "Host write through a writable alias left stale executable code");
}

void GuestCodeWritesAreSeen(Machine& machine) {
    // The guest stores a new immediate through the writable alias of the executable page.
    Bytes(machine, Shared, MovEax(6));
    Require(RunMov(machine, Shared) == 6, "Shared code fixture did not run");
    std::vector<std::uint8_t> writer{0xc7, 0x04, 0x25, 0, 0, 0, 0, 0x2a, 0, 0, 0};  // mov dword [SharedAlias + 1], 42
    const auto target = static_cast<std::uint32_t>(SharedAlias + 1);
    std::memcpy(writer.data() + 3, &target, 4);
    Bytes(machine, Code + 0x200, writer);
    Require(machine.Run(Code + 0x200, Code + 0x200 + writer.size(), 10) == Cpu::StopReason::Address,
        "Guest code writer did not finish");
    Require(RunMov(machine, Shared) == 42, "Guest store into translated code through an alias was not seen");
}

void RemapRunsNewCode(Machine& machine) {
    machine.Map(Remapped, 0x1000, rw);
    Bytes(machine, Remapped, MovEax(7));
    machine.Protect(Remapped, 0x1000, rx);
    Require(RunMov(machine, Remapped) == 7, "Owned remap fixture did not run");
    machine.Unmap(Remapped, 0x1000);
    machine.Map(Remapped, 0x1000, rw);
    Bytes(machine, Remapped, MovEax(8));
    machine.Protect(Remapped, 0x1000, rx);
    Require(RunMov(machine, Remapped) == 8, "Owned unmap/remap executed stale code");

    machine.Unmap(Remapped, 0x1000);
    auto* first = HostPages(HostChunk);
    auto* second = HostPages(HostChunk);
    const auto firstCode = MovEax(9), secondCode = MovEax(10);
    std::memcpy(first, firstCode.data(), firstCode.size());
    std::memcpy(second, secondCode.data(), secondCode.size());
    machine.MapBorrowed(Remapped, {first, HostChunk}, rx);
    Require(RunMov(machine, Remapped) == 9, "Borrowed remap fixture did not run");
    machine.Unmap(Remapped, HostChunk);
    machine.MapBorrowed(Remapped, {second, HostChunk}, rx);
    Require(RunMov(machine, Remapped) == 10, "Borrowed unmap/remap executed stale code");
    machine.Unmap(Remapped, HostChunk);
    std::memcpy(first, MovEax(11).data(), 5);
    machine.MapBorrowed(Remapped, {first, HostChunk}, rx);
    Require(RunMov(machine, Remapped) == 11, "Remapped borrowed memory rewritten while unmapped executed stale code");
}

void ProtectTakesEffect(Machine& machine) {
    Bytes(machine, Code, MovEax(12));
    Require(RunMov(machine, Code) == 12, "Protect fixture did not run");
    const auto before = machine.CodeCache();
    machine.Protect(Code, 0x1000, Permission::Read);
    Rejects([&] { RunMov(machine, Code); }, "instruction fetch");
    machine.Protect(Code, 0x1000, rx);
    Require(RunMov(machine, Code) == 12, "Restored executable permission did not run");
    Require(machine.CodeCache().FullFlushes == before.FullFlushes, "Protect flushed the whole code cache");

    // mov dword [Data + 0x100], 1
    std::vector<std::uint8_t> store{0xc7, 0x04, 0x25, 0x00, 0x01, 0x30, 0x00, 1, 0, 0, 0};
    Bytes(machine, Code + 0x300, store);
    const auto runStore = [&] {
        return machine.Run(Code + 0x300, Code + 0x300 + store.size(), 10) == Cpu::StopReason::Address;
    };
    Require(runStore(), "Writable data store did not run");
    machine.Protect(Data, 0x4000, Permission::Read);
    Rejects(runStore, "protected write");
    machine.Protect(Data, 0x4000, rw);
    Require(runStore(), "Restored write permission did not take effect");
}
}  // namespace

int main() {
    try {
        Machine machine;
        machine.Map(Code, 0x1000, rw);
        machine.Map(Stack, 0x10000, rw);
        machine.MapBorrowed(Data, {HostPages(0x10000), 0x10000}, rw);
        Bytes(machine, Gate, {0xc3});
        machine.Protect(Code, 0x1000, rx);
        machine.Set(Register::Rsp, Stack + 0xff00);
        DataWritesKeepTranslations(machine);
        HostCodeWritesAreSeen(machine);
        GuestCodeWritesAreSeen(machine);
        RemapRunsNewCode(machine);
        ProtectTakesEffect(machine);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
