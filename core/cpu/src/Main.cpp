#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/GuestThreads.hpp>
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
#include <cpu/SceThreadImports.hpp>
#include <cpu/SceAudioOut2Imports.hpp>
#include <cpu/Self.hpp>
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
#include <cpu/NativeModuleRunner.hpp>
#include <mach-o/dyld.h>
#endif
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
std::filesystem::path NativeUtilityMetallib() {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (!size) throw std::runtime_error("Cannot obtain native executable path");
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size))
        throw std::runtime_error("Cannot obtain native executable path");
    const auto executable = std::filesystem::canonical(path.data());
    const auto utility = executable.parent_path().parent_path() / "fixtures" / "AnyPS5Utilities.metallib";
    if (!std::filesystem::is_regular_file(utility))
        throw std::runtime_error("Native module runner missing package utility metallib: " + utility.string());
    return utility;
}
Cpu::NativeServiceConsumerProfile NativeServiceProfile(std::string_view hash, std::string_view size) {
    Cpu::NativeServiceConsumerProfile profile;
    if (hash.size() != 64) throw std::runtime_error("Native service public profile requires a SHA256 and positive byte size");
    const auto hex = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("Native service public profile requires a hexadecimal SHA256");
    };
    for (unsigned i = 0; i < 32; ++i)
        profile.SourceSha256[i] = static_cast<std::byte>((hex(hash[i * 2]) << 4) | hex(hash[i * 2 + 1]));
    const auto parsed = std::from_chars(size.data(), size.data() + size.size(), profile.SourceSize);
    if (parsed.ec != std::errc{} || parsed.ptr != size.data() + size.size() || !profile.SourceSize ||
        profile.SourceSha256 == std::array<std::byte, 32>{})
        throw std::runtime_error("Native service public profile requires a SHA256 and positive byte size");
    return profile;
}
#endif
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

// Game runs are unbounded. Each limit is an opt-in diagnostic cap where 0
// (the default) means unlimited.
struct RunLimits {
    std::optional<std::uint64_t> MaxInstructions, MaxInitInstructions;
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
    std::optional<std::uint64_t> MaxWallMs, MaxIdleMs;
#endif
};

void ParseLimit(std::optional<std::uint64_t>& limit, int argc, char** argv, int index,
                std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
    const std::string option(argv[index]);
    if (limit) throw std::runtime_error(option + " may be supplied only once");
    if (argc <= index + 1) throw std::runtime_error(option + " requires a non-negative integer (0 = unlimited)");
    const std::string_view text(argv[index + 1]);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value > maximum)
        throw std::runtime_error(option + " requires a non-negative integer (0 = unlimited)");
    limit = value;
}

std::uint64_t InstructionBudget(const std::optional<std::uint64_t>& limit) {
    return limit && *limit ? *limit : Cpu::UnboundedInstructionBudget;
}

