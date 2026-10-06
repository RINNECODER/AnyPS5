#include <cpu/Cpu.hpp>
#include <cpu/Runtime.hpp>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
using Cpu::StopReason;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr auto rw = Permission::Read | Permission::Write;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void service(Machine& machine, std::uint64_t number, std::uint64_t arg0,
             std::uint64_t arg1 = 0, std::uint64_t arg2 = 0) {
    std::vector<std::uint8_t> bytes;
    const auto immediate = [&](std::uint8_t opcode, std::uint64_t value) {
        bytes.push_back(0x48);
        bytes.push_back(opcode);
        for (unsigned shift = 0; shift < 64; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    immediate(0xb8, number);
    immediate(0xbf, arg0);
    immediate(0xbe, arg1);
    immediate(0xba, arg2);
    bytes.insert(bytes.end(), {0x0f, 0x05, 0x0f, 0x0b});
    machine.Write(0x1000, std::as_bytes(std::span(bytes)));
}

StopReason run(Machine& machine) {
    return machine.Run(0x1000, 0x102a, 20);
}

void rejected(Machine& machine, const char* expected) {
    try {
        run(machine);
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos,
                error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing runtime rejection: ") + expected);
}

void exits() {
    struct Case { std::uint64_t number; std::uint64_t argument; int expected; };
    constexpr std::array cases{
        Case{60, 0x123, 35},
        Case{231, 0xff, 255},
        Case{231, 0xffffffffffffffff, 255},
    };
    for (const auto& test : cases) {
        Machine machine;
        Cpu::LinuxRuntime runtime(machine);
        machine.Map(0x1000, 4096, rx);
        service(machine, test.number, test.argument);
        require(machine.Run(0x1000, 0x102c, 20) == StopReason::Exit,
                "Linux exit service did not stop execution");
        require(machine.ExitCode() == test.expected, "Linux exit code did not preserve the low eight bits");
    }
}

void fsRoundTrip() {
    Machine machine;
    Cpu::LinuxRuntime runtime(machine);
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 4096, rw);
    constexpr std::uint64_t expected = 0x1234567890;
    service(machine, 158, 0x1002, expected);
    require(run(machine) == StopReason::Address && machine.Get(Register::Rax) == 0,
            "Linux SET_FS did not return success");
    require(machine.Get(Register::FsBase) == expected, "Linux SET_FS truncated the TLS base");
    service(machine, 158, 0x1003, 0x3008);
    require(run(machine) == StopReason::Address && machine.Get(Register::Rax) == 0,
            "Linux GET_FS did not return success");
    std::uint64_t actual{};
    machine.Read(0x3008, std::as_writable_bytes(std::span(&actual, 1)));
    require(actual == expected, "Linux GET_FS did not write the persistent 64-bit TLS base");
}

void writeWouldBlock() {
    struct Descriptors {
        int Read = -1;
        int Write = -1;
        int SavedStdout = -1;
        ~Descriptors() {
            if (SavedStdout != -1) {
                ::dup2(SavedStdout, STDOUT_FILENO);
                ::close(SavedStdout);
            }
            if (Read != -1) ::close(Read);
            if (Write != -1) ::close(Write);
        }
    } descriptors;
    int pipeDescriptors[2];
    require(::pipe(pipeDescriptors) == 0, "Cannot create nonblocking write regression pipe");
    descriptors.Read = pipeDescriptors[0];
    descriptors.Write = pipeDescriptors[1];
    const int flags = ::fcntl(descriptors.Write, F_GETFL);
    require(flags != -1 && ::fcntl(descriptors.Write, F_SETFL, flags | O_NONBLOCK) == 0,
            "Cannot make write regression pipe nonblocking");
    require(::fcntl(descriptors.Write, F_SETNOSIGPIPE, 1) == 0,
            "Cannot establish host-owned safe nonblocking pipe output");
    std::array<std::byte, 4096> padding{};
    std::size_t size = padding.size();
    std::size_t filled = 0;
    while (true) {
        const auto written = ::write(descriptors.Write, padding.data(), size);
        if (written > 0) {
            filled += static_cast<std::size_t>(written);
            require(filled <= 8 * 1024 * 1024, "Write regression pipe did not reach a finite capacity");
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            require(written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
                    "Write regression pipe failed before reaching capacity");
            if (size == 1) break;
            size = 1;
        }
    }
    std::cout.flush();
    descriptors.SavedStdout = ::dup(STDOUT_FILENO);
    require(descriptors.SavedStdout != -1 && ::dup2(descriptors.Write, STDOUT_FILENO) != -1,
            "Cannot install inherited nonblocking stdout");
    Machine machine;
    Cpu::LinuxRuntime runtime(machine);
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 4096, rw);
    service(machine, 1, 1, 0x3000, 1);
    require(run(machine) == StopReason::Address, "Linux write EAGAIN unexpectedly stopped execution");
    require(machine.Get(Register::Rax) == 0xfffffffffffffff5ULL,
            "Linux write must return guest EAGAIN -11 for a full inherited nonblocking pipe");
}

void fsRange() {
    Machine machine;
    Cpu::LinuxRuntime runtime(machine);
    machine.Map(0x1000, 4096, rx);
    constexpr std::uint64_t previous = 0x7fffffffefff;
    service(machine, 158, 0x1002, previous);
    require(run(machine) == StopReason::Address && machine.Get(Register::Rax) == 0 &&
            machine.Get(Register::FsBase) == previous,
            "Linux SET_FS rejected the last byte below the four-level user address limit");
    for (const std::uint64_t invalid : {0x7ffffffff000ULL, 0x800000000000ULL,
                                       0xffff800000000000ULL, 0xffffffffffffffffULL}) {
        service(machine, 158, 0x1002, invalid);
        require(run(machine) == StopReason::Address, "Linux rejected SET_FS unexpectedly stopped execution");
        require(machine.Get(Register::Rax) == 0xffffffffffffffffULL,
                "Linux SET_FS must return guest EPERM -1 for bases at or above the user address limit");
        require(machine.Get(Register::FsBase) == previous,
                "Rejected Linux SET_FS changed the previous TLS base");
    }
}

void writeBrokenPipeCase(int noSigpipe) {
    int descriptors[2];
    require(::pipe(descriptors) == 0, "Cannot create broken-pipe regression pipe");
    ::close(descriptors[0]);
    const auto child = ::fork();
    if (child == 0) {
        try {
            require(::signal(SIGPIPE, SIG_DFL) != SIG_ERR, "Cannot restore default SIGPIPE in regression child");
            require(::dup2(descriptors[1], STDOUT_FILENO) != -1, "Cannot install broken-pipe child stdout");
            if (descriptors[1] != STDOUT_FILENO) ::close(descriptors[1]);
            require(::fcntl(STDOUT_FILENO, F_SETNOSIGPIPE, noSigpipe) == 0,
                    "Cannot establish original descriptor SIGPIPE behavior");
            const auto original = ::fcntl(STDOUT_FILENO, F_GETNOSIGPIPE);
            require(original == noSigpipe, "Original broken-pipe stdout did not retain configured SIGPIPE behavior");
            sigset_t originalMask;
            require(::pthread_sigmask(SIG_SETMASK, nullptr, &originalMask) == 0,
                    "Cannot inspect original host signal mask");
            Machine machine;
            Cpu::LinuxRuntime runtime(machine);
            machine.Map(0x1000, 4096, rx);
            machine.Map(0x3000, 4096, rw);
            service(machine, 1, 1, 0x3000, 1);
            if (noSigpipe == 0) rejected(machine, "Linux write: unsafe host output");
            else require(run(machine) == StopReason::Address && machine.Get(Register::Rax) == 0xffffffffffffffe0ULL,
                         "Linux write must return guest EPIPE -32 for a host-configured safe pipe with no reader");
            require(::fcntl(STDOUT_FILENO, F_GETNOSIGPIPE) == original,
                    "Linux write changed the original host descriptor SIGPIPE behavior");
            struct sigaction disposition{};
            require(::sigaction(SIGPIPE, nullptr, &disposition) == 0 && disposition.sa_handler == SIG_DFL,
                    "Linux write changed the original host SIGPIPE disposition");
            sigset_t finalMask;
            require(::pthread_sigmask(SIG_SETMASK, nullptr, &finalMask) == 0,
                    "Cannot inspect final host signal mask");
            for (int signal = 1; signal < NSIG; ++signal)
                require(sigismember(&originalMask, signal) == sigismember(&finalMask, signal),
                        "Linux write changed the original host signal mask");
            ::_exit(0);
        } catch (const std::exception& error) {
            std::cerr << "FAIL broken-pipe child: " << error.what() << '\n';
            ::_exit(1);
        }
    }
    ::close(descriptors[1]);
    require(child != -1, "Cannot fork native broken-pipe regression child");
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child, &status, 0); } while (result == -1 && errno == EINTR);
    require(result == child, "Cannot wait for broken-pipe regression child");
    if (WIFSIGNALED(status))
        throw std::runtime_error("Linux write terminated the native host child with signal " + std::to_string(WTERMSIG(status)));
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Broken-pipe child did not preserve host isolation, guest EPIPE, and original descriptor flags");
}

