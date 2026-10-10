#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include "SceImageData.hpp"
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace Cpu {
namespace {
constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t ReturnGate = 0x7ffdf7000000;
constexpr std::uint64_t TerminationGate = ReturnGate + 16;
constexpr unsigned GraphRequirements = static_cast<unsigned>(SceRequirement::Dependencies) |
    static_cast<unsigned>(SceRequirement::Initializers) | static_cast<unsigned>(SceRequirement::TypedImports) |
    static_cast<unsigned>(SceRequirement::ImportedTls) | static_cast<unsigned>(SceRequirement::Symbolic) |
    static_cast<unsigned>(SceRequirement::StaticTls);

[[noreturn]] void fail(const std::string& reason) { throw std::runtime_error("SCE module graph: " + reason); }

bool sameModule(const SceModuleIdentity& first, const SceModuleIdentity& second) {
    return first.Name == second.Name && first.Major == second.Major && first.Minor == second.Minor;
}

bool sameScope(const SceImport& first, const SceImport& second) {
    return first.Nid == second.Nid && first.LibraryName == second.LibraryName &&
        first.LibraryVersion == second.LibraryVersion && first.ModuleName == second.ModuleName &&
        first.ModuleMajor == second.ModuleMajor && first.ModuleMinor == second.ModuleMinor;
}

bool declares(const SceHostModule& host, const SceImport& import) {
    return host.Module.Name == import.ModuleName && host.Module.Major == import.ModuleMajor &&
        host.Module.Minor == import.ModuleMinor && std::any_of(host.Libraries.begin(), host.Libraries.end(),
            [&](const auto& library) { return library.Name == import.LibraryName && library.Version == import.LibraryVersion; });
}

bool libcInternalFunction(const SceImport& import, std::uint8_t type) {
    // SysV Internal ABI: shadPS4 945dbc3cc3eee80ac3e053b438502ed936fa6bb2,
    // src/core/libraries/libc_internal/{libc_internal_memory,libc_internal_str,libc_internal_io}.cpp;
    // fpPS4 04cefd43e6fddd1ab033e7980cd356d14c964905,
    // src/libcinternal/ps4_libscelibcinternal.pas (explicit printf/snprintf guest forwarding, strstr).
    // CPU06 target-informed extension: exact libc.prx SHA256
    // 78a080fdeccc28f2aa76356e97f82a35b3ba09deba8408dfce27db28fa0ce67f
    // and WebApi SHA256 38db047fd9dfd27fc17dfc0dd2cff31a2e0533ac1be2350e5082f8499f59c6b9.
    // Mspace wrappers 0xf6f0..0xf9eb and paired caller 0x3065/0x334a;
    // cxa registry 0x3abb0..0x3af3c and caller DSO 0x588000;
    // delete/new hooks 0x195080/0x195060; strncat 0x6cc70; _Stoul 0x3ddb0.
    // Termination 0x48d0/0x356c0 retains guest FS+0x28/int45 behavior;
    // exception dispatch and downstream platform services remain prerequisites.
    // These are direct guest routes: no host argument, heap, callback or errno adaptation.
    constexpr std::array nids{"Q3VBxCXhUHs", "8zTFvBIAIN8", "eLdDw6l0-bU", "Ovb2dSJOAuE",
                              "aesyjrHVWy4", "j4ViWNHEgww", "6sJWiWSRuqk", "hcuQgD53UxM", "viiwFMaNamA",
                              "-hn1tcVHq5Q", "W6SiVSiCDtI", "OJjm-QOIHlI", "Vla-Z+eXlxo",
                              "gigoVHZvVPE", "mfHdJTIvhuo", "tsvEmnenz48", "H2e8t5ScQGc",
                              "z+P+xCnWLBk", "kHg45qPC6f0", "zlfEH8FmyUA", "L1SBTkC+Cvw", "zr094EQ39Ww"};
    return type == 2 && import.ModuleName == "libSceLibcInternal" && import.ModuleMajor == 1 &&
        import.ModuleMinor == 1 && import.LibraryName == "libSceLibcInternal" && import.LibraryVersion == 1 &&
        std::find(nids.begin(), nids.end(), import.Nid) != nids.end();
}

bool bareFilename(const std::string& filename) {
    return !filename.empty() && filename != "." && filename != ".." &&
        filename.find_first_of("/\\:\r\n") == std::string::npos;
}

void writeWord(Machine& machine, std::uint64_t address, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(value >> (index * 8));
    machine.Write(address, bytes);
}

void validateCrt(const SceImageData& data, const std::optional<SceCrtCertificate>& certificate, bool isMain) {
    if (!certificate) {
        if (data.PreinitArray.Size || data.InitArray.Size || data.FiniArray.Size)
            fail("unsupported CRT-array ownership for the generic module profile");
        return;
    }
    if (isMain) fail("main executable CRT certification is unsupported");
    const auto& crt = *certificate;
    if (crt.SourceSize != data.SourceSize || crt.SourceSha256 != data.SourceSha256)
        fail("CRT certificate source identity mismatch");
    if (crt.Init != data.Init || crt.Fini != data.Fini)
        fail("CRT certificate layout mismatch");
    const auto array = [&](const SceImageData::Array& actual, const SceCrtArrayContract& expected,
                           SceCrtArrayOwner owner, std::uint64_t entry) {
        if (actual.Address != expected.Address || actual.Size != expected.Size)
            fail("CRT certificate layout mismatch");
        if (actual.Size ? expected.Owner != owner || !entry : expected.Owner != SceCrtArrayOwner::Unsupported)
            fail("CRT certificate array owner mismatch");
    };
    array(data.PreinitArray, crt.Preinit, SceCrtArrayOwner::DtInit, data.Init);
    array(data.InitArray, crt.InitArray, SceCrtArrayOwner::DtInit, data.Init);
    array(data.FiniArray, crt.FiniArray, SceCrtArrayOwner::DtFini, data.Fini);
}
}

GuestPhaseBudget::GuestPhaseBudget(std::uint64_t instructionLimit) : remaining_(instructionLimit) {
    if (!instructionLimit) throw std::invalid_argument("Guest execution phase requires a nonzero instruction budget");
}
std::uint64_t GuestPhaseBudget::Remaining() const noexcept { return remaining_; }
std::uint64_t GuestPhaseBudget::Consumed() const noexcept { return consumed_; }
void GuestPhaseBudget::Charge(std::uint64_t instructions) {
    if (instructions > remaining_ || instructions > UINT64_MAX - consumed_)
        throw std::runtime_error("Guest execution phase exceeded its instruction budget");
    remaining_ -= instructions;
    consumed_ += instructions;
}

struct SceModules::Impl {
    Machine& machine;
    std::vector<SceModuleRecord> modules;
    std::vector<SceHostModule> hosts;
    std::vector<std::set<std::size_t>> edges;
    std::vector<std::size_t> order;
    std::shared_ptr<SceTls> tls;
    struct OwnedTlsTemplate {
        std::uint64_t moduleId;
        std::vector<std::byte> initialBytes;
        std::uint64_t memorySize;
        std::uint64_t alignment;
    };
    std::vector<OwnedTlsTemplate> threadTlsTemplates;
    SceLoadedImage main;
    enum class Phase { Loaded, Initializing, Initialized, Finalizing, Finalized, Failed };
    Phase phase = Phase::Loaded;
    bool mainStarted = false;
    bool mainRunning = false;
    bool terminationRequested = false;
    std::optional<SceModuleExecutor> executor;

    Impl(Machine& guest, const SceModuleFile& executable, std::span<const SceModuleFile> dependencies,
         std::span<const SceHostModule> hostModules, const SceModuleResolver& resolver,
         const std::optional<SceLibcInternalProvider>& libcInternal,
         const SceConsumerModuleResolver& consumerResolver, const SceLazyImports& lazy)
        : machine(guest), hosts(hostModules.begin(), hostModules.end()) {
        if (dependencies.size() > 510 || hosts.size() > 510) fail("module graph exceeds the supported provider count");
        const auto add = [&](const SceModuleFile& file, bool isMain) {
            auto parsed = ParseSce(file.Path);
            if (isMain ? parsed.Type == 0xfe18 : parsed.Type != 0xfe18)
                fail(isMain ? "main image must be an executable" : "dependency image must be an SCE shared module");
            validateCrt(*parsed.Data, file.Crt, isMain);
            RequireSceProfile(parsed, GraphRequirements);
            ValidateSceMapping(parsed, file.LoadBias);
            const auto& data = *parsed.Data;
            modules.push_back({std::move(parsed), file.LoadBias, 0,
                data.Init ? SceAddress(file.LoadBias, data.Init) : 0,
                data.Fini ? SceAddress(file.LoadBias, data.Fini) : 0});
        };
        add(executable, true);
        for (const auto& file : dependencies) add(file, false);
        std::map<std::string, std::size_t> files;
        for (std::size_t index = 0; index < modules.size(); ++index) {
            const auto filename = modules[index].Image.Path.filename().string();
            if (!bareFilename(filename) || !files.emplace(filename, index).second) fail("ambiguous or invalid guest module filename " + filename);
        }
        std::set<std::string> hostFiles;
        for (const auto& host : hosts) {
            if (!bareFilename(host.Filename) || host.Module.Name.empty() || host.Libraries.empty() ||
                !hostFiles.insert(host.Filename).second || files.contains(host.Filename))
                fail("invalid or ambiguous host module declaration " + host.Filename);
        }
        std::optional<std::size_t> libcProvider;
        if (libcInternal) {
            const auto found = files.find(libcInternal->Filename);
            if (!bareFilename(libcInternal->Filename) || !libcInternal->SourceSize ||
                found == files.end() || found->second == 0)
                fail("missing supplied libc Internal source provider");
            const auto& image = modules[found->second].Image;
            if (image.SourceSize != libcInternal->SourceSize || image.SourceSha256 != libcInternal->SourceSha256)
                fail("libc Internal source identity mismatch");
            if (std::count_if(image.ExportModules.begin(), image.ExportModules.end(), [](const auto& module) {
                return module.Name == "libc" && module.Major == 1 && module.Minor == 1;
            }) != 1) fail("libc Internal target module identity mismatch");
            libcProvider = found->second;
        }
        const auto forwarded = [&](const SceImport& import, std::uint8_t type) {
            return libcProvider && libcInternalFunction(import, type) &&
                std::any_of(hosts.begin(), hosts.end(), [&](const auto& host) { return declares(host, import); });
        };
        edges.resize(modules.size());
        std::set<std::string> missingFiles;
        for (std::size_t index = 0; index < modules.size(); ++index) {
            const auto& image = modules[index].Image;
            for (const auto& filename : image.NeededFiles) {
                if (!bareFilename(filename)) fail("DT_NEEDED requires a declared bare filename: " + filename);
                if (const auto found = files.find(filename); found != files.end()) edges[index].insert(found->second);
                else if (!hostFiles.contains(filename)) {
                    if (!lazy.Bind) fail("missing DT_NEEDED provider " + filename);
                    if (lazy.MissingModule && missingFiles.insert(filename).second) lazy.MissingModule(filename);
                }
            }
            for (const auto& needed : image.Data->ImportedModules) {
                std::optional<std::size_t> provider;
                unsigned matches = 0;
                for (std::size_t candidate = 0; candidate < modules.size(); ++candidate)
                    if (std::any_of(modules[candidate].Image.ExportModules.begin(), modules[candidate].Image.ExportModules.end(),
                        [&](const auto& identity) { return sameModule(needed, identity); })) { provider = candidate; ++matches; }
                for (const auto& host : hosts) if (sameModule(needed, host.Module)) ++matches;
                if (matches > 1 || (!matches && !lazy.Bind))
                    fail((matches ? "ambiguous" : "missing") + std::string(" named module/version provider ") + needed.Name);
                if (provider && *provider != index) edges[index].insert(*provider);
            }
            for (const auto& symbol : image.Data->Symbols) if (symbol.Import &&
                forwarded(image.Imports.at(*symbol.Import), symbol.Type) && *libcProvider != index)
                edges[index].insert(*libcProvider);
        }
        std::vector<unsigned> visited(modules.size());
        const auto visit = [&](const auto& self, std::size_t index) -> void {
            if (visited[index] == 1) fail("cyclic guest module dependencies are unsupported");
            if (visited[index] == 2) return;
            visited[index] = 1;
            for (const auto dependency : edges[index]) self(self, dependency);
            visited[index] = 2;
            order.push_back(index);
        };
        visit(visit, 0);
        if (order.size() != modules.size()) fail("supplied guest dependency is not reachable from the main image");
        // Page-rounded guest mappings must have exactly one owning module, because a
        // page is mapped and protected once. Two segments of the same module may share
        // a boundary page: real titles pack the next segment against the tail of the
        // previous one without page padding. Their bytes still may not overlap, and any
        // permission split that the host cannot enforce inside one page is already a
        // profile blocker rejected by RequireSceProfile above.
        struct MappingRange {
            std::size_t Module;
            std::uint64_t PageBegin, PageEnd, ByteBegin, ByteEnd;
        };
        std::vector<MappingRange> ranges;
        for (std::size_t index = 0; index < modules.size(); ++index) for (const auto& segment : modules[index].Image.Segments) {
            if (segment.Type != 1 && segment.Type != 0x61000010) continue;
            const auto bias = modules[index].LoadBias;
            ranges.push_back({index, SceAddress(bias, segment.Address & ~(PageSize - 1)),
                SceAddress(bias, (segment.Address + segment.MemorySize + PageSize - 1) & ~(PageSize - 1)),
                SceAddress(bias, segment.Address), SceAddress(bias, segment.Address + segment.MemorySize)});
        }
        for (std::size_t left = 0; left < ranges.size(); ++left) for (std::size_t right = left + 1; right < ranges.size(); ++right) {
            const auto& first = ranges[left];
            const auto& second = ranges[right];
            if (first.PageBegin >= second.PageEnd || second.PageBegin >= first.PageEnd) continue;
            if (first.Module != second.Module || (first.ByteBegin < second.ByteEnd && second.ByteBegin < first.ByteEnd))
                fail("guest module mapping ranges overlap");
        }
        std::vector<SceTlsModuleTemplate> templates;
        for (auto& module : modules) if (module.Image.Tls) {
            if (templates.empty() && &module != &modules.front()) fail("dependent TLS without main-module TLS is unsupported");
            module.TlsModuleId = templates.size() + 1;
            const auto& descriptor = *module.Image.Tls;
            templates.push_back({module.TlsModuleId,
                std::span(module.Image.Data->Bytes).subspan(descriptor.Offset, descriptor.FileSize), descriptor.MemorySize, descriptor.Alignment});
        }
        std::vector<std::vector<SceSymbolValue>> resolved(modules.size());
        for (std::size_t index = 0; index < modules.size(); ++index) {
            const auto& module = modules[index];
            for (const auto& symbol : module.Image.Data->Symbols) {
                if (!symbol.Import) {
                    if (symbol.Section && symbol.Binding && symbol.Visibility == 0 && !module.Image.Data->Symbolic)
                        fail("preemptible defined module symbols without DT_SYMBOLIC are unsupported");
                    if (symbol.Type == 6) resolved[index].push_back({0, 6, symbol.Size, module.TlsModuleId, symbol.Value});
                    else resolved[index].push_back({symbol.Section == 0xfff1 ? symbol.Value : SceAddress(module.LoadBias, symbol.Value),
                        symbol.Type, symbol.Size, 0, 0});
                    continue;
                }
                const auto& import = module.Image.Imports.at(*symbol.Import);
                if (symbol.Type != 1 && symbol.Type != 2 && symbol.Type != 6) fail("untyped or unsupported module import " + import.Nid);
                std::optional<SceSymbolValue> value;
                for (const auto& provider : modules) for (const auto& definition : provider.Image.Exports) {
                    if (!sameScope(import, definition.Identity) || definition.Visibility == 1 || definition.Visibility == 2) continue;
                    if (definition.Type != symbol.Type) fail("guest import/export symbol type mismatch for " + import.Nid);
                    if (value) fail("ambiguous guest export provider for " + import.Nid);
                    if (definition.Section >= 0xff00) fail("absolute guest module exports are unsupported");
                    if (symbol.Size > definition.Size) fail("guest import size exceeds export storage for " + import.Nid);
                    if (definition.Type == 1 && std::none_of(provider.Image.Segments.begin(), provider.Image.Segments.end(), [&](const auto& segment) {
                        const auto size = std::max<std::uint64_t>(definition.Size, 1);
                        return (segment.Type == 1 || segment.Type == 0x61000010) && (segment.Flags & 4) &&
                            definition.Value >= segment.Address && definition.Value - segment.Address <= segment.MemorySize &&
                            size <= segment.MemorySize - (definition.Value - segment.Address);
                    })) fail("guest object provider has no readable storage for " + import.Nid);
                    value = SceSymbolValue{definition.Type == 6 ? 0 : SceAddress(provider.LoadBias, definition.Value), definition.Type,
                        definition.Size, definition.Type == 6 ? provider.TlsModuleId : 0, definition.Type == 6 ? definition.Value : 0};
                }
                unsigned hostMatches = 0;
                for (const auto& host : hosts) if (declares(host, import)) ++hostMatches;
                if (value && hostMatches) fail("ambiguous guest/host provider for " + import.Nid);
                if (!value && forwarded(import, symbol.Type)) {
                    if (hostMatches != 1) fail("ambiguous libc Internal scope provider for " + import.Nid);
                    const auto& provider = modules[*libcProvider];
                    auto target = import;
                    target.ModuleName = target.LibraryName = "libc";
                    for (const auto& definition : provider.Image.Exports) {
                        if (!sameScope(target, definition.Identity)) continue;
                        if (definition.Type != 2 || (definition.Binding != 1 && definition.Binding != 2) ||
                            (definition.Visibility != 0 && definition.Visibility != 3) ||
                            !definition.Section || definition.Section >= 0xff00)
                            fail("invalid libc Internal target function for " + import.Nid);
                        if (value) fail("ambiguous libc Internal target export for " + import.Nid);
                        if (symbol.Size > definition.Size) fail("libc Internal import size exceeds target storage for " + import.Nid);
                        const auto size = std::max<std::uint64_t>(definition.Size, 1);
                        if (std::none_of(provider.Image.Segments.begin(), provider.Image.Segments.end(), [&](const auto& segment) {
                            return (segment.Type == 1 || segment.Type == 0x61000010) && (segment.Flags & 1) &&
                                definition.Value >= segment.Address && definition.Value - segment.Address < segment.FileSize &&
                                size <= segment.FileSize - (definition.Value - segment.Address);
                        })) fail("libc Internal target has no executable file-backed storage for " + import.Nid);
                        const auto firstPage = definition.Value & ~(PageSize - 1);
                        const auto lastPage = (definition.Value + size - 1) & ~(PageSize - 1);
                        if (std::any_of(provider.Image.Data->Relro.begin(), provider.Image.Data->Relro.end(), [&](const auto& relro) {
                            return relro.MemorySize && firstPage < relro.Address + relro.MemorySize && lastPage >= relro.Address;
                        })) fail("libc Internal target loses execute permission to RELRO for " + import.Nid);
                        value = SceSymbolValue{SceAddress(provider.LoadBias, definition.Value), 2, definition.Size, 0, 0};
                    }
                    if (!value) fail("missing qualified libc Internal target export for " + import.Nid);
                }
                if (!value) {
                    const SceImportConsumer consumer{module.Image.Path, module.Image.Data->SourceSize,
                        module.Image.Data->SourceSha256};
                    std::optional<SceResolvedImport> hostValue;
                    std::string unresolved;
                    if (hostMatches > 1 || (!hostMatches && !lazy.Bind))
                        fail("unresolved typed import scope/version provider for " + import.Nid);
                    if (!hostMatches) unresolved = "no provider declares module " + import.ModuleName + " library " + import.LibraryName;
                    else {
                        if (!consumerResolver && !resolver) fail("missing typed host resolver for " + import.Nid);
                        try {
                            hostValue = consumerResolver ? consumerResolver(consumer, import, symbol.Type, symbol.Size)
                                                         : resolver(import, symbol.Type);
                        } catch (const std::exception& error) {
                            // Lazy linking treats a provider rejection as "not implemented here".
                            if (!lazy.Bind) throw;
                            unresolved = error.what();
                        }
                        if (!hostValue && unresolved.empty()) unresolved = "host provider does not implement it";
                        if ((hostValue && hostValue->Type != symbol.Type) || (!hostValue && !lazy.Bind))
                            fail("unresolved or wrongly typed host import " + import.Nid);
                    }
                    if (hostValue) {
                        if (symbol.Type == 6) fail("host TLS imports require unsupported external TLS storage");
                        if (!hostValue->Address || hostValue->TlsModuleId || hostValue->TlsOffset ||
                            (symbol.Type == 1 && (!hostValue->Size || symbol.Size > hostValue->Size)))
                            fail("invalid host import storage for " + import.Nid);
                        machine.CheckAccess(hostValue->Address, symbol.Type == 2 ? 1 : hostValue->Size,
                            symbol.Type == 2 ? Permission::Execute : Permission::Read);
                        value = SceSymbolValue{hostValue->Address, hostValue->Type, hostValue->Size, 0, 0};
                    } else {
                        if (symbol.Type == 6) fail("unresolved TLS import " + import.Nid + ": " + unresolved);
                        const bool weak = symbol.Binding == 2;
                        const auto bound = lazy.Bind({consumer, import, symbol.Type, symbol.Size, weak, unresolved});
                        if (bound.Type != symbol.Type || bound.TlsModuleId || bound.TlsOffset ||
                            (weak ? bound.Address != 0 : !bound.Address) || (!weak && symbol.Type == 1 && symbol.Size > bound.Size))
                            fail("invalid lazy import binding for " + import.Nid);
                        if (!weak) machine.CheckAccess(bound.Address, symbol.Type == 2 ? 1 : std::max<std::uint64_t>(symbol.Size, 1),
                            symbol.Type == 2 ? Permission::Execute : Permission::Read);
                        value = SceSymbolValue{bound.Address, bound.Type, bound.Size, 0, 0};
                    }
                }
                resolved[index].push_back(*value);
            }
        }
        if (!templates.empty()) tls = std::make_shared<SceTls>(machine, templates);
        std::vector<std::vector<SceRelocationWrite>> writes;
        for (std::size_t index = 0; index < modules.size(); ++index)
            writes.push_back(PlanSceRelocations(modules[index].Image, modules[index].LoadBias, [&](std::uint64_t symbol) {
                if (symbol == 0) return SceSymbolValue{0, 6, 0, modules[index].TlsModuleId, 0};
                return resolved[index].at(symbol);
            }, tls.get()));
        for (const auto& module : modules) MapSceImage(machine, module.Image, module.LoadBias);
        for (const auto& moduleWrites : writes) ApplySceRelocations(machine, moduleWrites);
        threadTlsTemplates.reserve(templates.size());
        for (const auto& module : modules) if (module.TlsModuleId) {
            const auto& descriptor = *module.Image.Tls;
            std::vector<std::byte> bytes(descriptor.FileSize);
            if (!bytes.empty()) {
                machine.Read(SceAddress(module.LoadBias, descriptor.Address), bytes);
                machine.Write(tls->TlsBase(module.TlsModuleId), bytes);
            }
            threadTlsTemplates.push_back({module.TlsModuleId, std::move(bytes), descriptor.MemorySize, descriptor.Alignment});
        }
        for (const auto& module : modules) ProtectSceImage(machine, module.Image, module.LoadBias);
        const auto& image = modules.front();
        main = {image.Image.Path, SceAddress(image.LoadBias, image.Image.Entry), image.LoadBias, 0, 0,
            image.Image.Imports, image.Image.NeededModules, image.Image.ProcParam, tls};
        if (main.ProcParam) main.ProcParam->Address = SceAddress(image.LoadBias, main.ProcParam->Address);
        std::array<std::byte, PageSize> bytes{};
        bytes.fill(std::byte{0xcc});
        bytes[0] = bytes[16] = std::byte{0xc3};
        machine.Map(ReturnGate, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(ReturnGate, bytes);
        machine.Protect(ReturnGate, bytes.size(), Permission::Read | Permission::Execute);
    }

    std::shared_ptr<SceTls> createThreadTls(std::uint64_t allocationBase) const {
        if (phase == Phase::Failed || phase == Phase::Finalizing || phase == Phase::Finalized)
            fail("thread TLS creation requires an active module graph");
        if (threadTlsTemplates.empty()) return {};
        std::vector<SceTlsModuleTemplate> templates;
        templates.reserve(threadTlsTemplates.size());
        for (const auto& source : threadTlsTemplates)
            templates.push_back({source.moduleId, source.initialBytes, source.memorySize, source.alignment});
        return std::make_shared<SceTls>(machine, templates, allocationBase, SceTlsActivation::Deferred);
    }

    Mapping initialStack() const {
        if (!main.StackPointer) fail("initial guest stack is not configured");
        for (const auto& mapping : machine.Mappings()) {
            const auto required = static_cast<unsigned>(Permission::Read | Permission::Write);
            if (mapping.Size && main.StackPointer >= mapping.Address &&
                main.StackPointer - mapping.Address < mapping.Size &&
                (static_cast<unsigned>(mapping.Permissions) & required) == required)
                return mapping;
        }
        fail("configured initial guest stack has no readable writable mapping");
    }

    void setExecutor(SceModuleExecutor value) {
        if (executor || phase != Phase::Loaded || mainStarted)
            fail("guest executor may be installed only once before dependency initialization");
        if (value.Owner != &machine || !value.InitialThread || !value.Invoke || !value.RunEntry ||
            !value.PauseTerminationFromHostCall || !value.PendingControl || !value.CompleteControl)
            fail("guest executor requires the owner machine, adopted initial identity, and all execution hooks");
        initialStack();
        if (machine.Get(Register::Rsp) != main.StackPointer ||
            machine.Get(Register::FsBase) != (tls ? tls->FsBase() : 0))
            fail("guest executor requires the configured initial stack and primary TLS to be active");
        const auto control = value.PendingControl();
        if (control.Kind != GuestEntryControlKind::None || control.ExitCode)
            fail("guest executor has pending entry control before initialization");
        executor = std::move(value);
    }

    void invokeWithExecutor(GuestModuleCallKind kind, std::uint64_t entry, std::uint64_t args,
                            std::uint64_t argp, std::uint64_t param, GuestPhaseBudget& budget) {
        if (!budget.Remaining()) fail("module initializer/finalizer exhausted its phase instruction budget");
        const auto consumed = budget.Consumed();
        const auto result = executor->Invoke({kind, entry, ReturnGate, {args, argp, param}}, budget);
        if (result.Reason == StopReason::Exit) fail("module initializer/finalizer exited the guest instead of returning");
        if (result.Reason == StopReason::Requested) fail("module initializer/finalizer execution was interrupted");
        if (result.Reason == StopReason::InstructionLimit) fail("module initializer/finalizer exceeded its phase instruction budget");
        if (result.Reason != StopReason::Address || !result.ReturnValue)
            fail("module initializer/finalizer executor did not complete an actual guest return");
        if (budget.Consumed() == consumed)
            fail("module initializer/finalizer executor returned without charging guest execution");
        const auto status = static_cast<std::uint32_t>(*result.ReturnValue);
        if (status != 0) fail("module initializer/finalizer returned a failure status " + std::to_string(status));
    }

    void invoke(std::uint64_t entry, std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
        constexpr std::array savedRegisters{Register::Rax, Register::Rbx, Register::Rcx, Register::Rdx, Register::Rsi, Register::Rdi,
            Register::Rbp, Register::Rsp, Register::R8, Register::R9, Register::R10, Register::R11, Register::R12, Register::R13,
            Register::R14, Register::R15, Register::Rip, Register::Rflags};
        std::array<std::uint64_t, savedRegisters.size()> saved{};
        for (std::size_t index = 0; index < saved.size(); ++index) saved[index] = machine.Get(savedRegisters[index]);
        const auto callerStack = machine.Get(Register::Rsp);
        if (callerStack < 512) fail("module initializer/finalizer guest stack underflows");
        const auto stack = ((callerStack - 256) & ~15ull) + 8;
        machine.CheckAccess(stack - 128, 136, Permission::Write);
        writeWord(machine, stack, ReturnGate);
        machine.Set(Register::Rsp, stack);
        machine.Set(Register::Rdi, args);
        machine.Set(Register::Rsi, argp);
        machine.Set(Register::Rdx, param);
        machine.Set(Register::Rflags, 2);
        const auto reason = machine.Run(entry, ReturnGate, budget);
        if (reason == StopReason::Exit) fail("module initializer/finalizer exited the guest instead of returning");
        if (reason == StopReason::Requested) fail("module initializer/finalizer execution was interrupted");
        if (reason == StopReason::InstructionLimit) fail("module initializer/finalizer exceeded its execution budget");
        if (machine.Get(Register::Rsp) != stack + 8) fail("module initializer/finalizer returned with an invalid guest stack");
        const auto status = static_cast<std::uint32_t>(machine.Get(Register::Rax));
        if (status != 0) fail("module initializer/finalizer returned a failure status " + std::to_string(status));
        for (std::size_t index = 0; index < saved.size(); ++index) machine.Set(savedRegisters[index], saved[index]);
    }

    void initialize(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
        if (phase != Phase::Loaded) fail("dependency initialization requires a freshly loaded graph");
        if (!main.StackPointer || !budget) fail("dependency initialization requires configured main entry arguments and a nonzero budget");
        phase = Phase::Initializing;
        try {
            if (executor) {
                GuestPhaseBudget phaseBudget(budget);
                for (const auto index : order)
                    if (index != 0 && modules[index].Init)
                        invokeWithExecutor(GuestModuleCallKind::Initialize, modules[index].Init, args, argp, param, phaseBudget);
            } else {
                for (const auto index : order)
                    if (index != 0 && modules[index].Init) invoke(modules[index].Init, args, argp, param, budget);
            }
            phase = Phase::Initialized;
        } catch (...) { phase = Phase::Failed; throw; }
    }

    void finalizeWithExecutor(std::uint64_t args, std::uint64_t argp, std::uint64_t param, GuestPhaseBudget& budget) {
        if (phase != Phase::Initialized) fail("dependency finalization requires successful initialization");
        phase = Phase::Finalizing;
        try {
            for (auto cursor = order.rbegin(); cursor != order.rend(); ++cursor)
                if (*cursor != 0 && modules[*cursor].Fini)
                    invokeWithExecutor(GuestModuleCallKind::Finalize, modules[*cursor].Fini, args, argp, param, budget);
            phase = Phase::Finalized;
        } catch (...) { phase = Phase::Failed; throw; }
    }

    void finalize(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
        if (phase != Phase::Initialized || !budget) fail("dependency finalization requires successful initialization and a nonzero budget");
        if (executor) {
            GuestPhaseBudget phaseBudget(budget);
            finalizeWithExecutor(args, argp, param, phaseBudget);
            return;
        }
        phase = Phase::Finalizing;
        try {
            for (auto cursor = order.rbegin(); cursor != order.rend(); ++cursor)
                if (*cursor != 0 && modules[*cursor].Fini) invoke(modules[*cursor].Fini, args, argp, param, budget);
            phase = Phase::Finalized;
        } catch (...) { phase = Phase::Failed; throw; }
    }

    void requestTermination() {
        if (!mainRunning || phase != Phase::Initialized || terminationRequested)
            fail("entry termination callback requires an active initialized main image and may execute only once");
        if (executor) {
            executor->PauseTerminationFromHostCall();
            terminationRequested = true;
        } else {
            terminationRequested = true;
            machine.RequestStop();
        }
    }

    StopReason runMainWithExecutor(std::uint64_t entryBudget, std::uint64_t finalizerBudget) {
        GuestPhaseBudget entryPhase(entryBudget);
        GuestPhaseBudget finalizerPhase(finalizerBudget);
        for (;;) {
            const auto reason = executor->RunEntry(entryPhase);
            if (reason != StopReason::Paused) {
                if (reason == StopReason::Requested || reason == StopReason::InstructionLimit) {
                    // Cancellation or exhaustion never completes a paused call
                    // or publishes process success. Withdraw retires any token.
                    phase = Phase::Failed;
                    return reason;
                }
                const auto control = executor->PendingControl();
                if (control.Kind != GuestEntryControlKind::None || control.ExitCode || terminationRequested)
                    fail("guest executor returned without its pending entry-control pause");
                if (reason == StopReason::Exit) {
                    if (phase != Phase::Finalized)
                        fail("guest executor exited before dependency finalization control");
                } else {
                    phase = Phase::Failed;
                    fail("guest executor returned an unsupported entry stop reason");
                }
                return reason;
            }
            const auto control = executor->PendingControl();
            if (control.Kind == GuestEntryControlKind::TerminationCallback) {
                if (control.ExitCode || !terminationRequested || machine.Get(Register::Rip) != TerminationGate)
                    fail("entry termination callback did not pause at its exact guest gate");
            } else if (control.Kind == GuestEntryControlKind::ProcessExit) {
                if (!control.ExitCode || terminationRequested)
                    fail("process exit control requires an exit code and a distinct entry pause");
            } else fail("guest executor paused without supported entry control");
            if (phase == Phase::Initialized) finalizeWithExecutor(0, 0, 0, finalizerPhase);
            else if (phase != Phase::Finalized) fail("entry control requires an initialized or finalized module graph");
            executor->CompleteControl();
            const auto retained = executor->PendingControl();
            if (retained.Kind != GuestEntryControlKind::None || retained.ExitCode)
                fail("guest executor did not consume its completed entry control");
            if (control.Kind == GuestEntryControlKind::ProcessExit) {
                // The runtime commits Machine::Exit only after successful fini.
                // Its terminal observation must charge zero and preserve the
                // original entry budget, even when that budget is exhausted.
                const auto consumed = entryPhase.Consumed();
                if (executor->RunEntry(entryPhase) != StopReason::Exit ||
                    entryPhase.Consumed() != consumed || machine.ExitCode() != *control.ExitCode)
                    fail("guest executor did not commit the exact process exit after finalization");
                return StopReason::Exit;
            }
            terminationRequested = false;
            if (!entryPhase.Remaining()) { phase = Phase::Failed; return StopReason::InstructionLimit; }
        }
    }

    StopReason runMain(std::uint64_t entryBudget, std::uint64_t finalizerBudget) {
        if (mainStarted || phase != Phase::Initialized || !main.StackPointer || !entryBudget || !finalizerBudget)
            fail("main execution requires a fresh initialized graph, configured stack, and nonzero phase budgets");
        mainStarted = mainRunning = true;
        struct Reset { bool& running; ~Reset() { running = false; } } reset{mainRunning};
        try {
            if (executor) return runMainWithExecutor(entryBudget, finalizerBudget);
            auto reason = machine.Run(main.Entry, 0, entryBudget);
            const auto consumed = machine.LastRunInstructions();
            if (consumed > entryBudget) fail("main execution exceeded its entry instruction budget");
            if (terminationRequested) {
                if (reason != StopReason::Requested || machine.Get(Register::Rip) != TerminationGate)
                    fail("entry termination callback did not pause at its exact guest gate");
                const auto stack = machine.Get(Register::Rsp);
                if (stack > UINT64_MAX - 8) fail("entry termination callback return stack overflows");
                machine.CheckAccess(stack, 8, Permission::Read);
                std::uint64_t destination = 0;
                machine.Read(stack, std::as_writable_bytes(std::span(&destination, 1)));
                machine.CheckAccess(destination, 1, Permission::Execute);
                finalize(0, 0, 0, finalizerBudget);
                machine.CheckAccess(stack, 8, Permission::Read);
                std::uint64_t retainedDestination = 0;
                machine.Read(stack, std::as_writable_bytes(std::span(&retainedDestination, 1)));
                if (machine.Get(Register::Rsp) != stack || retainedDestination != destination)
                    fail("entry termination callback return frame changed during dependency finalization");
                machine.CheckAccess(destination, 1, Permission::Execute);
                machine.Set(Register::Rsp, stack + 8);
                machine.Set(Register::Rip, destination);
                terminationRequested = false;
                const auto remaining = entryBudget - consumed;
                reason = remaining ? machine.Run(destination, 0, remaining) : StopReason::InstructionLimit;
            }
            if (reason == StopReason::Exit) {
                if (phase == Phase::Initialized) finalize(0, 0, 0, finalizerBudget);
            } else phase = Phase::Failed;
            return reason;
        } catch (...) { phase = Phase::Failed; throw; }
    }
};

SceModules::SceModules(Machine& machine, const SceModuleFile& main, std::span<const SceModuleFile> dependencies,
                     std::span<const SceHostModule> hosts, const SceModuleResolver& resolver,
                     const std::optional<SceLibcInternalProvider>& libcInternal,
                     const SceConsumerModuleResolver& consumerResolver, const SceLazyImports& lazyImports)
    : impl(std::make_shared<Impl>(machine, main, dependencies, hosts, resolver, libcInternal, consumerResolver, lazyImports)) {
    machine.AddHostCall(TerminationGate, [state = std::weak_ptr<Impl>(impl)](Machine&) {
        const auto context = state.lock();
        if (!context) fail("entry termination callback graph has expired");
        context->requestTermination();
    });
}
SceModules::~SceModules() = default;
SceLoadedImage& SceModules::Main() { return impl->main; }
std::span<const SceModuleRecord> SceModules::Modules() const { return impl->modules; }
std::shared_ptr<SceTls> SceModules::Tls() const { return impl->tls; }
std::shared_ptr<SceTls> SceModules::CreateThreadTls(std::uint64_t allocationBase) const {
    return impl->createThreadTls(allocationBase);
}
SceThreadTlsFactory SceModules::ThreadTlsFactory() const {
    return [state = std::weak_ptr<Impl>(impl)](std::uint64_t allocationBase) {
        const auto context = state.lock();
        if (!context) fail("thread TLS factory graph has expired");
        return context->createThreadTls(allocationBase);
    };
}
Mapping SceModules::InitialStack() const { return impl->initialStack(); }
void SceModules::SetExecutor(SceModuleExecutor executor) { impl->setExecutor(std::move(executor)); }
std::uint64_t SceModules::EntryTerminationGate() const { return TerminationGate; }
StopReason SceModules::RunMain(std::uint64_t entryBudget, std::uint64_t finalizerBudget) {
    return impl->runMain(entryBudget, finalizerBudget);
}
void SceModules::InitializeDependencies(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
    impl->initialize(args, argp, param, budget);
}
void SceModules::FinalizeDependencies(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
    impl->finalize(args, argp, param, budget);
}

}
