#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/Runtime.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceKernelImports.hpp>
#include <cpu/SceNpLocalImports.hpp>
#include <cpu/SceNetAddressImports.hpp>
#include <cpu/SceCommonDialogImports.hpp>
#include <cpu/SceLibcBootstrapImports.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceMemoryImports.hpp>
#include <cpu/SceModules.hpp>
#include <cpu/SceUserImports.hpp>
#include <cpu/SceSystemImports.hpp>
#include <cpu/SceAudioOut2Imports.hpp>
#include <cpu/Self.hpp>
#include <array>
#include <csignal>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>
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
        << "\"sce_module_argument\":\"--sce-module\",\"resource_root_argument\":\"--resource-root\",\"sce_kernel_imports\":{\"module\":\"libkernel\",\"module_version\":\"1.1\",\"library\":\"libkernel\",\"library_version\":1,\"functions\":[\"sceKernelOpen\",\"sceKernelRead\",\"sceKernelPread\",\"sceKernelLseek\",\"sceKernelClose\",\"__tls_get_addr\"]},"
        << "\"sce_lifecycle_imports\":{\"module\":\"libkernel\",\"library_version\":1,\"module_version\":\"1.1\",\"functions\":[\"_exit\"],\"constraints\":\"nonreturning process exit; low 32-bit status truncated to 8 bits; guest libc owns atexit\"},"
        << "\"sce_memory_imports\":{\"module\":\"libkernel\",\"module_version\":\"1.1\",\"library_version\":1,"
        << "\"functions\":[\"sceKernelGetDirectMemorySize\",\"sceKernelAvailableDirectMemorySize\",\"sceKernelAllocateDirectMemory\",\"sceKernelAllocateMainDirectMemory\",\"sceKernelMapDirectMemory\",\"sceKernelMapFlexibleMemory\",\"sceKernelReserveVirtualRange\",\"sceKernelMprotect\",\"sceKernelVirtualQuery\",\"sceKernelMunmap\",\"sceKernelReleaseDirectMemory\"],"
        << "\"constraints\":\"virtual 12 GiB direct address capacity; demand-backed 16 KiB extents; direct types 0/12; protection mask 0x37 with raw query metadata; fixed non-overwriting maps 0x90; unsupported flags fail explicitly\"},"
        << "\"sce_user_imports\":{\"module\":\"libSceUserService\",\"module_version\":\"1.1\",\"library_version\":1,"
        << "\"functions\":[\"sceUserServiceInitialize\",\"sceUserServiceGetInitialUser\",\"sceUserServiceGetLoginUserIdList\",\"sceUserServiceGetUserName\"],\"constraints\":\"session-local guest profile; no network account services\"},"
        << "\"sce_system_imports\":{\"module\":\"libSceSystemService\",\"module_version\":\"1.1\",\"library_version\":1,"
        << "\"functions\":[\"sceSystemServiceParamGetInt\",\"sceSystemServiceParamGetString\",\"sceSystemServiceHideSplashScreen\"],"
        << "\"recognized_unavailable\":[\"sceSystemServiceGetStatus\",\"sceSystemServiceReceiveEvent\",\"sceSystemServiceGetHdrToneMapLuminance\",\"sceSystemServiceLaunchPlayerDialog\"],"
        << "\"constraints\":\"virtual console settings: English US, UTC, no summertime, AnyPS5 name; unavailable calls return signed 0x80a10002 without touching outputs; player dialog initializer unsupported\"},"
        << "\"sce_common_dialog_imports\":{\"module\":\"libSceCommonDialog\",\"module_version\":\"1.1\",\"library_version\":1,\"functions\":[\"sceCommonDialogInitialize\"],\"constraints\":\"session initializer only; repeated initialization returns signed 0x80b80002; dialog operations unsupported\"},"
        << "\"sce_np_local_imports\":{\"module\":\"libSceNpManager\",\"module_version\":\"1.1\",\"library_version\":1,\"functions\":[\"sceNpGetState\"],\"constraints\":\"session-local user and offline state only; no network account or authentication services\"},"
        << "\"sce_net_address_imports\":{\"module\":\"libSceNet\",\"module_version\":\"1.1\",\"library_version\":1,\"functions\":[\"sceNetHtonl\",\"sceNetHtons\",\"sceNetInetNtop\",\"sceNetInetPton\"],\"constraints\":\"local IPv4 conversion only; unsupported family/text/capacity fails; no socket, resolver or guest errno services\"},"
        << "\"sce_libc_bootstrap_imports\":{\"function_nids\":[\"959qrazPIrg\",\"p5EcQeEeJAE\",\"NWtTN10cJzE\"],\"object_nids\":[\"f7uOxY9mM1U\",\"djxxOmW6-aw\"],\"constraints\":\"typed static module graph only; actual mapped process parameters; captures checked heap callbacks; tracing disabled with writable guest storage\"},"
        << "\"supported_containers\":[\"plain_self\"],\"sce_constraints\":[\"no encrypted or compressed SELF segments\",\"static graph TLS; main TLS provider required before dependency TLS\",\"read-only /app0 resources; regular files only\",\"explicit static --sce-module graph only; unknown attributes and shared permission pages unsupported\",\"dependency CRT initializers/finalizers only; nonempty arrays require an exact source certificate; main owns its initializer\",\"host object imports limited to checked libc bootstrap storage; no host TLS imports\",\"entry termination callback requires static module graph and defers dependency cleanup outside active CPU execution\"],"
