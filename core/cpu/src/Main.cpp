#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/Runtime.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceKernelImports.hpp>
#include <cpu/Self.hpp>
#include <array>
#include <csignal>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
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
        << ",\"supported_formats\":[\"static_elf64_x86_64\",\"sce_elf64_x86_64\"],\"runtime_abi\":\"linux_sysv\",\"runtime_abis\":[\"linux_sysv\",\"sce_sysv\"],\"services\":["
        << "{\"name\":\"write\",\"number\":1,\"constraints\":\"stdout/stderr only; at most 16 MiB per call\"},"
        << "{\"name\":\"exit\",\"number\":60,\"constraints\":\"status truncated to 8 bits\"},"
        << "{\"name\":\"exit_group\",\"number\":231,\"constraints\":\"single guest thread; status truncated to 8 bits\"},"
        << "{\"name\":\"arch_prctl\",\"number\":158,\"constraints\":\"ARCH_SET_FS and ARCH_GET_FS only\"}],"
        << "\"sce_imports\":{\"module\":\"libc\",\"module_version\":\"1.1\",\"library\":\"libc\",\"library_version\":1,"
        << "\"functions\":[\"memcpy\",\"memmove\",\"memset\",\"strlen\",\"strcmp\",\"exit\"]},"
        << "\"resource_root_argument\":\"--resource-root\",\"sce_kernel_imports\":{\"module\":\"libkernel\",\"module_version\":\"1.1\",\"library\":\"libkernel\",\"library_version\":1,\"functions\":[\"sceKernelOpen\",\"sceKernelRead\",\"sceKernelPread\",\"sceKernelLseek\",\"sceKernelClose\",\"__tls_get_addr\"]},"
        << "\"supported_containers\":[\"plain_self\"],\"sce_constraints\":[\"no encrypted or compressed SELF segments\",\"main-module TLS only; zero alignment remainder\",\"read-only /app0 resources; regular files only\",\"no guest module loading\",\"no initializers or finalizers\",\"no data imports\",\"entry termination callback unsupported\"],"
        << "\"unsupported_instruction_families\":[\"AVX\",\"AVX2\",\"AVX-512\",\"XOP\"],\"ps5_game_runtime_ready\":false}\n";
}

