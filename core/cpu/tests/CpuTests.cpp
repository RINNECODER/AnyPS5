#include <cpu/Cpu.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
using Cpu::StopReason;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void inventory(const std::vector<Cpu::Mapping>& actual, std::initializer_list<Cpu::Mapping> expected) {
    std::size_t actualSize = 0;
    std::uint64_t previousEnd = 0;
    for (const auto& mapping : actual) {
        require(mapping.Size && !(mapping.Address & 4095) && !(mapping.Size & 4095) && mapping.Address >= previousEnd,
                "Mapping inventory is unordered, overlapping or not page aligned");
        previousEnd = mapping.Address + mapping.Size;
        actualSize += mapping.Size;
    }
    std::size_t expectedSize = 0;
    for (const auto& mapping : expected) {
        expectedSize += mapping.Size;
        for (auto page = mapping.Address; page < mapping.Address + mapping.Size; page += 4096) {
            const auto found = std::find_if(actual.begin(), actual.end(), [&](const Cpu::Mapping& extent) {
                return page >= extent.Address && page - extent.Address < extent.Size;
            });
            require(found != actual.end() && found->Permissions == mapping.Permissions && found->Borrowed == mapping.Borrowed,
                    "Mapping inventory omitted occupied pages or reported incorrect permissions or backing status");
        }
    }
    require(actualSize == expectedSize, "Mapping inventory retained unmapped pages or omitted occupied storage");
}
void failure(const std::function<void()>& operation, const char* expected, const char* alternative = nullptr) {
    try { operation(); }
    catch (const std::exception& error) {
        const std::string diagnostic = error.what();
        require(diagnostic.find(expected) != std::string::npos ||
                (alternative && diagnostic.find(alternative) != std::string::npos), error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing expected failure: ") + expected);
}
void code(Machine& machine, std::uint64_t address, std::initializer_list<std::uint8_t> bytes) {
    const std::vector<std::uint8_t> values(bytes);
    machine.Write(address, std::as_bytes(std::span(values)));
}
template<class T> void write(Machine& machine, std::uint64_t address, const T& value) { machine.Write(address, std::as_bytes(std::span(&value, 1))); }
template<class T> T read(Machine& machine, std::uint64_t address) {
    T value{};
    machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}
void setup(Machine& machine) {
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 8192, rw);
    machine.Set(Register::Rsp, 0x4ff0);
}
void widthsAndFlags() {
    Machine machine;
    setup(machine);
    code(machine, 0x1000, {0x48,0xb8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
                          0xb8,0xff,0xff,0xff,0xff,0x66,0xb8,0x34,0x12});
    require(machine.Run(0x1000, 0x1013, 20) == StopReason::Address, "Register-width program did not finish");
    require(machine.Get(Register::Rax) == 0xffff1234, "32-bit zero extension or 16-bit preservation is incorrect");
    code(machine, 0x1020, {0xb8,0x7f,0,0,0,0x04,0x01,0x9c,0x5b,
                          0xb8,0xff,0,0,0,0x04,0x01,0x9c,0x59,
                          0xb8,0xff,0xff,0xff,0xff,0x83,0xc0,0x01});
    require(machine.Run(0x1020, 0x103a, 30) == StopReason::Address, "Flag program did not finish");
    constexpr std::uint64_t arithmetic = 0x8d5;
    require((machine.Get(Register::Rbx) & arithmetic) == 0x890, "Signed 8-bit overflow flags are incorrect");
    require((machine.Get(Register::Rcx) & arithmetic) == 0x55, "Unsigned 8-bit carry/zero/parity flags are incorrect");
    require(machine.Get(Register::Rax) == 0 && (machine.Get(Register::Rflags) & arithmetic) == 0x55, "32-bit wraparound state is incorrect");
    code(machine, 0x1000, {0xb8,42,0,0,0});
    require(machine.Run(0x1000, 0x1005, 10) == StopReason::Address && machine.Get(Register::Rax) == 42, "External code write reused stale translated instructions");
}
void callsBranchesAndMemory() {
    Machine machine;
    setup(machine);
    code(machine, 0x1000, {0xb9,5,0,0,0,0x31,0xc0,0x01,0xc8,0xff,0xc9,0x75,0xfa,
                          0xbf,0,0x30,0,0,0x89,0x07,0xe8,0x27,0,0,0,0x89,0xc3});
    code(machine, 0x1040, {0x6b,0xc0,3,0xc3});
    require(machine.Run(0x1000, 0x101b, 100) == StopReason::Address, "Call/branch program did not finish");
    require(read<std::uint32_t>(machine, 0x3000) == 15, "Loop did not store sum 1 through 5");
    require(machine.Get(Register::Rax) == 45 && machine.Get(Register::Rbx) == 45, "Guest CALL/RET or IMUL result is incorrect");
    require(machine.Get(Register::Rsp) == 0x4ff0, "Guest CALL/RET corrupted stack");
}
void tlsAndSse() {
    Machine machine;
    setup(machine);
    machine.Set(Register::FsBase, 0x3000);
    machine.Set(Register::GsBase, 0x3100);
    write(machine, 0x3010, std::uint32_t{123});
    write(machine, 0x3118, std::uint32_t{456});
    code(machine, 0x1000, {0x64,0x8b,0x04,0x25,0x10,0,0,0,0x65,0x03,0x04,0x25,0x18,0,0,0});
    require(machine.Run(0x1000, 0x1010, 10) == StopReason::Address && machine.Get(Register::Rax) == 579, "FS/GS TLS addressing is incorrect");
    const std::array<std::uint32_t,4> left{1,0xffffffff,0x7fffffff,100};
    const std::array<std::uint32_t,4> right{2,2,1,42};
    write(machine, 0x3203, left);
    write(machine, 0x3223, right);
    machine.Set(Register::Rdi, 0x3203);
    machine.Set(Register::Rsi, 0x3223);
    machine.Set(Register::Rdx, 0x3243);
    code(machine, 0x1020, {0xf3,0x0f,0x6f,0x07,0xf3,0x0f,0x6f,0x0e,0x66,0x0f,0xfe,0xc1,0xf3,0x0f,0x7f,0x02});
    require(machine.Run(0x1020, 0x1030, 10) == StopReason::Address, "SSE program did not finish");
    const std::array<std::uint32_t,4> expected{3,1,0x80000000,142};
    require(read<std::array<std::uint32_t,4>>(machine, 0x3243) == expected, "Unaligned SSE packed-add result is incorrect");
    machine.Set(Register::Rdi, 0x3400);
    code(machine, 0x1040, {0xd9,0x3f,0x0f,0xae,0x5f,4});
    require(machine.Run(0x1040, 0x1046, 10) == StopReason::Address, "FP-default-state program did not finish");
    require(read<std::uint16_t>(machine, 0x3400) == 0x037f && read<std::uint32_t>(machine, 0x3404) == 0x1f80, "Guest ABI floating-point defaults are incorrect");
}
void unsupportedVectorEncodings() {
    const std::vector<std::vector<std::uint8_t>> encodings{
#if !ANYPS5_CPU_MODERN_TCG
        {0xc5,0xf0,0x58,0xc2},
        {0xc4,0xe1,0x70,0x58,0xc2},
        {0x67,0xc5,0xf0,0x58,0xc2},
        {0x64,0xc4,0xe1,0x70,0x58,0xc2},
#endif
        {0x62,0xf1,0x74,0x08,0x58,0xc2},
        {0x64,0x62,0xf1,0x74,0x08,0x58,0xc2},
    };
    for (const auto& encoding : encodings) {
        Machine machine;
        setup(machine);
        const std::array<std::uint32_t,4> destination{0x42c80000,0x42c80000,0x42c80000,0x42c80000};
        const std::array<std::uint32_t,4> first{0x3f800000,0x3f800000,0x3f800000,0x3f800000};
        const std::array<std::uint32_t,4> second{0x40000000,0x40000000,0x40000000,0x40000000};
        const std::array<std::uint32_t,4> sentinel{0xaabbccdd,0x12345678,0xdeadbeef,0x76543210};
        write(machine, 0x3200, destination);
        write(machine, 0x3220, first);
        write(machine, 0x3240, second);
        write(machine, 0x3260, sentinel);
        machine.Set(Register::Rdi, 0x3200);
        machine.Set(Register::Rsi, 0x3220);
        machine.Set(Register::Rdx, 0x3240);
        machine.Set(Register::Rcx, 0x3260);
        std::vector<std::uint8_t> program{0x0f,0x10,0x07,0x0f,0x10,0x0e,0x0f,0x10,0x12};
        program.insert(program.end(), encoding.begin(), encoding.end());
        const auto storeAddress = 0x1100 + program.size();
        program.insert(program.end(), {0x0f,0x11,0x01});
        machine.Write(0x1100, std::as_bytes(std::span(program)));
        bool rejected = false;
        try { machine.Run(0x1100, storeAddress + 3, 20); }
        catch (const std::exception& error) {
            const std::string diagnostic = error.what();
            require(diagnostic.find("Unsupported guest VEX/EVEX instruction") != std::string::npos ||
                    diagnostic.find("Unsupported guest instruction") != std::string::npos, error.what());
            rejected = true;
        }
        if (!rejected) {
            const auto actual = read<std::array<std::uint32_t,4>>(machine, 0x3260);
            throw std::runtime_error("VEX/EVEX unexpectedly executed: first result bits=" + std::to_string(actual[0]) +
                                     "; VADDPS(1,2) mathematical result bits=1077936128; explicit rejection required");
        }
        require(read<std::array<std::uint32_t,4>>(machine, 0x3260) == sentinel,
                "Rejected vector instruction executed its following guest memory store");
        require(machine.Run(storeAddress, storeAddress + 3, 10) == StopReason::Address,
                "Legacy SSE register-state readback did not finish after rejected vector instruction");
        require(read<std::array<std::uint32_t,4>>(machine, 0x3260) == destination,
                "Unsupported vector instruction changed XMM destination before rejection");
    }
}
#if ANYPS5_CPU_MODERN_TCG
void modernVectors() {
    using Vector = std::array<std::uint32_t, 8>;
    const Vector first{0x3f800000,0x40000000,0x40400000,0x40800000,0x40a00000,0x40c00000,0x40e00000,0x41000000};
    const Vector second{0x40000000,0x40800000,0x40c00000,0x41000000,0x41200000,0x41400000,0x41600000,0x41800000};
    const Vector destination{0x42c80000,0x42c80000,0x42c80000,0x42c80000,0x42c80000,0x42c80000,0x42c80000,0x42c80000};
    struct Case {
        const char* name;
        Vector left, right, oldDestination, expected;
        std::vector<std::uint8_t> instruction;
    };
    const Case cases[] = {
        {"AVX256 three operands", first, second, destination,
            {0x40400000,0x40c00000,0x41100000,0x41400000,0x41700000,0x41900000,0x41a80000,0x41c00000},
            {0xc5,0xfc,0x58,0xd1}},
        {"VEX128 clears upper lanes", first, second, destination,
            {0x40400000,0x40c00000,0x41100000,0x41400000,0,0,0,0},
            {0xc5,0xf8,0x58,0xd1}},
        {"AVX2 wrapping addition",
            {1,0xffffffff,0x7fffffff,0x80000000,0x12345678,17,0,0xfffffffe},
            {2,1,1,0xffffffff,0x11111111,25,0xffffffff,3}, destination,
            {3,0,0x80000000,0x7fffffff,0x23456789,42,0xffffffff,1},
            {0xc5,0xfd,0xfe,0xd1}},
        {"FMA fused residual and ordering",
            {0xbf800002,0x40000000,0x40400000,0x40800000,0x40a00000,0x40c00000,0x40e00000,0x41000000},
            {0x3f800001,0x40800000,0x40c00000,0x41000000,0x41200000,0x41400000,0x41600000,0x41800000},
            {0x3f800001,0x40000000,0x40000000,0x40000000,0x40000000,0x40000000,0x40000000,0x40000000},
            {0x28800000,0x41200000,0x41700000,0x41a00000,0x41c80000,0x41f00000,0x420c0000,0x42200000},
            {0xc4,0xe2,0x7d,0x98,0xd1}},
    };
    for (const auto& test : cases) {
        Machine machine;
        setup(machine);
        write(machine, 0x3200, test.left);
        write(machine, 0x3220, test.right);
        write(machine, 0x3240, test.oldDestination);
        write(machine, 0x32c0, std::uint64_t{0x123456789abcdef0});
        machine.Set(Register::Rdi, 0x3200);
        machine.Set(Register::Rsi, 0x3220);
        machine.Set(Register::Rcx, 0x3240);
        machine.Set(Register::Rdx, 0x3260);
        machine.Set(Register::Rax, 0x8877665544332211);
        machine.Set(Register::Rflags, 2);
        std::vector<std::uint8_t> program{0xc5,0xfc,0x10,0x07,0xc5,0xfc,0x10,0x0e,0xc5,0xfc,0x10,0x11};
        program.insert(program.end(), test.instruction.begin(), test.instruction.end());
        program.insert(program.end(), {0xc5,0xfc,0x11,0x12,0xc5,0xfc,0x11,0x42,0x20,0xc5,0xfc,0x11,0x4a,0x40});
        machine.Write(0x1100, std::as_bytes(std::span(program)));
        require(machine.Run(0x1100, 0x1100 + program.size(), 20) == StopReason::Address,
                "Modern vector program did not reach its expected control-flow boundary");
        if (read<Vector>(machine, 0x3260) != test.expected) throw std::runtime_error(std::string(test.name) + " has incorrect result bits");
        require(read<Vector>(machine, 0x3280) == test.left && read<Vector>(machine, 0x32a0) == test.right,
                "Three-operand vector instruction changed a source register");
        require(read<std::uint64_t>(machine, 0x32c0) == 0x123456789abcdef0,
                "Vector result store exceeded its expected memory range");
        require(machine.Get(Register::Rax) == 0x8877665544332211 && machine.Get(Register::Rflags) == 2,
                "Vector program changed scalar register or flag state");
    }
    const std::array<std::uint16_t,8> halves{0x3c00,0xc000,0x3800,0,0x8000,0x7bff,0x0400,0x0001};
    const Vector singles{0x3f800000,0xc0000000,0x3f000000,0,0x80000000,0x477fe000,0x38800000,0x33800000};
    Machine machine;
    setup(machine);
    write(machine, 0x3200, halves);
    machine.Set(Register::Rdi, 0x3200);
    machine.Set(Register::Rdx, 0x3260);
    code(machine, 0x1100, {0xc5,0xfa,0x6f,0x07,0xc4,0xe2,0x7d,0x13,0xd0,0xc5,0xfc,0x11,0x12});
    require(machine.Run(0x1100, 0x110d, 10) == StopReason::Address && read<Vector>(machine, 0x3260) == singles,
            "F16C half-to-single conversion lost signed zero, finite maximum, normal or subnormal values");
    write(machine, 0x3200, singles);
    code(machine, 0x1140, {0xc5,0xfc,0x10,0x17,0xc4,0xe3,0x7d,0x1d,0xd3,0,0xc5,0xfa,0x7f,0x1a});
    require(machine.Run(0x1140, 0x114e, 10) == StopReason::Address && read<std::array<std::uint16_t,8>>(machine, 0x3260) == halves,
            "F16C single-to-half conversion disagrees with independently specified half encodings");
}
void modernCpuFeatures() {
    Machine machine;
    setup(machine);
    code(machine, 0x1100, {0x0f,0xa2});
    machine.Set(Register::Rax, 1);
    machine.Set(Register::Rcx, 0);
    require(machine.Run(0x1100, 0x1102, 10) == StopReason::Address, "Guest CPUID leaf 1 did not execute");
    constexpr std::uint64_t required = (1ULL << 12) | (1ULL << 26) | (1ULL << 27) | (1ULL << 28) | (1ULL << 29);
    require((machine.Get(Register::Rcx) & required) == required, "CPUID does not advertise the FMA/XSAVE/OSXSAVE/AVX/F16C instructions exercised by the guest");
    machine.Set(Register::Rax, 7);
    machine.Set(Register::Rcx, 0);
    require(machine.Run(0x1100, 0x1102, 10) == StopReason::Address && (machine.Get(Register::Rbx) & 32),
            "CPUID does not advertise the AVX2 instruction exercised by the guest");
    machine.Set(Register::Rcx, 0);
    code(machine, 0x1120, {0x0f,0x01,0xd0});
    require(machine.Run(0x1120, 0x1123, 10) == StopReason::Address && machine.Get(Register::Rax) == 7 && machine.Get(Register::Rdx) == 0,
            "XGETBV does not enable x87, SSE and YMM state for advertised AVX execution");
}
void invalidInstructionDiagnostics() {
    const std::vector<std::vector<std::uint8_t>> encodings{{0x0f,0x20,0xc8}, {0xf0,0x0f,0x08}};
    for (const auto& encoding : encodings) {
        Machine machine;
        setup(machine);
        constexpr std::uint64_t initialRax = 0x8877665544332211;
        constexpr std::uint64_t initialRbx = 0x123456789abcdef0;
        machine.Set(Register::Rax, initialRax);
        machine.Set(Register::Rbx, initialRbx);
        machine.Set(Register::Rdi, 0x3000);
        machine.Set(Register::Rflags, 2);
        write(machine, 0x3000, std::uint32_t{0xaabbccdd});
        auto program = encoding;
        program.insert(program.end(), {0xc7,0x07,0x78,0x56,0x34,0x12,0x48,0xff,0xc3});
        machine.Write(0x1100, std::as_bytes(std::span(program)));
        bool rejected = false;
        try { machine.Run(0x1100, 0x1100 + program.size(), 20); }
        catch (const std::exception& error) {
            require(std::string(error.what()) == "Unsupported guest instruction at 0x1100", error.what());
            rejected = true;
        }
        require(rejected, "Invalid control-register or LOCK encoding executed without an unsupported-instruction diagnostic");
        require(machine.Get(Register::Rip) == 0x1100 && machine.Get(Register::Rax) == initialRax &&
                machine.Get(Register::Rbx) == initialRbx && machine.Get(Register::Rflags) == 2,
                "Invalid instruction changed guest registers or flags before rejection");
        require(read<std::uint32_t>(machine, 0x3000) == 0xaabbccdd,
                "Invalid instruction retired its following memory store before rejection");
        require(machine.Run(0x1100 + encoding.size(), 0x1100 + program.size(), 10) == StopReason::Address &&
                read<std::uint32_t>(machine, 0x3000) == 0x12345678 && machine.Get(Register::Rbx) == initialRbx + 1,
                "Valid continuation after rejected instruction failed to execute its store and increment");
    }
}
#endif
void unsupportedXopEncoding() {
    for (const std::uint8_t prefix : {0, 0x64, 0x67}) {
        Machine machine;
        setup(machine);
        constexpr std::uint64_t stackValue = 0x123456789abcdef0;
        constexpr std::uint64_t initialRax = 0xfedcba9876543210;
        write(machine, 0x4ff0, stackValue);
        machine.Set(Register::Rax, initialRax);
        std::vector<std::uint8_t> program;
        if (prefix) program.push_back(prefix);
        program.insert(program.end(), {0x8f,0xe8,0x60,0xa2,0xe2,0x10});
        machine.Write(0x1000, std::as_bytes(std::span(program)));
        bool rejected = false;
        try { machine.Run(0x1000, 0x1002 + (prefix != 0), 10); }
        catch (const std::exception& error) {
            const std::string diagnostic = error.what();
            require(diagnostic.find("Unsupported guest XOP instruction") != std::string::npos ||
                    diagnostic.find("Unsupported guest instruction") != std::string::npos, error.what());
            rejected = true;
        }
        if (!rejected) {
            throw std::runtime_error("XOP unexpectedly executed: RAX=" + std::to_string(machine.Get(Register::Rax)) +
                                     " RSP=" + std::to_string(machine.Get(Register::Rsp)) +
                                     "; XOP must not decode as legacy POP");
        }
        require(machine.Get(Register::Rax) == initialRax && machine.Get(Register::Rsp) == 0x4ff0,
                "Rejected XOP instruction changed scalar registers or consumed guest stack");
        require(read<std::uint64_t>(machine, 0x4ff0) == stackValue, "Rejected XOP instruction changed guest stack memory");
        code(machine, 0x1040, {0x8f,0xc0});
        require(machine.Run(0x1040, 0x1042, 10) == StopReason::Address,
                "Vector prefix guard rejected valid legacy POP encoding");
        require(machine.Get(Register::Rax) == stackValue && machine.Get(Register::Rsp) == 0x4ff8,
                "Legacy POP did not retain its register and stack semantics");
    }
}
void syscallAndHostAbi() {
    Machine machine;
    setup(machine);
    code(machine, 0x1000, {0xb8,7,0,0,0,0xbf,11,0,0,0,0x0f,0x05,0x89,0xc3});
    machine.SetSyscallHandler([](Machine& guest) {
        require(guest.Get(Register::Rax) == 7 && guest.Get(Register::Rdi) == 11, "Syscall arguments were corrupted");
        require(guest.Get(Register::Rcx) == 0x100c && guest.Get(Register::R11) == guest.Get(Register::Rflags), "SYSCALL return registers are incorrect");
        guest.Set(Register::Rax, 99);
    });
    require(machine.Run(0x1000, 0x100e, 20) == StopReason::Address && machine.Get(Register::Rbx) == 99, "Syscall return did not reach guest");
    code(machine, 0x1020, {0xbf,21,0,0,0,0xe8,0x16,0,0,0,0x83,0xc0,1});
    code(machine, 0x1040, {0x0f,0x0b});
    machine.AddHostCall(0x1040, [](Machine& guest) { guest.Set(Register::Rax, guest.Get(Register::Rdi) * 2); });
    require(machine.Run(0x1020, 0x102d, 20) == StopReason::Address && machine.Get(Register::Rax) == 43, "Host import did not return through guest CALL stack");
    require(machine.Get(Register::Rsp) == 0x4ff0, "Host import did not restore stack");
    for (const std::uint8_t prefix : {0x66, 0x48}) {
        machine.Set(Register::Rax, 7);
        machine.SetSyscallHandler([](Machine& guest) { guest.Set(Register::Rax, 101); });
        code(machine, 0x10f0, {prefix,0x0f,0x05});
        require(machine.Run(0x10f0, 0x10f3, 10) == StopReason::Address, "Prefixed SYSCALL did not finish at its three-byte return address");
        require(machine.Get(Register::Rip) == 0x10f3 && machine.Get(Register::Rcx) == 0x10f3,
                "Prefixed SYSCALL saved RCX different from its actual three-byte return address");
        require(machine.Get(Register::Rax) == 101, "Prefixed SYSCALL lost its service return value");
    }
    machine.SetSyscallHandler([](Machine& guest) { guest.Exit(17); });
    code(machine, 0x1060, {0x0f,0x05,0x0f,0x0b});
    require(machine.Run(0x1060, 0x1064, 10) == StopReason::Exit && machine.ExitCode() == 17, "Guest exit service did not stop execution");
}
void borrowedMemory() {
#if ANYPS5_CPU_MODERN_TCG
    alignas(16384) std::array<std::byte,16384> backing{};
    alignas(16384) std::array<std::byte,16384> executable{};
    std::array<std::byte,4096> unknownAllocation{};
#endif
    Machine machine;
    machine.Map(0x1000, 4096, rx);
#if ANYPS5_CPU_MODERN_TCG
    machine.MapBorrowed(0x3000, backing, rw);
    backing[3] = std::byte{9};
    machine.Set(Register::Rdi, 0x3000);
    code(machine, 0x1000, {0x0f,0xb6,0x47,3,0x83,0xc0,4,0x89,0x47,8});
    require(machine.Run(0x1000, 0x100a, 10) == StopReason::Address,
            "Borrowed-memory program did not finish");
    require(backing[8] == std::byte{13} && backing[9] == std::byte{0} && machine.Get(Register::Rax) == 13,
            "Guest/host borrowed-memory visibility is incorrect");
    backing[3] = std::byte{20};
    require(machine.Run(0x1000, 0x100a, 10) == StopReason::Address && machine.Get(Register::Rax) == 24 && backing[8] == std::byte{24},
            "Host update was invisible on resumed guest execution");
    machine.MapBorrowed(0x8000, std::span(backing).subspan(4096, 4096), rw);
    machine.Protect(0x4000, 4096, Permission::Read);
    backing[4099] = std::byte{38};
    machine.Set(Register::Rdi, 0x8000);
    require(machine.Run(0x1000, 0x100a, 10) == StopReason::Address && read<std::uint32_t>(machine, 0x4008) == 42 && backing[4104] == std::byte{42},
            "A 4 KiB backing alias lost shared bytes or inherited its peer's read-only permission");
    machine.Set(Register::Rdi, 0x4000);
    failure([&] { machine.Run(0x1000, 0x100a, 10); }, "protected write at 0x4008");
    require(read<std::uint32_t>(machine, 0x8008) == 42,
            "Rejected write through a read-only alias changed shared storage");
    executable[0] = std::byte{0xb8};
    executable[1] = std::byte{7};
    machine.MapBorrowed(0x9000, executable, rx);
    machine.MapBorrowed(0x10000, std::span(executable).first(4096), rx);
    for (const std::uint64_t entry : {0x9000, 0x10000})
        require(machine.Run(entry, entry + 5, 10) == StopReason::Address && machine.Get(Register::Rax) == 7,
                "Borrowed executable alias did not execute its actual source bytes");
    executable[1] = std::byte{19};
    for (const std::uint64_t entry : {0x9000, 0x10000})
        require(machine.Run(entry, entry + 5, 10) == StopReason::Address && machine.Get(Register::Rax) == 19,
                "Borrowed executable alias retained stale translated instructions after a host update");
    failure([&] { machine.MapBorrowed(0x20000, unknownAllocation, rw); }, "requires a complete aligned host-page backing");
    failure([&] { machine.CheckAccess(0x20000, 1, Permission::Read); }, "Guest access denied at 0x20000");
#else
    std::array<std::byte,4096> backing{};
    machine.MapBorrowed(0x3000, backing, rw);
    backing[3] = std::byte{9};
    machine.Set(Register::Rdi, 0x3000);
    code(machine, 0x1000, {0x0f,0xb6,0x47,3,0x83,0xc0,4,0x89,0x47,8});
    require(machine.Run(0x1000, 0x100a, 10) == StopReason::Address, "Borrowed-memory program did not finish");
    require(backing[8] == std::byte{13} && backing[9] == std::byte{0} && machine.Get(Register::Rax) == 13, "Guest/host borrowed-memory visibility is incorrect");
    backing[3] = std::byte{20};
    require(machine.Run(0x1000, 0x100a, 10) == StopReason::Address && machine.Get(Register::Rax) == 24 && backing[8] == std::byte{24}, "Host update was invisible on resumed guest execution");
    std::array<std::byte,4096> executable{};
    executable[0] = std::byte{0xb8};
    executable[1] = std::byte{7};
    machine.MapBorrowed(0x5000, executable, rx);
    require(machine.Run(0x5000, 0x5005, 10) == StopReason::Address && machine.Get(Register::Rax) == 7, "Borrowed executable code did not run");
    executable[1] = std::byte{19};
    require(machine.Run(0x5000, 0x5005, 10) == StopReason::Address && machine.Get(Register::Rax) == 19, "Borrowed executable update reused stale translated instructions");
#endif
}
void errorsAndBudget() {
    Machine machine;
    setup(machine);
    code(machine, 0x1000, {0x0f,0x05});
    failure([&] { machine.Run(0x1000, 0x1002, 10); }, "Unsupported guest syscall");
    code(machine, 0x1010, {0x0f,0x0b});
    failure([&] { machine.Run(0x1010, 0x1012, 10); }, "Unsupported guest instruction");
    code(machine, 0x1020, {0xcd,0x80});
    failure([&] { machine.Run(0x1020, 0x1022, 10); }, "Unsupported guest interrupt 128");
    machine.Set(Register::Rdi, 0x9000);
    code(machine, 0x1030, {0x8b,0x07});
    failure([&] { machine.Run(0x1030, 0x1032, 10); }, "unmapped read at 0x9000");
    code(machine, 0x1040, {0x89,0x07});
    machine.Set(Register::Rdi, 0x3000);
    machine.Protect(0x3000, 4096, Permission::Read);
    failure([&] { machine.Run(0x1040, 0x1042, 10); }, "protected write at 0x3000");
    failure([&] { machine.Run(0x3000, 0x3002, 10); }, "protected instruction fetch at 0x3000");
    machine.Protect(0x3000, 4096, Permission::Write);
    failure([&] { machine.Run(0x1030, 0x1032, 10); }, "protected read at 0x3000");
    failure([&] { machine.Run(0x9000, 0x9002, 10); }, "unmapped instruction fetch at 0x9000");
    code(machine, 0x1050, {0xff,0xc0,0xeb,0xfc});
    machine.Set(Register::Rax, 0);
    require(machine.Run(0x1050, 0x1054, 10) == StopReason::InstructionLimit, "Infinite loop ignored instruction budget");
    require(machine.Get(Register::Rax) == 5 && machine.Get(Register::Rip) == 0x1050, "Instruction budget executed wrong number of guest instructions");
    machine.AddHostCall(0x1070, [](Machine&) { throw std::runtime_error("host callback failure"); });
    code(machine, 0x1070, {0x0f,0x0b});
    failure([&] { machine.Run(0x1070, 0x1072, 10); }, "host callback failure");
    machine.SetSyscallHandler([](Machine&) { throw std::runtime_error("syscall callback failure"); });
    failure([&] { machine.Run(0x1000, 0x1002, 10); }, "syscall callback failure");
    machine.AddHostCall(0x1080, [](Machine& guest) { guest.RequestStop(); });
    code(machine, 0x1080, {0x0f,0x0b});
    require(machine.Run(0x1080, 0x1082, 10) == StopReason::Requested, "Requested stop did not propagate");
    machine.AddHostCall(0x1088, [](Machine&) {});
    code(machine, 0x1088, {0x0f,0x0b});
    machine.Set(Register::Rsp, 0x3000);
    failure([&] { machine.Run(0x1088, 0x108a, 10); }, "return stack is not readable");
    code(machine, 0x1090, {0xf4});
    failure([&] { machine.Run(0x1090, 0x1091, 10); }, "Unsupported guest privileged service instruction");
    const std::vector<std::vector<std::uint8_t>> privileged{
        {0xfa}, {0xfb}, {0x0f,0x32}, {0x0f,0x30}, {0x0f,0x20,0xc0}, {0x0f,0x22,0xc0}, {0x66,0xf4},
        {0x0f,0x06}, {0x0f,0x08}, {0x0f,0x09}, {0x0f,0x01,0xf8},
        {0x0f,0x01,0x17}, {0x0f,0x01,0x1f}, {0x0f,0x01,0xc8}, {0x0f,0x01,0xc9}
    };
    machine.Set(Register::Rdi, 0x4000);
    machine.Set(Register::Rcx, 0);
    for (const auto& bytes : privileged) {
        machine.Write(0x10e0, std::as_bytes(std::span(bytes)));
        failure([&] { machine.Run(0x10e0, 0x10e0 + bytes.size(), 10); }, "Unsupported guest privileged service instruction",
                "Unsupported guest instruction");
    }
    code(machine, 0x10a0, {0xe4,0x80});
    failure([&] { machine.Run(0x10a0, 0x10a2, 10); }, "Unsupported guest port input 128");
    code(machine, 0x10b0, {0xe6,0x80});
    failure([&] { machine.Run(0x10b0, 0x10b2, 10); }, "Unsupported guest port output 128");
    code(machine, 0x10c0, {0x0f,0x34});
    failure([&] { machine.Run(0x10c0, 0x10c2, 10); }, "Unsupported guest SYSENTER service");
    machine.AddHostCall(0x10d0, [](Machine& guest) { static_cast<void>(read<std::uint32_t>(guest, 0x3000)); });
    code(machine, 0x10d0, {0x0f,0x0b});
    failure([&] { machine.Run(0x10d0, 0x10d2, 10); }, "Guest access denied at 0x3000");
    failure([&] { machine.CheckAccess(0x4ff8, 16, Permission::Read); }, "Guest access denied at 0x5000");
    failure([&] { machine.CheckAccess(0xfffffffffffffff8, 16, Permission::Read); }, "access range overflows");
}
void unmapRanges() {
    Machine machine;
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 12288, rw);
    code(machine, 0x1000, {0x48,0x8b,0x07});
    code(machine, 0x1020, {0x48,0x89,0x07});
    write(machine, 0x3000, std::uint64_t{11});
    write(machine, 0x4000, std::uint64_t{22});
    write(machine, 0x5000, std::uint64_t{33});
    machine.Protect(0x3000, 4096, Permission::Read);
    machine.Protect(0x4000, 4096, static_cast<Permission>(0));
    machine.Protect(0x5000, 4096, Permission::Write);
    const auto protectedSnapshot = machine.Mappings();
    inventory(protectedSnapshot, {{0x1000,4096,rx,false}, {0x3000,4096,Permission::Read,false},
              {0x4000,4096,static_cast<Permission>(0),false}, {0x5000,4096,Permission::Write,false}});
    std::string foreignDiagnostic;
    std::thread foreign([&] {
        try { machine.Mappings(); }
        catch (const std::exception& error) { foreignDiagnostic = error.what(); }
    });
    foreign.join();
    require(foreignDiagnostic.find("owner thread") != std::string::npos, "Mapping inventory allowed access outside its owner thread");
    machine.Unmap(0x4000, 4096);
    inventory(machine.Mappings(), {{0x1000,4096,rx,false}, {0x3000,4096,Permission::Read,false},
              {0x5000,4096,Permission::Write,false}});
    inventory(protectedSnapshot, {{0x1000,4096,rx,false}, {0x3000,4096,Permission::Read,false},
              {0x4000,4096,static_cast<Permission>(0),false}, {0x5000,4096,Permission::Write,false}});
    machine.Set(Register::Rdi, 0x4000);
    failure([&] { machine.Run(0x1000, 0x1003, 10); }, "unmapped read at 0x4000");
    failure([&] { machine.Unmap(0x3000, 12288); }, "Guest access denied at 0x4000");
    failure([&] { machine.Unmap(0x3001, 4096); }, "4 KiB aligned");
    failure([&] { machine.Unmap(0xfffffffffffff000, 4096); }, "without overflow");
    machine.Set(Register::Rdi, 0x3000);
    machine.Set(Register::Rax, 99);
    failure([&] { machine.Run(0x1020, 0x1023, 10); }, "protected write at 0x3000");
    require(read<std::uint64_t>(machine, 0x3000) == 11, "Partial or rejected unmap changed its retained read-only neighbor");
    machine.Set(Register::Rdi, 0x5000);
    failure([&] { machine.Run(0x1000, 0x1003, 10); }, "protected read at 0x5000");
    machine.Set(Register::Rax, 99);
    require(machine.Run(0x1020, 0x1023, 10) == StopReason::Address,
            "Partial or rejected unmap removed its retained writable neighbor");
    machine.Protect(0x5000, 4096, rw);
    require(read<std::uint64_t>(machine, 0x5000) == 99, "Retained neighbor no longer refers to its original storage");
    machine.Map(0x4000, 4096, rw);
    machine.Set(Register::Rdi, 0x4000);
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 0,
            "Remapping a removed page retained old bytes or a stale memory translation");
    machine.Map(0x2000, 4096, rx);
    unsigned calls = 0;
    machine.AddHostCall(0x2000, [&](Machine& guest) { ++calls; guest.Exit(7); });
    require(machine.Run(0x2000, 0, 10) == StopReason::Exit && machine.ExitCode() == 7 && calls == 1,
            "Original host gate did not execute before unmapping");
    machine.Unmap(0x2000, 4096);
    machine.Map(0x2000, 4096, rx);
    code(machine, 0x2000, {0xb8,77,0,0,0});
    require(machine.Run(0x2000, 0x2005, 10) == StopReason::Address && machine.Get(Register::Rax) == 77 && calls == 1,
            "Address reuse retained a removed host gate or stale translated code");
    for (std::uint64_t iteration = 0; iteration < 512; ++iteration) {
        machine.Map(0x7000, 4096, rw);
        write(machine, 0x7000, iteration);
        machine.Set(Register::Rdi, 0x7000);
        require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == iteration,
                "Repeated allocation reuse leaked backing capacity or executed stale memory");
        machine.Unmap(0x7000, 4096);
    }
    failure([&] { machine.CheckAccess(0x7000, 1, Permission::Read); }, "Guest access denied at 0x7000");
    inventory(machine.Mappings(), {{0x1000,8192,rx,false}, {0x3000,4096,Permission::Read,false}, {0x4000,8192,rw,false}});
}
void replacementRanges() {
#if ANYPS5_CPU_MODERN_TCG
    alignas(16384) std::array<std::byte,16384> original{};
    alignas(16384) std::array<std::byte,16384> fresh{};
#endif
    alignas(16384) std::array<std::byte,16384> replacement{};
    Machine machine;
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 16384, rw);
    write(machine, 0x4000, std::uint64_t{111});
    code(machine, 0x1000, {0x48,0x8b,0x07});
    machine.Set(Register::Rdi, 0x4000);
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 111,
            "Original range did not execute its memory read before replacement");
