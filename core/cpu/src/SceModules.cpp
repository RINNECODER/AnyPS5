#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include "SceImageData.hpp"
#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace Cpu {
namespace {
constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t ReturnGate = 0x7ffdf7000000;
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

bool bareFilename(const std::string& filename) {
    return !filename.empty() && filename != "." && filename != ".." &&
        filename.find_first_of("/\\:\r\n") == std::string::npos;
}

void writeWord(Machine& machine, std::uint64_t address, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(value >> (index * 8));
    machine.Write(address, bytes);
}
}

struct SceModules::Impl {
    Machine& machine;
    std::vector<SceModuleRecord> modules;
    std::vector<SceHostModule> hosts;
    std::vector<std::set<std::size_t>> edges;
    std::vector<std::size_t> order;
    std::shared_ptr<SceTls> tls;
    SceLoadedImage main;
    enum class Phase { Loaded, Initializing, Initialized, Finalizing, Finalized, Failed };
    Phase phase = Phase::Loaded;

    Impl(Machine& guest, const SceModuleFile& executable, std::span<const SceModuleFile> dependencies,
         std::span<const SceHostModule> hostModules, const SceModuleResolver& resolver) : machine(guest), hosts(hostModules.begin(), hostModules.end()) {
        if (dependencies.size() > 510 || hosts.size() > 510) fail("module graph exceeds the supported provider count");
        const auto add = [&](const SceModuleFile& file, bool isMain) {
            auto parsed = ParseSce(file.Path);
            if (isMain ? parsed.Type == 0xfe18 : parsed.Type != 0xfe18)
                fail(isMain ? "main image must be an executable" : "dependency image must be an SCE shared module");
            RequireSceProfile(parsed, GraphRequirements);
            ValidateSceMapping(parsed, file.LoadBias);
            const auto& data = *parsed.Data;
            if (data.PreinitArray.Size || data.InitArray.Size || data.FiniArray.Size)
                fail("unsupported CRT-array ownership for the generic module profile");
            if (isMain && data.Fini) fail("main executable DT_FINI lifecycle is unsupported by the dependency finalizer");
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
        edges.resize(modules.size());
        for (std::size_t index = 0; index < modules.size(); ++index) {
            const auto& image = modules[index].Image;
            for (const auto& filename : image.NeededFiles) {
                if (!bareFilename(filename)) fail("DT_NEEDED requires a declared bare filename: " + filename);
                if (const auto found = files.find(filename); found != files.end()) edges[index].insert(found->second);
                else if (!hostFiles.contains(filename)) fail("missing DT_NEEDED provider " + filename);
            }
            for (const auto& needed : image.Data->ImportedModules) {
                std::optional<std::size_t> provider;
                unsigned matches = 0;
                for (std::size_t candidate = 0; candidate < modules.size(); ++candidate)
                    if (std::any_of(modules[candidate].Image.ExportModules.begin(), modules[candidate].Image.ExportModules.end(),
                        [&](const auto& identity) { return sameModule(needed, identity); })) { provider = candidate; ++matches; }
                for (const auto& host : hosts) if (sameModule(needed, host.Module)) ++matches;
                if (matches != 1) fail((matches ? "ambiguous" : "missing") + std::string(" named module/version provider ") + needed.Name);
                if (provider && *provider != index) edges[index].insert(*provider);
            }
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
        std::map<std::uint64_t, std::uint64_t> ranges;
        for (const auto& module : modules) for (const auto& segment : module.Image.Segments) {
            if (segment.Type != 1 && segment.Type != 0x61000010) continue;
            const auto begin = SceAddress(module.LoadBias, segment.Address & ~(PageSize - 1));
            const auto end = SceAddress(module.LoadBias, (segment.Address + segment.MemorySize + PageSize - 1) & ~(PageSize - 1));
            const auto next = ranges.lower_bound(begin);
            if ((next != ranges.end() && next->first < end) || (next != ranges.begin() && std::prev(next)->second > begin))
                fail("guest module mapping ranges overlap");
            ranges.emplace(begin, end);
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
                if (!value) {
                    if (hostMatches != 1) fail("unresolved typed import scope/version provider for " + import.Nid);
                    if (!resolver) fail("missing typed host resolver for " + import.Nid);
                    const auto hostValue = resolver(import, symbol.Type);
                    if (!hostValue || hostValue->Type != symbol.Type) fail("unresolved or wrongly typed host import " + import.Nid);
                    if (symbol.Type == 6) fail("host TLS imports require unsupported external TLS storage");
                    if (!hostValue->Address || hostValue->TlsModuleId || hostValue->TlsOffset ||
                        (symbol.Type == 1 && (!hostValue->Size || symbol.Size > hostValue->Size)))
                        fail("invalid host import storage for " + import.Nid);
                    machine.CheckAccess(hostValue->Address, symbol.Type == 2 ? 1 : hostValue->Size,
                        symbol.Type == 2 ? Permission::Execute : Permission::Read);
                    value = SceSymbolValue{hostValue->Address, hostValue->Type, hostValue->Size, 0, 0};
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
        for (const auto& module : modules) if (module.TlsModuleId && module.Image.Tls->FileSize) {
            std::vector<std::byte> bytes(module.Image.Tls->FileSize);
            machine.Read(SceAddress(module.LoadBias, module.Image.Tls->Address), bytes);
            machine.Write(tls->TlsBase(module.TlsModuleId), bytes);
        }
        for (const auto& module : modules) ProtectSceImage(machine, module.Image, module.LoadBias);
        const auto& image = modules.front();
        main = {image.Image.Path, SceAddress(image.LoadBias, image.Image.Entry), image.LoadBias, 0, 0,
            image.Image.Imports, image.Image.NeededModules, image.Image.ProcParam, tls};
        if (main.ProcParam) main.ProcParam->Address = SceAddress(image.LoadBias, main.ProcParam->Address);
    }

    void invoke(std::uint64_t entry, std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
        constexpr std::array savedRegisters{Register::Rax, Register::Rbx, Register::Rcx, Register::Rdx, Register::Rsi, Register::Rdi,
            Register::Rbp, Register::Rsp, Register::R8, Register::R9, Register::R10, Register::R11, Register::R12, Register::R13,
            Register::R14, Register::R15, Register::Rip, Register::Rflags};
        std::array<std::uint64_t, savedRegisters.size()> saved{};
        for (std::size_t index = 0; index < saved.size(); ++index) saved[index] = machine.Get(savedRegisters[index]);
        const auto stack = ((main.StackPointer - 256) & ~15ull) + 8;
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
        std::array<std::byte, PageSize> bytes{};
        bytes.fill(std::byte{0xcc});
        bytes[0] = std::byte{0xc3};
        machine.Map(ReturnGate, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(ReturnGate, bytes);
        machine.Protect(ReturnGate, bytes.size(), Permission::Read | Permission::Execute);
        phase = Phase::Initializing;
        try {
            for (const auto index : order) if (index != 0 && modules[index].Init) invoke(modules[index].Init, args, argp, param, budget);
            phase = Phase::Initialized;
        } catch (...) { phase = Phase::Failed; throw; }
    }

    void finalize(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
        if (phase != Phase::Initialized || !budget) fail("dependency finalization requires successful initialization and a nonzero budget");
        phase = Phase::Finalizing;
        try {
            for (auto cursor = order.rbegin(); cursor != order.rend(); ++cursor)
                if (*cursor != 0 && modules[*cursor].Fini) invoke(modules[*cursor].Fini, args, argp, param, budget);
            phase = Phase::Finalized;
        } catch (...) { phase = Phase::Failed; throw; }
    }
};

SceModules::SceModules(Machine& machine, const SceModuleFile& main, std::span<const SceModuleFile> dependencies,
                     std::span<const SceHostModule> hosts, const SceModuleResolver& resolver)
    : impl(std::make_unique<Impl>(machine, main, dependencies, hosts, resolver)) {}
SceModules::~SceModules() = default;
SceLoadedImage& SceModules::Main() { return impl->main; }
std::span<const SceModuleRecord> SceModules::Modules() const { return impl->modules; }
std::shared_ptr<SceTls> SceModules::Tls() const { return impl->tls; }
void SceModules::InitializeDependencies(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
    impl->initialize(args, argp, param, budget);
}
void SceModules::FinalizeDependencies(std::uint64_t args, std::uint64_t argp, std::uint64_t param, std::uint64_t budget) {
    impl->finalize(args, argp, param, budget);
}

}
