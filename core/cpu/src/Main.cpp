#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/Runtime.hpp>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr int LaunchFailure = 126;
enum class ErrorCode {
    InvalidArguments, InputUnavailable, UnsupportedExecutable, LoaderFailure,
    UnsupportedInstruction, UnsupportedService, GuestMemoryFault,
    ExecutionLimit, ExecutionFailure, HostFailure
};

const char* Code(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidArguments: return "invalid_arguments";
    case ErrorCode::InputUnavailable: return "input_unavailable";
    case ErrorCode::UnsupportedExecutable: return "unsupported_executable";
    case ErrorCode::LoaderFailure: return "loader_failure";
    case ErrorCode::UnsupportedInstruction: return "unsupported_instruction";
    case ErrorCode::UnsupportedService: return "unsupported_service";
    case ErrorCode::GuestMemoryFault: return "guest_memory_fault";
    case ErrorCode::ExecutionLimit: return "execution_limit";
    case ErrorCode::ExecutionFailure: return "execution_failure";
    case ErrorCode::HostFailure: return "host_failure";
    }
    return "host_failure";
}

const char* HostArchitecture() {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}

std::string Json(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (byte < 0x20) {
                result += "\\u00";
                result += hex[byte >> 4]; result += hex[byte & 15];
            } else result += static_cast<char>(byte);
        }
    }
    result += '"';
    return result;
}

void Capabilities() {
    std::cout << "{\"schema_version\":1,\"host_architecture\":" << Json(HostArchitecture())
        << ",\"guest_architecture\":\"x86_64\",\"backend\":" << Json(Cpu::Machine::Backend())
        << ",\"supported_formats\":[\"static_elf64_x86_64\"],\"runtime_abi\":\"linux_sysv\",\"services\":["
        << "{\"name\":\"write\",\"number\":1,\"constraints\":\"stdout/stderr only; at most 16 MiB per call\"},"
        << "{\"name\":\"exit\",\"number\":60,\"constraints\":\"status truncated to 8 bits\"},"
        << "{\"name\":\"exit_group\",\"number\":231,\"constraints\":\"single guest thread; status truncated to 8 bits\"},"
        << "{\"name\":\"arch_prctl\",\"number\":158,\"constraints\":\"ARCH_SET_FS and ARCH_GET_FS only\"}],"
        << "\"ps5_game_runtime_ready\":false}\n";
}

ErrorCode LoaderCode(std::string_view message) {
    if (message.starts_with("ELF loader: cannot open ") || message == "ELF loader: cannot read complete executable")
        return ErrorCode::InputUnavailable;
    if (message.find("unsupported") != std::string_view::npos || message.find("requires ") != std::string_view::npos ||
        message == "ELF loader: input is not an ELF executable" ||
        message == "ELF loader: only static ET_EXEC executables are supported")
        return ErrorCode::UnsupportedExecutable;
    return ErrorCode::LoaderFailure;
}

ErrorCode ExecutionCode(std::string_view message) {
    if (message.starts_with("Unsupported guest instruction")) return ErrorCode::UnsupportedInstruction;
    if (message.starts_with("Unsupported Linux guest syscall") || message.starts_with("Unsupported guest syscall") ||
        message.starts_with("Unsupported guest interrupt") || message.starts_with("Unsupported guest SYSENTER") ||
        message.starts_with("Unsupported guest port ") || message.find("unsupported operation") != std::string_view::npos)
        return ErrorCode::UnsupportedService;
    if (message.starts_with("Guest unmapped ") || message.starts_with("Guest protected ") ||
        message.starts_with("Guest access denied ") || message == "Guest access range overflows")
        return ErrorCode::GuestMemoryFault;
    return ErrorCode::ExecutionFailure;
}
}

int main(int argc, char** argv) {
    bool diagnostics = false;
    std::string executable;
    auto code = ErrorCode::InvalidArguments;
    try {
        int first = 1;
        if (argc > 1 && std::string_view(argv[1]) == "--capabilities-json") {
            if (argc != 2) throw std::runtime_error("--capabilities-json does not accept executable arguments");
            Capabilities();
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--diagnostics-json") {
            diagnostics = true;
            first = 2;
        }
        if (argc <= first)
            throw std::runtime_error("Usage: anyps5_cpu_run <static-x86-64.elf> [guest arguments...]");
        if (std::string_view(argv[first]).starts_with('-'))
            throw std::runtime_error("Unsupported CLI option: " + std::string(argv[first]));
        executable = argv[first];
        code = ErrorCode::HostFailure;
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Cannot configure standalone CLI SIGPIPE handling");
        Cpu::Machine machine;
        Cpu::LinuxRuntime runtime(machine);
        Cpu::LoadedImage image;
        try {
            image = Cpu::Load(machine, executable);
            std::vector<std::string> arguments;
            for (int index = first; index < argc; ++index) arguments.emplace_back(argv[index]);
            Cpu::SetupStack(machine, image, arguments);
        } catch (const std::exception& error) {
            code = LoaderCode(error.what());
            throw;
        }
        if (diagnostics) {
            std::cerr << "{\"schema_version\":1,\"event\":\"startup\",\"executable\":" << Json(executable)
                << ",\"entry\":" << image.Entry << ",\"host_architecture\":" << Json(HostArchitecture())
                << ",\"guest_architecture\":\"x86_64\",\"backend\":" << Json(Cpu::Machine::Backend()) << "}\n";
        } else {
            std::cerr << "host=" << HostArchitecture() << " backend=" << Cpu::Machine::Backend()
                << " guest=x86_64 entry=0x" << std::hex << image.Entry << std::dec << '\n';
        }
        Cpu::StopReason reason;
        try { reason = machine.Run(image.Entry, 0, 100000000); }
        catch (const std::exception& error) {
            code = ExecutionCode(error.what());
            throw;
        }
        if (reason != Cpu::StopReason::Exit) {
            code = reason == Cpu::StopReason::InstructionLimit ? ErrorCode::ExecutionLimit : ErrorCode::ExecutionFailure;
            throw std::runtime_error("Guest did not exit: CPU execution budget or stop reached");
        }
        if (diagnostics)
            std::cerr << "{\"schema_version\":1,\"event\":\"guest_exit\",\"exit_code\":" << machine.ExitCode() << "}\n";
        else std::cerr << "guest_exit=" << machine.ExitCode() << '\n';
        return machine.ExitCode();
    } catch (const std::exception& error) {
        if (diagnostics) {
            std::cerr << "{\"schema_version\":1,\"event\":\"error\",\"code\":" << Json(Code(code))
                << ",\"message\":" << Json(error.what()) << ",\"executable\":" << Json(executable)
                << ",\"process_exit\":" << LaunchFailure << "}\n";
        } else std::cerr << "anyps5_cpu_run: " << error.what() << '\n';
        return LaunchFailure;
    }
}