#if ANYPS5_CPU_MODERN_TCG
    machine.Unmap(0x3000, 16384);
    machine.MapBorrowed(0x3000, original, rw);
    machine.MapBorrowed(0x8000, std::span(original).subspan(4096, 4096), rw);
    machine.MapBorrowed(0x10000, replacement, rw);
    write(machine, 0x4000, std::uint64_t{111});
    write(machine, 0x11000, std::uint64_t{222});
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 111,
            "Original borrowed alias did not warm its memory translation");
    machine.ReplaceBorrowed(0x4000, std::span(replacement).subspan(4096, 4096), rw);
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 222,
            "Partial replacement retained its old physical backing or stale memory translation");
    machine.Set(Register::Rdi, 0x8000);
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 111,
            "Partial replacement changed a retained peer of the old backing");
    code(machine, 0x1020, {0x48,0x89,0x07});
    machine.Set(Register::Rdi, 0x4000);
    machine.Set(Register::Rax, 333);
    require(machine.Run(0x1020, 0x1023, 10) == StopReason::Address && read<std::uint64_t>(machine, 0x11000) == 333 &&
            read<std::uint64_t>(machine, 0x8000) == 111, "Replacement writes did not cohere only with their new backing");
    machine.Unmap(0x3000, 16384);
    require(read<std::uint64_t>(machine, 0x8000) == 111 && read<std::uint64_t>(machine, 0x11000) == 333,
            "Unmapping mixed fragments released an allocation still used by another guest alias");
    failure([&] { machine.ReplaceBorrowed(0x8000, replacement, rw); }, "Guest access denied at 0x9000");
    failure([&] { machine.ReplaceBorrowed(0x8000, std::span(replacement).subspan(1, 4096), rw); }, "4 KiB aligned offset");
    std::array<std::byte,4096> unknownAllocation{};
    failure([&] { machine.ReplaceBorrowed(0x8000, unknownAllocation, rw); }, "requires a complete aligned host-page backing");
    require(read<std::uint64_t>(machine, 0x8000) == 111, "Rejected replacement changed existing storage");
    machine.Map(0x2000, 4096, rx);
    code(machine, 0x2000, {0xb8,1,0,0,0});
    require(machine.Run(0x2000, 0x2005, 10) == StopReason::Address && machine.Get(Register::Rax) == 1,
            "Original executable range did not warm its translation");
    unsigned calls = 0;
    machine.AddHostCall(0x2000, [&](Machine& guest) { ++calls; guest.Exit(7); });
    require(machine.Run(0x2000, 0, 10) == StopReason::Exit && calls == 1, "Original replacement gate did not execute");
    constexpr std::array<std::uint8_t,5> newCode{0xb8,9,0,0,0};
    std::copy(newCode.begin(), newCode.end(), reinterpret_cast<std::uint8_t*>(replacement.data()));
    machine.ReplaceBorrowed(0x2000, std::span(replacement).first(4096), rx);
    inventory(machine.Mappings(), {{0x1000,4096,rx,false}, {0x2000,4096,rx,true},
              {0x8000,4096,rw,true}, {0x10000,16384,rw,true}});
    failure([&] { machine.CheckAccess(0x2000, 1, Permission::Write); }, "Guest access denied");
    require(machine.Run(0x2000, 0x2005, 10) == StopReason::Address && machine.Get(Register::Rax) == 9 && calls == 1,
            "Executable replacement retained a host gate, old code or old permissions");
    replacement[1] = std::byte{19};
    require(machine.Run(0x2000, 0x2005, 10) == StopReason::Address && machine.Get(Register::Rax) == 19,
            "Replaced borrowed code stopped observing host changes");
    fresh[0] = std::byte{0xf5}; fresh[1] = std::byte{1};
    machine.ReplaceBorrowed(0x10000, fresh, rw);
    machine.Set(Register::Rdi, 0x10000);
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 501,
            "Replacement did not register a new complete borrowed allocation");
    require(machine.Run(0x2000, 0x2005, 10) == StopReason::Address && machine.Get(Register::Rax) == 19,
            "Full replacement released old storage still used by a retained executable alias");