#if ANYPS5_CPU_MODERN_TCG
        << "\"cpu_profile\":\"Haswell\",\"supported_instruction_families\":[\"AVX\",\"AVX2\",\"F16C\",\"FMA\"],"
        << "\"cpu_constraints\":[\"single guest CPU; owner-thread execution and teardown\",\"borrowed backing must cover complete aligned host pages\"],"
        << "\"unsupported_instruction_families\":[\"AVX-512\",\"XOP\"],\"ps5_game_runtime_ready\":false}\n";
#else
        << "\"unsupported_instruction_families\":[\"AVX\",\"AVX2\",\"AVX-512\",\"XOP\"],\"ps5_game_runtime_ready\":false}\n";
#endif
}

std::vector<Cpu::SceModuleFile> ModuleFiles(const std::filesystem::path& main,
                                             const std::vector<std::filesystem::path>& paths) {
    constexpr std::uint64_t ceiling = 0x7ffdf0000000;
    std::uint64_t next = 0x1000000;
    const auto extent = [&](const Cpu::SceParsedImage& image, std::uint64_t bias) {
        auto end = bias;
        for (const auto& segment : image.Segments) {
            if ((segment.Type != 1 && segment.Type != 0x61000010) || !segment.MemorySize) continue;
            if (segment.Address >= ceiling || segment.MemorySize > ceiling - segment.Address ||
                bias > ceiling - segment.Address - segment.MemorySize)
                throw std::runtime_error("SCE module placement exceeds the supported guest address range");
            end = std::max(end, bias + segment.Address + segment.MemorySize);
        }
        return end;
    };
    next = extent(Cpu::ParseSce(main), next);
    std::vector<Cpu::SceModuleFile> files;
    for (const auto& path : paths) {
        const auto image = Cpu::ParseSce(path);
        std::uint64_t alignment = 4096;
        for (const auto& segment : image.Segments) {
            if ((segment.Type != 1 && segment.Type != 0x61000010) || !segment.MemorySize) continue;
            if (segment.Alignment > 1 && (segment.Alignment & (segment.Alignment - 1)))
                throw std::runtime_error("SCE module placement requires power-of-two segment alignment");
            alignment = std::max(alignment, segment.Alignment);
        }
        if (alignment >= ceiling || next > ceiling - alignment)
            throw std::runtime_error("SCE module placement alignment exceeds the supported guest address range");
        const auto bias = (next + alignment - 1) & ~(alignment - 1);
        Cpu::SceModuleFile file{path, bias};
        constexpr std::array<std::byte, 32> libcSource{
            std::byte{0x78}, std::byte{0xa0}, std::byte{0x80}, std::byte{0xfd}, std::byte{0xec}, std::byte{0xcc}, std::byte{0x28}, std::byte{0xf2},
            std::byte{0xaa}, std::byte{0x76}, std::byte{0x35}, std::byte{0x6e}, std::byte{0x97}, std::byte{0xf8}, std::byte{0x2a}, std::byte{0x35},
            std::byte{0xb3}, std::byte{0xba}, std::byte{0x09}, std::byte{0xde}, std::byte{0xba}, std::byte{0x84}, std::byte{0x08}, std::byte{0xdf},
            std::byte{0xce}, std::byte{0x27}, std::byte{0xdb}, std::byte{0x28}, std::byte{0xfa}, std::byte{0x0c}, std::byte{0xe6}, std::byte{0x7f}};
        if (image.SourceSize == 1875018 && image.SourceSha256 == libcSource)
            file.Crt = Cpu::SceCrtCertificate{libcSource, 1875018, 0x10, 0x114cb0,
                {0x192818, 8, Cpu::SceCrtArrayOwner::DtInit}, {}, {}};
        files.push_back(std::move(file));
        next = extent(image, bias);
    }
    return files;
}