void writeBrokenPipe() {
    for (const int noSigpipe : {0, 1}) writeBrokenPipeCase(noSigpipe);
}

void unsupportedServices() {
    Machine machine;
    Cpu::LinuxRuntime runtime(machine);
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 4096, rw);
    service(machine, 99999, 0);
    rejected(machine, "Unsupported Linux guest syscall 99999");
    service(machine, 158, 0x1001, 0x3000);
    rejected(machine, "Linux arch_prctl: unsupported operation 4097");
    for (const std::uint64_t descriptor : {0ULL, 3ULL, 0xffffffffffffffffULL}) {
        service(machine, 1, descriptor, 0x3000, 1);
        rejected(machine, "Linux write: only stdout and stderr are supported");
    }
}

void rejectedGuestBuffers() {
    Machine machine;
    Cpu::LinuxRuntime runtime(machine);
    machine.Map(0x1000, 4096, rx);
    machine.Map(0x3000, 4096, Permission::Write);
    const std::uint64_t sentinel = 0x9988776655443322;
    machine.Write(0x3000, std::as_bytes(std::span(&sentinel, 1)));
    service(machine, 1, 1, 0x3000, 1);
    rejected(machine, "Guest access denied at 0x3000");

    machine.Protect(0x3000, 4096, Permission::Read);
    machine.Set(Register::FsBase, 0x1234567890);
    service(machine, 158, 0x1003, 0x3000);
    rejected(machine, "Guest access denied at 0x3000");
    std::uint64_t actual{};
    machine.Read(0x3000, std::as_writable_bytes(std::span(&actual, 1)));
    require(actual == sentinel, "Rejected GET_FS modified a read-only guest destination");

    service(machine, 1, 1, 0x9000, 16 * 1024 * 1024 + 1);
    rejected(machine, "Linux write: guest buffer exceeds 16 MiB limit");
}
}

int main() {
    try {
        exits();
        fsRoundTrip();
        unsupportedServices();
        rejectedGuestBuffers();
        std::string regressionFailures;
        for (auto regression : {writeWouldBlock, fsRange, writeBrokenPipe}) {
            try { regression(); }
            catch (const std::exception& error) { regressionFailures += std::string(error.what()) + '\n'; }
        }
        if (!regressionFailures.empty()) throw std::runtime_error(regressionFailures);
        std::cout << "PASS Linux runtime exit variants, TLS round trip and range, Linux write errno, broken-pipe host isolation, unsupported services, and guest buffer rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