void Capabilities(const RunLimits& limits) {
    std::cout << "{\"schema_version\":1,\"host_architecture\":" << Json(HostArchitecture())
        << ",\"guest_architecture\":\"x86_64\",\"backend\":" << Json(Cpu::Machine::Backend())
        << ",\"supported_formats\":[\"static_elf64_x86_64\",\"sce_elf64_x86_64\"],\"runtime_abi\":\"linux_sysv\",\"runtime_abis\":[\"linux_sysv\",\"sce_sysv\"],\"services\":["
        << "{\"name\":\"write\",\"number\":1,\"constraints\":\"stdout/stderr only; at most 16 MiB per call\"},"
        << "{\"name\":\"exit\",\"number\":60,\"constraints\":\"status truncated to 8 bits\"},"
        << "{\"name\":\"exit_group\",\"number\":231,\"constraints\":\"single guest thread; status truncated to 8 bits\"},"
        << "{\"name\":\"arch_prctl\",\"number\":158,\"constraints\":\"ARCH_SET_FS and ARCH_GET_FS only\"}],"
        << "\"sce_imports\":{\"module\":\"libc\",\"module_version\":\"1.1\",\"library\":\"libc\",\"library_version\":1,"
        << "\"functions\":[\"memcpy\",\"memmove\",\"memset\",\"strlen\",\"strcmp\",\"exit\"]},"
        << "\"execution_limits\":{\"max_instructions\":" << limits.MaxInstructions.value_or(0)
        << ",\"max_init_instructions\":" << limits.MaxInitInstructions.value_or(0) << "},"
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
        << "\"sce_net_address_imports\":{\"module\":\"libSceNet\",\"module_version\":\"1.1\",\"library_version\":1,\"functions\":[\"sceNetHtonl\",\"sceNetHtons\",\"sceNetInetNtop\",\"sceNetInetPton\"],\"constraints\":\"local IPv4 conversion only; malformed text returns 0 without writing output; unsupported family/insufficient capacity fails explicitly; no socket, resolver or guest errno services\"},"
        << "\"sce_libc_bootstrap_imports\":{\"function_nids\":[\"959qrazPIrg\",\"p5EcQeEeJAE\",\"NWtTN10cJzE\"],\"object_nids\":[\"f7uOxY9mM1U\",\"djxxOmW6-aw\"],\"constraints\":\"typed static module graph only; actual mapped process parameters; captures checked heap callbacks; tracing disabled with writable guest storage\"},"
        << "\"supported_containers\":[\"plain_self\"],\"sce_constraints\":[\"no encrypted or compressed SELF segments\",\"static graph TLS; main TLS provider required before dependency TLS\",\"read-only /app0 resources; regular files only\",\"explicit static --sce-module graph only; unknown attributes unsupported\",\"dependency CRT initializers/finalizers only; nonempty arrays require an exact source certificate; main owns its initializer\",\"host object imports limited to checked libc bootstrap storage; no host TLS imports\",\"entry termination callback requires static module graph and defers dependency cleanup outside active CPU execution\"],"
#if ANYPS5_CPU_MODERN_TCG
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
        << "\"native_module_runner\":{\"enabled\":true,\"owned_memory\":\"live staged CPU/Metal publication\",\"provider_selection\":\"actual parsed consumer SHA-256, size, scope and ELF symbol\",\"utility_metallib\":\"../fixtures/AnyPS5Utilities.metallib relative to engine\",\"wall_limit_ms\":"
        << limits.MaxWallMs.value_or(0) << ",\"idle_limit_ms\":" << limits.MaxIdleMs.value_or(0)
        << ",\"constraints\":\"unbounded game profile by default; 0 means unlimited; idle limit bounds one continuous idle stretch; qualified provider subset only; high CPU owned stack/TLS are GPU read-only under written-page ABI; no WebAPI2 provider; no retail gameplay evidence\"},"
#endif
        << "\"sce_thread_imports\":{\"module\":\"libkernel\",\"module_version\":\"1.1\",\"library_version\":1,"
        << "\"functions\":[\"_sceKernelSetThreadDtors\",\"_sceKernelSetThreadAtexitCount\",\"_sceKernelSetThreadAtexitReport\",\"scePthreadCreate\",\"scePthreadYield\",\"scePthreadJoin\",\"scePthreadSelf\",\"scePthreadEqual\",\"__error\",\"__tls_get_addr\",\"scePthreadExit\"],"
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
        << "\"constraints\":\"explicit static module graph only; cooperative guest threads on one owner CPU; optionally bounded execution phases and 4096-instruction slices; at most 256 non-reused thread slots; owned attributes; FIFO/priority inheritance and logical RR with 4096-instruction engineering turns; immutable relocated per-thread TLS, errno and guarded stacks; guest dtors before join completion; count/report registrations retained without unproved invocation; affinity, cancellation, detach, once and TSD unsupported\"},"
#else
        << "\"constraints\":\"explicit static module graph only; cooperative guest threads on one owner CPU; optionally bounded execution phases and 4096-instruction slices; at most 256 non-reused thread slots; nullable default attributes only; immutable relocated per-thread TLS, errno and guarded stacks; guest dtors before join completion; count/report registrations retained without unproved invocation; scheduling policies, affinity, cancellation, detach, once and TSD unsupported\"},"
#endif
        << "\"cpu_profile\":\"Haswell\",\"supported_instruction_families\":[\"AVX\",\"AVX2\",\"F16C\",\"FMA\"],"
        << "\"cpu_constraints\":[\"single guest CPU; owner-thread execution and teardown\",\"borrowed backing must cover complete aligned host pages\",\"shared data pages preserve exact byte permissions; mixed executable permission pages unsupported\"],"
        << "\"unsupported_instruction_families\":[\"AVX-512\",\"XOP\"],\"ps5_game_runtime_ready\":false}\n";
#else
        << "\"cpu_constraints\":[\"mixed permission pages unsupported\"],"
        << "\"unsupported_instruction_families\":[\"AVX\",\"AVX2\",\"AVX-512\",\"XOP\"],\"ps5_game_runtime_ready\":false}\n";
#endif
}

