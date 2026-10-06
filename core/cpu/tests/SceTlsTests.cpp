#ifdef ANYPS5_GUEST_TLS_FIXTURE

_Alignas(16) _Thread_local unsigned long guestTlsInitialized = 0x1122334455667788UL;
_Alignas(16) _Thread_local unsigned long guestTlsZero[3];

unsigned long GuestTlsProbe(void) {
    const unsigned long before = guestTlsInitialized;
    guestTlsInitialized += 9;
    guestTlsZero[0] = before ^ 0xa5a5a5a5a5a5a5a5UL;
    guestTlsZero[2] += 0x31;
    return guestTlsZero[1] + guestTlsInitialized + guestTlsZero[2];
}

#else

#include <cpu/SceTls.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing SCE TLS rejection: ") + expected);
}

std::uint64_t word(const Machine& machine, std::uint64_t address) {
    std::uint64_t value = 0;
    machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}

void layoutAndBounds() {
    constexpr std::array<std::byte, 7> bytes{std::byte{0x98}, std::byte{0x76}, std::byte{0x54}, std::byte{0x32}, std::byte{0x10}, std::byte{0xfe}, std::byte{0xdc}};
    for (const auto alignment : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{16}, std::uint64_t{8192}}) {
        Machine machine;
        Cpu::SceTls tls(machine, bytes, 43, alignment);
        const auto rounded = alignment > 1 ? (std::uint64_t{43} + alignment - 1) / alignment * alignment : 43;
        require(tls.FsBase() - tls.TlsBase() == rounded, "TLS block uses the wrong Variant II negative offset");
        require(tls.TlsBase() % std::max<std::uint64_t>(alignment, 1) == 0, "TLS data is not aligned");
        require(machine.Get(Register::FsBase) == tls.FsBase(), "FS base was not installed for actual guest execution");
        require(tls.ModuleId() == 1 && tls.MemorySize() == 43, "Main TLS module identity or size changed");
        std::array<std::byte, 43> contents{};
        machine.Read(tls.TlsBase(), contents);
        require(std::equal(bytes.begin(), bytes.end(), contents.begin()), "TLS template initialization is corrupt");
        require(std::all_of(contents.begin() + bytes.size(), contents.end(), [](auto value) { return value == std::byte{0}; }), "TLS BSS is not zero initialized");
        require(word(machine, tls.FsBase()) == tls.FsBase(), "TCB self pointer is not the guest FS base");
        const auto dtv = word(machine, tls.FsBase() + 8);
        require(word(machine, dtv) == 1 && word(machine, dtv + 8) == 1 && word(machine, dtv + 16) == tls.TlsBase(), "Initial main-module DTV has wrong generation, bounds, or guest pointer");
        require(tls.Resolve(1, 42) == tls.TlsBase() + 42, "TLS resolution changed module-relative offset");
        require(tls.Dtpoff(1, 7) == 7, "DTPOFF unexpectedly includes the image load bias");
        require(tls.Tpoff(1, 7) == std::int64_t{7} - static_cast<std::int64_t>(rounded), "TPOFF is not signed relative to FS");
        rejects([&] { tls.Resolve(0, 0); }, "module ID");
        rejects([&] { tls.Resolve(2, 0); }, "module ID");
        rejects([&] { tls.Resolve(std::numeric_limits<std::uint64_t>::max(), 0); }, "module ID");
        rejects([&] { tls.Resolve(1, 43); }, "outside");
        rejects([&] { tls.Resolve(1, std::numeric_limits<std::uint64_t>::max()); }, "outside");
        rejects([&] { tls.Tpoff(2, 0); }, "module ID");
        rejects([&] { tls.Dtpoff(1, 43); }, "outside");
        machine.CheckAccess(tls.TlsBase(), 43, rw);
        rejects([&] { machine.CheckAccess(tls.TlsBase(), 1, Permission::Execute); }, "permissions");
    }
    for (const auto alignment : {std::uint64_t{3}, std::uint64_t{17}, std::uint64_t{1} << 25}) {
        Machine machine;
        rejects([&] { Cpu::SceTls tls(machine, bytes, 43, alignment); }, "alignment");
        require(machine.Get(Register::FsBase) == 0, "Rejected TLS setup modified FS");
    }
    Machine machine;
    rejects([&] { Cpu::SceTls tls(machine, bytes, 6, 16); }, "memory size");
    rejects([&] { Cpu::SceTls tls(machine, {}, 0, 16); }, "memory size");
    rejects([&] { Cpu::SceTls tls(machine, {}, std::uint64_t{1} << 25, 16); }, "memory size");
    rejects([&] { Cpu::SceTls tls(machine, bytes, 43, 16, 0x1234); }, "alignment");
    rejects([&] { Cpu::SceTls tls(machine, bytes, 43, 16, 0x7ffffffff000); }, "address range");
    require(machine.Get(Register::FsBase) == 0, "Rejected TLS setup modified FS");
}