#else
    failure([&] { machine.ReplaceBorrowed(0x3000, replacement, rw); }, "Unicorn guest borrowed range replacement is unsupported");
    require(machine.Run(0x1000, 0x1003, 10) == StopReason::Address && machine.Get(Register::Rax) == 111,
            "Unsupported replacement changed the original Unicorn guest mapping");
#endif
    machine.MapBorrowed(0x20000, replacement, rw);
    machine.Protect(0x21000, 4096, static_cast<Permission>(0));
    machine.Unmap(0x22000, 4096);
#if ANYPS5_CPU_MODERN_TCG
    inventory(machine.Mappings(), {{0x1000,4096,rx,false}, {0x2000,4096,rx,true}, {0x8000,4096,rw,true},
              {0x10000,16384,rw,true}, {0x20000,4096,rw,true}, {0x21000,4096,static_cast<Permission>(0),true}, {0x23000,4096,rw,true}});
#else
    inventory(machine.Mappings(), {{0x1000,4096,rx,false}, {0x3000,16384,rw,false},
              {0x20000,4096,rw,true}, {0x21000,4096,static_cast<Permission>(0),true}, {0x23000,4096,rw,true}});
#endif
}
}
int main(int argc, const char** argv) {
    try {
        const std::pair<const char*,void(*)()> tests[] = {
            {"widths/flags", widthsAndFlags}, {"calls/branches/memory", callsBranchesAndMemory},
            {"TLS/SSE", tlsAndSse},
#if ANYPS5_CPU_MODERN_TCG
            {"unsupported EVEX", unsupportedVectorEncodings},
            {"AVX/AVX2/F16C/FMA", modernVectors}, {"CPUID/XGETBV", modernCpuFeatures},
            {"invalid instruction diagnostics", invalidInstructionDiagnostics},
#else
            {"unsupported VEX/EVEX", unsupportedVectorEncodings},
#endif
            {"unsupported XOP", unsupportedXopEncoding}, {"syscall/import ABI", syscallAndHostAbi},
            {"borrowed memory", borrowedMemory}, {"errors/budget", errorsAndBudget},
            {"unmap ranges", unmapRanges}, {"replacement ranges", replacementRanges}
        };
        bool selected = false;
        for (const auto& [name, test] : tests) {
            if (argc > 1 && std::string(argv[1]) != name) continue;
            selected = true;
            test();
            std::cout << "PASS " << name << '\n';
        }
        require(selected, "Unknown CPU test group");
        std::cout << "CPU backend: " << Machine::Backend() << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