std::vector<Cpu::SceModuleFile> ModuleFiles(const std::filesystem::path& main,
                                             const std::vector<std::filesystem::path>& paths,
                                             std::vector<Cpu::SceParsedImage>* parsedConsumers = nullptr) {
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
        if (parsedConsumers) parsedConsumers->push_back(image);
    }
    return files;
}

std::vector<Cpu::SceHostModule> HostModules(const std::filesystem::path& main,
                                         const std::vector<Cpu::SceModuleFile>& files
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                                         , const Cpu::NativeModuleRunner* native
#endif
                                         ) {
    std::vector<Cpu::SceHostModule> hosts{
        {"libc.prx", {"libc", 0, 1, 1}, {{"libc", 0, 1}}},
        {"libkernel.sprx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}},
        {"libSceUserService.sprx", {"libSceUserService", 0, 1, 1}, {{"libSceUserService", 0, 1}}},
        {"libSceSystemService.sprx", {"libSceSystemService", 0, 1, 1}, {{"libSceSystemService", 0, 1}}},
        {"libSceLibcInternal.prx", {"libSceLibcInternal", 0, 1, 1}, {{"libSceLibcInternalExt", 0, 1}, {"libSceLibcInternal", 0, 1}}},
        {"libSceAudioOut.prx", {"libSceAudioOut", 0, 1, 1}, {{"libSceAudioOut2", 0, 1}}},
        {"libSceNpManager.prx", {"libSceNpManager", 0, 1, 1}, {{"libSceNpManager", 0, 1}}},
        {"libSceNet.prx", {"libSceNet", 0, 1, 1}, {{"libSceNet", 0, 1}}},
        {"libSceCommonDialog.prx", {"libSceCommonDialog", 0, 1, 1}, {{"libSceCommonDialog", 0, 1}}}};
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
    if (native) native->AddHostModules(hosts);
#endif
    bool kernelPrx = false, kernelSprx = false;
    const auto neededKernel = [&](const Cpu::SceParsedImage& image) {
        for (const auto& needed : image.NeededFiles) {
            if (needed == "libkernel.prx") kernelPrx = true;
            else if (needed == "libkernel.sprx") kernelSprx = true;
        }
    };
    neededKernel(Cpu::ParseSce(main));
    for (const auto& file : files) {
        const auto image = Cpu::ParseSce(file.Path);
        neededKernel(image);
        std::erase_if(hosts, [&](const auto& host) {
            return std::any_of(image.ExportModules.begin(), image.ExportModules.end(), [&](const auto& module) {
                return host.Module.Name == module.Name && host.Module.Major == module.Major && host.Module.Minor == module.Minor;
            });
        });
    }
    for (auto& host : hosts) if (host.Module.Name == "libkernel") {
        if (kernelPrx && kernelSprx)
            throw std::runtime_error("Unsupported SCE libkernel host filename aliases: graph requires both libkernel.prx and libkernel.sprx");
        if (kernelPrx) host.Filename = "libkernel.prx";
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
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
        std::optional<Cpu::NativeServiceConsumerProfile> publicNpIdentity, publicUriEscape;
#endif
        bool capabilities = false;
        RunLimits limits;
        while (argc > first) {
            const std::string_view option(argv[first]);
            if (option == "--capabilities-json") {
                if (capabilities) throw std::runtime_error("--capabilities-json may be supplied only once");
                capabilities = true;
                ++first;
            } else if (option == "--max-instructions") {
                ParseLimit(limits.MaxInstructions, argc, argv, first);
                first += 2;
            } else if (option == "--max-init-instructions") {
                ParseLimit(limits.MaxInitInstructions, argc, argv, first);
                first += 2;
            } else if (option == "--diagnostics-json") {
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
            }
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
            else if (option == "--max-wall-ms" || option == "--max-idle-ms") {
                // Limits are compared against steady_clock nanoseconds; larger values overflow there.
                ParseLimit(option == "--max-wall-ms" ? limits.MaxWallMs : limits.MaxIdleMs, argc, argv, first,
                           static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::duration::max()).count()));
                first += 2;
            } else if (option == "--native-service-public-profile") {
                if (argc <= first + 3)
                    throw std::runtime_error("--native-service-public-profile requires np-identity|http-uri SHA256 SIZE");
                const std::string_view service(argv[first + 1]);
                if (service != "np-identity" && service != "http-uri")
                    throw std::runtime_error("Unsupported native service public profile");
                auto& profile = service == "np-identity" ? publicNpIdentity : publicUriEscape;
                if (profile) throw std::runtime_error("Native service public profile may be supplied only once per service");
                profile = NativeServiceProfile(argv[first + 2], argv[first + 3]);
                first += 4;
            }
#endif
            else if (option == "--resource-root") {
                if (argc <= first + 1 || std::string_view(argv[first + 1]).empty() || std::string_view(argv[first + 1]).starts_with('-'))
                    throw std::runtime_error("--resource-root requires a directory path");
                if (!resourceRoot.empty()) throw std::runtime_error("--resource-root may be supplied only once");
                resourceRoot = argv[first + 1];
                first += 2;
            } else break;
        }
        if (capabilities) {
            if (argc != first || diagnostics || inspect || !modulePaths.empty() || !resourceRoot.empty()
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                || publicNpIdentity || publicUriEscape
#endif
                ) throw std::runtime_error("--capabilities-json accepts only run limit options");
            Capabilities(limits);
            return 0;
        }
        if (argc <= first)
            throw std::runtime_error("Usage: anyps5_cpu_run [--diagnostics-json] [--max-instructions N] [--max-init-instructions N] <x86-64.elf> [guest arguments...] or --inspect-sce-json <clean-sce.elf>");
        if (std::string_view(argv[first]).starts_with('-'))
            throw std::runtime_error("Unsupported CLI option: " + std::string(argv[first]));
        executable = argv[first];
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
        if ((publicNpIdentity || publicUriEscape) && (inspect || modulePaths.empty()))
            throw std::runtime_error("Native service public profiles require a native SCE module graph");
        if ((limits.MaxWallMs || limits.MaxIdleMs) && (inspect || modulePaths.empty()))
            throw std::runtime_error("Native runner wall and idle limits require a native SCE module graph");
#endif
        if ((limits.MaxInstructions || limits.MaxInitInstructions) && inspect)
            throw std::runtime_error("Run limits cannot be combined with --inspect-sce-json");
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
        std::shared_ptr<Cpu::GuestThreads> threadRuntime;
        std::unique_ptr<Cpu::SceThreadImports> threadImports;
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
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
        // Declared last so graphics drains before all external provider owners
        // on loader, initializer and entry exceptions as well as normal exit.
        std::unique_ptr<Cpu::NativeModuleRunner> nativeRuntime;
#endif
        std::uint64_t entry;
        const bool sce = SceExecutable(executable);
        constexpr std::uint32_t sessionUserId = 0x10000000;
        try {
            std::vector<std::string> arguments;
            for (int index = first; index < argc; ++index) arguments.emplace_back(argv[index]);
            if (sce) {
#if ANYPS5_CPU_MODERN_TCG
                if (!modulePaths.empty()) {
                    threadRuntime = std::make_shared<Cpu::GuestThreads>(machine);
#if !ANYPS5_CPU_NATIVE_MODULE_RUNNER
                    threadImports = std::make_unique<Cpu::SceThreadImports>(machine, threadRuntime);
#endif
                }
#endif
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                if (threadRuntime) {
                    const auto actualMain = Cpu::ParseSce(executable);
                    Cpu::NativeModuleRunnerConfiguration nativeConfig;
                    nativeConfig.UtilityMetallib = NativeUtilityMetallib();
                    nativeConfig.SessionUserId = sessionUserId;
                    nativeConfig.EnableQualifiedServiceConsumers = true;
                    nativeConfig.PublicNpIdentity = publicNpIdentity;
                    nativeConfig.PublicUriEscape = publicUriEscape;
                    nativeConfig.MaximumWallTime = std::chrono::milliseconds(limits.MaxWallMs.value_or(0));
                    nativeConfig.MaximumIdleWait = std::chrono::milliseconds(limits.MaxIdleMs.value_or(0));
                    nativeRuntime = std::make_unique<Cpu::NativeModuleRunner>(machine, threadRuntime,
                        Cpu::SceImportConsumer{actualMain.Path, actualMain.SourceSize, actualMain.SourceSha256},
                        std::move(nativeConfig));
                    nativeRuntime->RegisterParsedConsumer(actualMain);
                    memoryRuntime = nativeRuntime->Memory();
                } else
#endif
                    memoryRuntime = std::make_shared<Cpu::GuestMemoryRuntime>(machine, 12ULL << 30);
                memoryImports = std::make_unique<Cpu::SceMemoryImports>(machine, memoryRuntime);
                sceRuntime = std::make_unique<Cpu::SceImports>(machine);
                lifecycleRuntime = std::make_unique<Cpu::SceLifecycleImports>(machine);
                kernelRuntime = std::make_unique<Cpu::SceKernelImports>(machine, resourceRoot.empty() ? std::filesystem::current_path() : resourceRoot);
                userRuntime = std::make_unique<Cpu::SceUserImports>(machine);
                npRuntime = std::make_unique<Cpu::SceNpLocalImports>(machine, sessionUserId);
                netAddressRuntime = std::make_unique<Cpu::SceNetAddressImports>(machine);
                commonDialogRuntime = std::make_unique<Cpu::SceCommonDialogImports>(machine, 0x7ffdf2000000);
                systemRuntime = std::make_unique<Cpu::SceSystemImports>(machine);
                audioRuntime = std::make_unique<Cpu::SceAudioOut2Imports>(machine);
                bootstrapRuntime = std::make_unique<Cpu::SceLibcBootstrapImports>(machine, std::filesystem::path(executable).filename().string());
                const auto resolve = [&](const auto& import) {
                    if (threadImports) if (const auto gate = threadImports->Resolve(import)) return *gate;
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
#if ANYPS5_CPU_MODERN_TCG
                    const auto processExit = [owner = std::weak_ptr<Cpu::GuestThreads>(threadRuntime)](int status) {
                        const auto runtime = owner.lock();
                        if (!runtime) throw std::runtime_error("SCE process exit guest thread runtime has expired");
                        runtime->ProcessExitFromHostCall(status);
                    };
                    sceRuntime->SetProcessExitHandler(processExit);
                    lifecycleRuntime->SetProcessExitHandler(processExit);
#endif
                    std::vector<Cpu::SceParsedImage> parsedConsumers;
                    const auto files = ModuleFiles(executable, modulePaths,
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                        nativeRuntime ? &parsedConsumers : nullptr
#else
                        nullptr
#endif
                        );
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                    if (nativeRuntime) for (const auto& parsed : parsedConsumers)
                        nativeRuntime->RegisterParsedConsumer(parsed);
#endif
                    const auto hosts = HostModules(executable, files
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                        , nativeRuntime.get()
#endif
                        );
                    std::optional<Cpu::SceLibcInternalProvider> libcInternal;
                    for (const auto& file : files) if (file.Path.filename() == "libc.prx" && file.Crt)
                        libcInternal = Cpu::SceLibcInternalProvider{"libc.prx", file.Crt->SourceSha256, file.Crt->SourceSize};
                    modules = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{executable, 0x1000000}, files, hosts,
                        Cpu::SceModuleResolver{}, libcInternal,
                        [&](const Cpu::SceImportConsumer& consumer, const auto& import,
                            std::uint8_t type, std::uint64_t size) -> std::optional<Cpu::SceResolvedImport> {
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                            if (nativeRuntime) if (const auto selected = nativeRuntime->Resolve(consumer, import, type, size))
                                return selected;
#endif
                            if (threadImports) if (const auto address = threadImports->Resolve(import, type)) {
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                                if (size) throw std::runtime_error("Unsupported SCE thread function symbol size");
#endif
                                return Cpu::SceResolvedImport{*address, type};
                            }
                            if (const auto address = bootstrapRuntime->Resolve(import)) {
                                const std::uint8_t expectedType = import.Nid == "f7uOxY9mM1U" || import.Nid == "djxxOmW6-aw" ? 1 : 2;
                                if (type != expectedType)
                                    throw std::runtime_error("Unsupported SCE libc bootstrap symbol type");
                                return Cpu::SceResolvedImport{*address, expectedType, expectedType == 1 ? 8u : 0u};
                            }
                            if (type != 2) return std::nullopt;
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                            if (size) return std::nullopt;
#endif
                            return Cpu::SceResolvedImport{resolve(import), 2};
                        });
                    kernelRuntime->SetTls(modules->Tls());
                    bootstrapRuntime->SetProcessParameters(modules->Main().ProcParam ? modules->Main().ProcParam->Address : 0,
                        modules->Main().ProcParam ? modules->Main().ProcParam->FileSize : 0);
                    Cpu::SetupSceEntry(machine, modules->Main(), arguments, modules->EntryTerminationGate());
                    if (threadRuntime) {
                        auto factory = modules->ThreadTlsFactory();
                        if (!factory) throw std::runtime_error("Unsupported SCE guest threads: frozen TLS factory is not configured");
                        threadRuntime->AdoptInitial({modules->Main().Entry, modules->InitialStack(), modules->Tls(), std::move(factory)});
                        modules->SetExecutor(threadRuntime->ModuleExecutor());
                    }
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
                    if (nativeRuntime) nativeRuntime->ActivateBeforeInitializers();
#endif
                    try { modules->InitializeDependencies(0, 0, 0, InstructionBudget(limits.MaxInitInstructions)); }
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
        try {
            const auto entryBudget = InstructionBudget(limits.MaxInstructions);
            reason = modules ? modules->RunMain(entryBudget, InstructionBudget(limits.MaxInitInstructions))
                             : machine.Run(entry, 0, entryBudget);
#if ANYPS5_CPU_NATIVE_MODULE_RUNNER
            if (nativeRuntime) nativeRuntime->Shutdown();
            else
#endif
                if (threadRuntime) threadRuntime->Withdraw();
        }
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