void indexGateway() {
    Machine machine;
    constexpr std::uint64_t initial = 0x8877665544332211;
    Cpu::SceTls tls(machine, std::as_bytes(std::span(&initial, 1)), 32, 16);
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x2000, 4096, rx);
    machine.Map(0x3000, 4096, rw);
    machine.Map(0x4000, 4096, rw);
    constexpr std::array<std::uint8_t, 17> code{
        0xe8, 0xfb, 0x0f, 0, 0,
        0x48, 0x8b, 0x00,
        0x48, 0x89, 0x05, 0x01, 0x20, 0, 0,
        0x90, 0x90};
    machine.Write(0x1000, std::as_bytes(std::span(code)));
    constexpr std::array<std::uint8_t, 1> ret{0xc3};
    machine.Write(0x2000, std::as_bytes(std::span(ret)));
    machine.AddHostCall(0x2000, [&](Machine& guest) { guest.Set(Register::Rax, tls.ResolveIndex(guest.Get(Register::Rdi))); });
    const std::array<std::uint64_t, 2> index{1, 0};
    machine.Write(0x3000, std::as_bytes(std::span(index)));
    machine.Set(Register::Rdi, 0x3000);
    machine.Set(Register::Rsp, 0x4ff0);
    machine.Set(Register::Rbx, 0xabcdef1234567890);
    require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "TLS gateway did not resume the actual x86 caller");
    require(word(machine, 0x3010) == initial, "x86 caller could not dereference guest TLS address from index gateway");
    require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0xabcdef1234567890, "TLS gateway corrupted guest call ABI");
    rejects([&] { tls.ResolveIndex(0x4ff8); }, "access denied");
    machine.Protect(0x3000, 4096, Permission::Write);
    rejects([&] { tls.ResolveIndex(0x3000); }, "access denied");
}

void compiledFsProbe(const char* fixturePath) {
    std::ifstream input(fixturePath, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read compiler-built x86 TLS fixture");
    const std::vector<char> code{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    require(!code.empty() && code.size() < 4096, "Compiler-built TLS text must fit one guest page");
    Machine machine;
    constexpr std::uint64_t initial = 0x1122334455667788;
    Cpu::SceTls tls(machine, std::as_bytes(std::span(&initial, 1)), 40, 16);
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x4000, 4096, rw);
    machine.Write(0x1000, std::as_bytes(std::span(code)));
    const std::uint64_t returnAddress = 0x1800;
    machine.Write(0x4ff8, std::as_bytes(std::span(&returnAddress, 1)));
    machine.Set(Register::Rsp, 0x4ff8);
    machine.Set(Register::Rbx, 0xabcdef1234567890);
    require(machine.Run(0x1000, returnAddress, 100) == Cpu::StopReason::Address, "Compiler-built FS TLS program did not return");
    require(machine.Get(Register::Rax) == initial + 0x3a, "Compiler-built FS TLS program returned incorrect initialized/BSS arithmetic");
    require(word(machine, tls.TlsBase()) == initial + 9, "Compiler-built FS TLS store missed initialized data");
    require(word(machine, tls.TlsBase() + 16) == (initial ^ 0xa5a5a5a5a5a5a5a5), "Compiler-built FS TLS store missed BSS array");
    require(word(machine, tls.TlsBase() + 24) == 0 && word(machine, tls.TlsBase() + 32) == 0x31, "Compiler-built FS TLS program saw nonzero BSS or wrong offsets");
    require(machine.Get(Register::Rsp) == 0x5000 && machine.Get(Register::Rbx) == 0xabcdef1234567890, "Compiler-built FS TLS program corrupted return ABI");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Expected compiler-built x86 TLS text path");
        layoutAndBounds();
        indexGateway();
        compiledFsProbe(argv[1]);
        std::cout << "SCE main-module TLS: Variant II layout, compiler FS execution, index gateway and strict rejection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#endif