bool SceExecutable(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 18> bytes{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) return false;
    if (Cpu::IsSelf(std::as_bytes(std::span(bytes)))) return true;
    if (bytes[0] != 0x7f || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F') return false;
    const auto type = bytes[16] | (unsigned(bytes[17]) << 8);
    return type == 0xfe10 || type == 0xfe18 || (type == 3 && (bytes[7] == 9 || bytes[8] == 2));
}

void InspectSce(const Cpu::SceParsedImage& image) {
    std::cout << "{\"schema_version\":1,\"event\":\"inspection\",\"format\":\"sce_elf64_x86_64\",\"executable\":" << Json(image.Path.string())
        << ",\"type\":" << image.Type << ",\"os_abi\":" << unsigned(image.OsAbi) << ",\"abi_version\":" << unsigned(image.AbiVersion)
        << ",\"entry\":" << image.Entry << ",\"container_format\":" << Json(image.SourceContainer) << ",\"segment_count\":" << image.Segments.size() << ",\"relocation_count\":" << image.RelocationCount
        << ",\"has_tls\":" << (image.Tls ? "true" : "false") << ",\"has_process_parameters\":" << (image.ProcParam ? "true" : "false") << ",\"imports\":[";
    for (std::size_t index = 0; index < image.Imports.size(); ++index) {
        if (index) std::cout << ',';
        const auto& import = image.Imports[index];
        std::cout << "{\"nid\":" << Json(import.Nid) << ",\"library\":" << Json(import.LibraryName) << ",\"library_id\":" << import.LibraryId
            << ",\"library_version\":" << import.LibraryVersion << ",\"module\":" << Json(import.ModuleName) << ",\"module_id\":" << import.ModuleId
            << ",\"module_major\":" << unsigned(import.ModuleMajor) << ",\"module_minor\":" << unsigned(import.ModuleMinor) << '}';
    }
    const auto strings = [](const auto& values) {
        for (std::size_t index = 0; index < values.size(); ++index) { if (index) std::cout << ','; std::cout << Json(values[index]); }
    };
    std::cout << "],\"import_library_attributes\":[";
    for (std::size_t index = 0; index < image.ImportLibraryAttributes.size(); ++index) {
        if (index) std::cout << ',';
        const auto& attribute = image.ImportLibraryAttributes[index];
        std::cout << "{\"library_id\":" << attribute.LibraryId << ",\"attributes\":" << attribute.Attributes << '}';
    }
    std::cout << "],\"needed_modules\":["; strings(image.NeededModules);
    std::cout << "],\"needed_files\":["; strings(image.NeededFiles);
    std::cout << "],\"unsupported_reasons\":["; strings(image.UnsupportedReasons);
    std::cout << "],\"normalization_notes\":["; strings(image.ReconstructionNotes);
    std::cout << "],\"relocation_types\":[";
    for (std::size_t index = 0; index < image.RelocationTypes.size(); ++index) { if (index) std::cout << ','; std::cout << image.RelocationTypes[index]; }
    std::cout << "]}\n";
}

ErrorCode LoaderCode(std::string_view message) {
    if (message.starts_with("ELF loader: cannot open ") || message == "ELF loader: cannot read complete executable" ||
        message.starts_with("SCE ELF loader: cannot open ") || message == "SCE ELF loader: cannot read complete executable")
        return ErrorCode::InputUnavailable;
    if (message.find("unsupported") != std::string_view::npos || message.find("Unsupported") != std::string_view::npos || message.find("requires ") != std::string_view::npos ||
        message == "SCE ELF loader: SELF containers require an extracted decrypted ELF" ||
        message == "ELF loader: input is not an ELF executable" ||
        message == "ELF loader: only static ET_EXEC executables are supported")
        return ErrorCode::UnsupportedExecutable;
    return ErrorCode::LoaderFailure;
}

ErrorCode ExecutionCode(std::string_view message) {
    if (message.starts_with("Unsupported guest instruction") || message.starts_with("Unsupported guest VEX/EVEX instruction") ||
        message.starts_with("Unsupported guest XOP instruction")) return ErrorCode::UnsupportedInstruction;
    if (message.starts_with("Unsupported Linux guest syscall") || message.starts_with("Unsupported guest syscall") ||
        message.starts_with("Unsupported guest /app0 ") ||
        message.starts_with("Unsupported guest interrupt") || message.starts_with("Unsupported guest SYSENTER") ||
        message.starts_with("Unsupported guest port ") || message.starts_with("Unsupported guest privileged service instruction") ||
        message.starts_with("Unsupported SCE ") || message.find("unsupported operation") != std::string_view::npos)
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
        bool inspect = false;
        std::filesystem::path resourceRoot;
        if (argc > 1 && std::string_view(argv[1]) == "--capabilities-json") {
            if (argc != 2) throw std::runtime_error("--capabilities-json does not accept executable arguments");
            Capabilities();
            return 0;
        }
        while (argc > first) {
            const std::string_view option(argv[first]);
            if (option == "--diagnostics-json") {
                diagnostics = true;
                ++first;
            } else if (option == "--inspect-sce-json") {
                diagnostics = true;
                inspect = true;
                ++first;
            } else if (option == "--resource-root") {
                if (argc <= first + 1 || std::string_view(argv[first + 1]).empty() || std::string_view(argv[first + 1]).starts_with('-'))
                    throw std::runtime_error("--resource-root requires a directory path");
                if (!resourceRoot.empty()) throw std::runtime_error("--resource-root may be supplied only once");
                resourceRoot = argv[first + 1];
                first += 2;
            } else break;
        }
        if (argc <= first)
            throw std::runtime_error("Usage: anyps5_cpu_run [--diagnostics-json] <x86-64.elf> [guest arguments...] or --inspect-sce-json <clean-sce.elf>");
        if (std::string_view(argv[first]).starts_with('-'))
            throw std::runtime_error("Unsupported CLI option: " + std::string(argv[first]));
        executable = argv[first];
        if (inspect) {
            if (argc != first + 1) throw std::runtime_error("--inspect-sce-json accepts exactly one executable");
            try { InspectSce(Cpu::ParseSce(executable)); }
            catch (const std::exception& error) { code = LoaderCode(error.what()); throw; }
            return 0;
        }
        code = ErrorCode::HostFailure;
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Cannot configure standalone CLI SIGPIPE handling");
        Cpu::Machine machine;
        std::unique_ptr<Cpu::LinuxRuntime> linuxRuntime;
        std::unique_ptr<Cpu::SceImports> sceRuntime;
        std::unique_ptr<Cpu::SceKernelImports> kernelRuntime;
        std::uint64_t entry;
        const bool sce = SceExecutable(executable);
        try {
            std::vector<std::string> arguments;
            for (int index = first; index < argc; ++index) arguments.emplace_back(argv[index]);
            if (sce) {
                sceRuntime = std::make_unique<Cpu::SceImports>(machine);
                kernelRuntime = std::make_unique<Cpu::SceKernelImports>(machine, resourceRoot.empty() ? std::filesystem::current_path() : resourceRoot);
                auto image = Cpu::LoadSce(machine, executable, 0x1000000, [&](const auto& import) {
                    if (import.ModuleName == "libkernel" || import.LibraryName == "libkernel") return kernelRuntime->Resolve(import);
                    return sceRuntime->Resolve(import);
                });
                kernelRuntime->SetTls(image.Tls);
                Cpu::SetupSceEntry(machine, image, arguments, sceRuntime->ExitGate());
                entry = image.Entry;
            } else {
                linuxRuntime = std::make_unique<Cpu::LinuxRuntime>(machine);
                auto image = Cpu::Load(machine, executable);
                Cpu::SetupStack(machine, image, arguments);
                entry = image.Entry;
            }
        } catch (const std::exception& error) {
            code = LoaderCode(error.what());
            throw;
        }
        if (diagnostics) {
            std::cerr << "{\"schema_version\":1,\"event\":\"startup\",\"executable\":" << Json(executable)
                << ",\"entry\":" << entry << ",\"format\":" << Json(sce ? "sce_elf64_x86_64" : "static_elf64_x86_64") << ",\"host_architecture\":" << Json(HostArchitecture())
                << ",\"guest_architecture\":\"x86_64\",\"backend\":" << Json(Cpu::Machine::Backend()) << "}\n";
        } else {
            std::cerr << "host=" << HostArchitecture() << " backend=" << Cpu::Machine::Backend()
                << " guest=x86_64 entry=0x" << std::hex << entry << std::dec << '\n';
        }
        Cpu::StopReason reason;
        try { reason = machine.Run(entry, 0, 100000000); }
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
