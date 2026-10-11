// libkernel time provider without a guest scheduler (single-image runs): import scopes,
// and a host-side sleep that a stop request ends early. The scheduler path, where a
// sleep parks only its guest thread, is covered end to end by KernelTimeCliTest.py.
#include <cpu/SceKernelTimeImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Cpu::Permission;
using Cpu::Register;
using namespace std::chrono;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Cpu::SceImport import(const char* nid, const char* library = "libkernel") {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = library;
    value.ModuleName = "libkernel";
    value.LibraryId = 4;
    value.ModuleId = 9;
    value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::atomic<bool> stop{false};
    Cpu::SceKernelTimeImports imports{machine, nullptr, [this] { return stop.load(); }};
    Session() {
        machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
        machine.Map(0x2000, 4096, Permission::Read | Permission::Write);
        machine.Map(0x4000, 4096, Permission::Read | Permission::Write);
        // call [rip -> 0x2000]; mov [rip -> 0x2010], rax; inc rbx; nop
        constexpr std::array<std::uint8_t, 17> program{
            0xff, 0x15, 0xfa, 0x0f, 0x00, 0x00, 0x48, 0x89, 0x05, 0x03, 0x10, 0x00, 0x00, 0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(program)));
    }
    std::uint64_t call(const Cpu::SceImport& target, std::uint64_t first, std::uint64_t second = 0) {
        const auto gate = imports.Resolve(target);
        require(gate.has_value(), "Kernel time import was not resolved");
        machine.Write(0x2000, std::as_bytes(std::span(&*gate, 1)));
        machine.Set(Register::Rdi, first);
        machine.Set(Register::Rsi, second);
        machine.Set(Register::Rsp, 0x4ff0);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "Kernel time gate did not return to its caller");
        return machine.Get(Register::Rax);
    }
};

void scopes() {
    Session session;
    // clock_gettime is exported by both libraries of the libkernel module; the sce* entry
    // points only by libkernel.
    require(session.imports.Resolve(import("lLMT9vJAck0", "libScePosix")).has_value() &&
            session.imports.Resolve(import("lLMT9vJAck0")).has_value(), "POSIX clock_gettime scope rejected");
    bool rejected = false;
    try { session.imports.Resolve(import("QBi7HCK03hw", "libScePosix")); }
    catch (const std::exception& error) { rejected = std::string(error.what()).find("scope/version") != std::string::npos; }
    require(rejected, "sceKernelClockGettime accepted the libScePosix library scope");
    require(!session.imports.Resolve(import("AAAAAAAAAAA")).has_value(), "Unknown NID was claimed by the time provider");
}

void hostSleep() {
    Session session;
    // sceKernelNanosleep({0, 20 ms}, &remaining) sleeps on the host and clears remaining.
    const std::array<std::int64_t, 4> request{0, 20000000, 7, 7};
    session.machine.Write(0x2100, std::as_bytes(std::span(request)));
    auto started = steady_clock::now();
    require(session.call(import("QvsZxomvUHs"), 0x2100, 0x2110) == 0, "sceKernelNanosleep failed");
    require(steady_clock::now() - started >= 20ms, "sceKernelNanosleep returned early");
    std::array<std::int64_t, 2> remaining{};
    session.machine.Read(0x2110, std::as_writable_bytes(std::span(remaining)));
    require(remaining == std::array<std::int64_t, 2>{0, 0}, "sceKernelNanosleep left remaining time");

    // A stop request made while the guest sleeps ends an hour-long sleep promptly, reported as
    // interrupted (signed SCE EINTR) with the unslept time left in remaining.
    const std::array<std::int64_t, 2> hour{3600, 0};
    session.machine.Write(0x2100, std::as_bytes(std::span(hour)));
    started = steady_clock::now();
    std::thread requester([&] { std::this_thread::sleep_for(100ms); session.stop = true; });
    const auto result = session.call(import("QvsZxomvUHs"), 0x2100, 0x2110);
    requester.join();
    const auto elapsed = steady_clock::now() - started;
    require(result == 0xffffffff80020004ull, "An interrupted host sleep did not report SCE EINTR");
    require(elapsed >= 100ms && elapsed < 2s, "A stop request did not end a host sleep");
    session.machine.Read(0x2110, std::as_writable_bytes(std::span(remaining)));
    require(remaining[0] >= 3590 && remaining[0] < 3600 && remaining[1] >= 0 && remaining[1] < 1000000000,
            "An interrupted host sleep did not report its unslept time");
    session.stop = false;
    started = steady_clock::now();
    require(session.call(import("-ZR+hG7aDHw"), 0) == 0 && session.call(import("1jfXLRVzisc"), 1000) == 0 &&
            steady_clock::now() - started >= 1ms, "sceKernelSleep(0)/sceKernelUsleep(1000) failed");
}

}

int main() {
    try {
        scopes();
        hostSleep();
        std::cout << "PASS kernel time scopes and stop-aware host sleep\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