std::vector<Cpu::SceHostModule> HostModules(const std::vector<Cpu::SceModuleFile>& files) {
    std::vector<Cpu::SceHostModule> hosts{
        {"libc.prx", {"libc", 0, 1, 1}, {{"libc", 0, 1}}},
        {"libkernel.sprx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}},
        {"libSceUserService.sprx", {"libSceUserService", 0, 1, 1}, {{"libSceUserService", 0, 1}}},
        {"libSceSystemService.sprx", {"libSceSystemService", 0, 1, 1}, {{"libSceSystemService", 0, 1}}},
        {"libSceLibcInternal.prx", {"libSceLibcInternal", 0, 1, 1}, {{"libSceLibcInternalExt", 0, 1}}},
        {"libSceAudioOut.prx", {"libSceAudioOut", 0, 1, 1}, {{"libSceAudioOut2", 0, 1}}},
        {"libSceNpManager.prx", {"libSceNpManager", 0, 1, 1}, {{"libSceNpManager", 0, 1}}},
        {"libSceNet.prx", {"libSceNet", 0, 1, 1}, {{"libSceNet", 0, 1}}},
        {"libSceCommonDialog.prx", {"libSceCommonDialog", 0, 1, 1}, {{"libSceCommonDialog", 0, 1}}}};
    for (const auto& file : files) {
        const auto image = Cpu::ParseSce(file.Path);
        std::erase_if(hosts, [&](const auto& host) {
            return std::any_of(image.ExportModules.begin(), image.ExportModules.end(), [&](const auto& module) {
                return host.Module.Name == module.Name && host.Module.Major == module.Major && host.Module.Minor == module.Minor;
            });
        });
    }
    return hosts;
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
    std::cout << "],\"export_modules\":[";
    for (std::size_t index = 0; index < image.ExportModules.size(); ++index) {
        if (index) std::cout << ',';
        const auto& module = image.ExportModules[index];
        std::cout << "{\"name\":" << Json(module.Name) << ",\"id\":" << module.Id
            << ",\"major\":" << unsigned(module.Major) << ",\"minor\":" << unsigned(module.Minor) << '}';
    }
    std::cout << "],\"export_libraries\":[";
    for (std::size_t index = 0; index < image.ExportLibraries.size(); ++index) {
        if (index) std::cout << ',';
        const auto& library = image.ExportLibraries[index];
        std::cout << "{\"name\":" << Json(library.Name) << ",\"id\":" << library.Id << ",\"version\":" << library.Version << '}';
    }
    std::cout << "],\"export_library_attributes\":[";
    for (std::size_t index = 0; index < image.ExportLibraryAttributes.size(); ++index) {
        if (index) std::cout << ',';
        const auto& attribute = image.ExportLibraryAttributes[index];
        std::cout << "{\"library_id\":" << attribute.LibraryId << ",\"attributes\":" << attribute.Attributes << '}';
    }
    std::cout << "],\"exports\":[";
    for (std::size_t index = 0; index < image.Exports.size(); ++index) {
        if (index) std::cout << ',';
        const auto& symbol = image.Exports[index];
        const auto& identity = symbol.Identity;
        std::cout << "{\"nid\":" << Json(identity.Nid) << ",\"library\":" << Json(identity.LibraryName)
            << ",\"library_id\":" << identity.LibraryId << ",\"library_version\":" << identity.LibraryVersion
            << ",\"module\":" << Json(identity.ModuleName) << ",\"module_id\":" << identity.ModuleId
            << ",\"module_major\":" << unsigned(identity.ModuleMajor) << ",\"module_minor\":" << unsigned(identity.ModuleMinor)
            << ",\"symbol_index\":" << symbol.SymbolIndex << ",\"value\":" << symbol.Value << ",\"size\":" << symbol.Size
            << ",\"section\":" << symbol.Section << ",\"type\":" << unsigned(symbol.Type)
            << ",\"binding\":" << unsigned(symbol.Binding) << ",\"visibility\":" << unsigned(symbol.Visibility) << '}';
    }
    std::cout << "],\"original_filename\":" << (image.OriginalFilename ? Json(*image.OriginalFilename) : "null");
    std::cout << ",\"needed_modules\":["; strings(image.NeededModules);
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
        message.starts_with("Unsupported guest /app0 ") || message.starts_with("Unsupported guest memory ") ||
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
        std::vector<std::filesystem::path> modulePaths;
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
            } else if (option == "--sce-module") {
                if (argc <= first + 1 || std::string_view(argv[first + 1]).empty() || std::string_view(argv[first + 1]).starts_with('-'))
                    throw std::runtime_error("--sce-module requires a module path");
                modulePaths.emplace_back(argv[first + 1]);
                first += 2;
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
            if (!modulePaths.empty()) throw std::runtime_error("--sce-module cannot be combined with --inspect-sce-json");
            if (argc != first + 1) throw std::runtime_error("--inspect-sce-json accepts exactly one executable");
            try { InspectSce(Cpu::ParseSce(executable)); }
            catch (const std::exception& error) { code = LoaderCode(error.what()); throw; }
            return 0;
        }
        code = ErrorCode::HostFailure;
        if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
            throw std::runtime_error("Cannot configure standalone CLI SIGPIPE handling");
        Cpu::Machine machine;
        std::shared_ptr<Cpu::GuestMemoryRuntime> memoryRuntime;
        std::unique_ptr<Cpu::SceMemoryImports> memoryImports;
        std::unique_ptr<Cpu::LinuxRuntime> linuxRuntime;
        std::unique_ptr<Cpu::SceImports> sceRuntime;
        std::unique_ptr<Cpu::SceKernelImports> kernelRuntime;
        std::unique_ptr<Cpu::SceUserImports> userRuntime;
        std::unique_ptr<Cpu::SceNpLocalImports> npRuntime;
        std::unique_ptr<Cpu::SceNetAddressImports> netAddressRuntime;
        std::unique_ptr<Cpu::SceCommonDialogImports> commonDialogRuntime;
        std::unique_ptr<Cpu::SceModules> modules;
        std::unique_ptr<Cpu::SceSystemImports> systemRuntime;
        std::unique_ptr<Cpu::SceAudioOut2Imports> audioRuntime;
        std::unique_ptr<Cpu::SceLibcBootstrapImports> bootstrapRuntime;
        std::unique_ptr<Cpu::SceLifecycleImports> lifecycleRuntime;
        std::uint64_t entry;
        const bool sce = SceExecutable(executable);
        try {
            std::vector<std::string> arguments;
            for (int index = first; index < argc; ++index) arguments.emplace_back(argv[index]);
            if (sce) {
                memoryRuntime = std::make_shared<Cpu::GuestMemoryRuntime>(machine, 12ULL << 30);
                memoryImports = std::make_unique<Cpu::SceMemoryImports>(machine, memoryRuntime);
                sceRuntime = std::make_unique<Cpu::SceImports>(machine);
                lifecycleRuntime = std::make_unique<Cpu::SceLifecycleImports>(machine);
                kernelRuntime = std::make_unique<Cpu::SceKernelImports>(machine, resourceRoot.empty() ? std::filesystem::current_path() : resourceRoot);
                userRuntime = std::make_unique<Cpu::SceUserImports>(machine);
                npRuntime = std::make_unique<Cpu::SceNpLocalImports>(machine);
                netAddressRuntime = std::make_unique<Cpu::SceNetAddressImports>(machine);
                commonDialogRuntime = std::make_unique<Cpu::SceCommonDialogImports>(machine, 0x7ffdf2000000);
                systemRuntime = std::make_unique<Cpu::SceSystemImports>(machine);
                audioRuntime = std::make_unique<Cpu::SceAudioOut2Imports>(machine);
                bootstrapRuntime = std::make_unique<Cpu::SceLibcBootstrapImports>(machine, std::filesystem::path(executable).filename().string());
                const auto resolve = [&](const auto& import) {
                    if (const auto gate = lifecycleRuntime->Resolve(import)) return *gate;
                    if (const auto gate = memoryImports->Resolve(import)) return *gate;
                    if (const auto gate = commonDialogRuntime->Resolve(import)) return *gate;
                    if (const auto gate = npRuntime->Resolve(import)) return *gate;
                    if (const auto gate = netAddressRuntime->Resolve(import)) return *gate;
                    if (const auto gate = audioRuntime->Resolve(import)) return *gate;
                    if (const auto gate = systemRuntime->Resolve(import)) return *gate;
                    if (const auto gate = userRuntime->Resolve(import)) return *gate;
                    if (import.ModuleName == "libkernel" || import.LibraryName == "libkernel") return kernelRuntime->Resolve(import);
                    return sceRuntime->Resolve(import);
                };
                if (modulePaths.empty()) {
                    auto image = Cpu::LoadSce(machine, executable, 0x1000000, resolve);
                    bootstrapRuntime->SetProcessParameters(image.ProcParam ? image.ProcParam->Address : 0,
                        image.ProcParam ? image.ProcParam->FileSize : 0);
                    kernelRuntime->SetTls(image.Tls);
                    Cpu::SetupSceEntry(machine, image, arguments, sceRuntime->ExitGate());
                    entry = image.Entry;
                } else {
                    const auto files = ModuleFiles(executable, modulePaths);
                    const auto hosts = HostModules(files);
                    modules = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{executable, 0x1000000}, files, hosts,
                        [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                            if (const auto address = bootstrapRuntime->Resolve(import)) {
                                const std::uint8_t expectedType = import.Nid == "f7uOxY9mM1U" || import.Nid == "djxxOmW6-aw" ? 1 : 2;
                                if (type != expectedType)
                                    throw std::runtime_error("Unsupported SCE libc bootstrap symbol type");
                                return Cpu::SceResolvedImport{*address, expectedType, expectedType == 1 ? 8u : 0u};
                            }
                            if (type != 2) return std::nullopt;
                            return Cpu::SceResolvedImport{resolve(import), 2};
                        });
                    kernelRuntime->SetTls(modules->Tls());
                    bootstrapRuntime->SetProcessParameters(modules->Main().ProcParam ? modules->Main().ProcParam->Address : 0,
                        modules->Main().ProcParam ? modules->Main().ProcParam->FileSize : 0);
                    Cpu::SetupSceEntry(machine, modules->Main(), arguments, modules->EntryTerminationGate());
                    try { modules->InitializeDependencies(); }
                    catch (const std::exception& error) { code = ExecutionCode(error.what()); throw; }
                    entry = modules->Main().Entry;
                }
            } else {
                if (!modulePaths.empty()) {
                    code = ErrorCode::InvalidArguments;
                    throw std::runtime_error("--sce-module requires an SCE executable");
                }
                linuxRuntime = std::make_unique<Cpu::LinuxRuntime>(machine);
                auto image = Cpu::Load(machine, executable);
                Cpu::SetupStack(machine, image, arguments);
                entry = image.Entry;
            }
        } catch (const std::exception& error) {
            if (code == ErrorCode::HostFailure) code = LoaderCode(error.what());
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
        try { reason = modules ? modules->RunMain() : machine.Run(entry, 0, 100000000); }
        catch (const std::exception& error) {
            code = ExecutionCode(error.what());
            throw;
        }
        if (reason != Cpu::StopReason::Exit) {
            code = reason == Cpu::StopReason::InstructionLimit ? ErrorCode::ExecutionLimit : ErrorCode::ExecutionFailure;
            throw std::runtime_error("Guest did not exit: CPU execution budget or stop reached");
        }
        const auto exitCode = machine.ExitCode();
        if (diagnostics)
            std::cerr << "{\"schema_version\":1,\"event\":\"guest_exit\",\"exit_code\":" << exitCode << "}\n";
        else std::cerr << "guest_exit=" << exitCode << '\n';
        return exitCode;
    } catch (const std::exception& error) {
        if (diagnostics) {
            std::cerr << "{\"schema_version\":1,\"event\":\"error\",\"code\":" << Json(Code(code))
                << ",\"message\":" << Json(error.what()) << ",\"executable\":" << Json(executable)
                << ",\"process_exit\":" << LaunchFailure << "}\n";
        } else std::cerr << "anyps5_cpu_run: " << error.what() << '\n';
        return LaunchFailure;
    }
}
