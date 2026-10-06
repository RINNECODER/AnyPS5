#include <cpu/Runtime.hpp>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace Cpu {
namespace {

int writeErrno(int hostError) {
    switch (hostError) {
    case EINTR: return 4;
    case EIO: return 5;
    case ENXIO: return 6;
    case EBADF: return 9;
    case EAGAIN: return 11;
    case EFAULT: return 14;
    case EINVAL: return 22;
    case EFBIG: return 27;
    case ENOSPC: return 28;
    case EPIPE: return 32;
    case EDEADLK: return 35;
    case EDESTADDRREQ: return 89;
    case ENETDOWN: return 100;
    case ENETUNREACH: return 101;
    case ECONNRESET: return 104;
    case ENOBUFS: return 105;
    case EDQUOT: return 122;
    default: throw std::runtime_error("Linux write: unsupported host errno " + std::to_string(hostError));
    }
}

void checkOutput(int descriptor) {
    struct stat attributes{};
    if (::fstat(descriptor, &attributes) != 0)
        throw std::runtime_error("Linux write: cannot inspect host output descriptor, errno " + std::to_string(errno));
    if (!S_ISFIFO(attributes.st_mode) && !S_ISSOCK(attributes.st_mode)) return;
    struct sigaction disposition{};
    if (::sigaction(SIGPIPE, nullptr, &disposition) != 0)
        throw std::runtime_error("Linux write: cannot inspect host SIGPIPE disposition, errno " + std::to_string(errno));
    if (disposition.sa_handler == SIG_IGN) return;
    const int noSigpipe = ::fcntl(descriptor, F_GETNOSIGPIPE);
    if (noSigpipe < 0)
        throw std::runtime_error("Linux write: cannot inspect host no-SIGPIPE output setting, errno " + std::to_string(errno));
    if (noSigpipe == 0)
        throw std::runtime_error("Linux write: unsafe host output requires host SIGPIPE ignored or F_SETNOSIGPIPE enabled");
}

}

LinuxRuntime::LinuxRuntime(Machine& machine) {
    machine.SetSyscallHandler(syscall);
}

void LinuxRuntime::syscall(Machine& machine) {
    const auto number = machine.Get(Register::Rax);
    const auto arg0 = machine.Get(Register::Rdi);
    const auto arg1 = machine.Get(Register::Rsi);
    const auto arg2 = machine.Get(Register::Rdx);
    switch (number) {
    case 1: {
        if (arg0 != STDOUT_FILENO && arg0 != STDERR_FILENO) throw std::runtime_error("Linux write: only stdout and stderr are supported");
        if (arg2 > 16 * 1024 * 1024) throw std::runtime_error("Linux write: guest buffer exceeds 16 MiB limit");
        std::vector<std::byte> bytes(static_cast<std::size_t>(arg2));
        machine.Read(arg1, bytes);
        checkOutput(static_cast<int>(arg0));
        ssize_t result;
        do { result = ::write(static_cast<int>(arg0), bytes.data(), bytes.size()); } while (result < 0 && errno == EINTR);
        machine.Set(Register::Rax, result < 0 ? static_cast<std::uint64_t>(-writeErrno(errno)) : static_cast<std::uint64_t>(result));
        break;
    }
    case 60:
    case 231:
        machine.Exit(static_cast<int>(arg0 & 255));
        break;
    case 158:
        if (arg0 == 0x1002) {
            if (arg1 >= 0x7ffffffff000) {
                machine.Set(Register::Rax, 0xffffffffffffffffULL);
                break;
            }
            machine.Set(Register::FsBase, arg1);
        }
        else if (arg0 == 0x1003) {
            const auto value = machine.Get(Register::FsBase);
            machine.CheckAccess(arg1, sizeof(value), Permission::Write);
            machine.Write(arg1, std::as_bytes(std::span(&value, 1)));
        } else throw std::runtime_error("Linux arch_prctl: unsupported operation " + std::to_string(arg0));
        machine.Set(Register::Rax, 0);
        break;
    default:
        throw std::runtime_error("Unsupported Linux guest syscall " + std::to_string(number));
    }
}

}
