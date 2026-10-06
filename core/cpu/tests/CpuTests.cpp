#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
using Cpu::StopReason;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
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
    Machine machine;
    machine.Map(0x1000, 4096, rx);
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
}
int main(int argc, const char** argv) {
    try {
        const std::pair<const char*,void(*)()> tests[] = {
            {"widths/flags", widthsAndFlags}, {"calls/branches/memory", callsBranchesAndMemory},
            {"TLS/SSE", tlsAndSse}, {"syscall/import ABI", syscallAndHostAbi},
            {"borrowed memory", borrowedMemory}, {"errors/budget", errorsAndBudget}
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
