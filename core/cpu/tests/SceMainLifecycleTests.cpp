#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceModules.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr std::uint64_t MainBias = 0x1000000;
constexpr std::uint64_t RuntimeBias = 0x2000000;
const Cpu::SceHostModule Kernel{"libkernel.prx", {"libkernel", 7, 1, 1}, {{"libkernel", 11, 1}}};

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Function> void rejects(Function&& function, const char* diagnostic) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing lifecycle rejection: ") + diagnostic);
}

struct Source {
    std::filesystem::path Path;
    std::uint64_t Init, Fini, State;
};
struct Inputs { Source Runtime; Source Main; };
Inputs receipt(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot open independent main lifecycle receipt");
    Inputs result;
    for (unsigned index = 0; index < 2; ++index) {
        std::string line, kind, filename, hash;
        std::uint64_t size;
        auto& source = index ? result.Main : result.Runtime;
        require(bool(std::getline(input, line)), "Incomplete main lifecycle receipt");
        std::istringstream record(line);
        require(bool(record >> kind >> filename >> hash >> size >> source.Init >> source.Fini >> source.State),
                "Invalid main lifecycle receipt fields");
        require(kind == (index ? "main" : "runtime") && hash.size() == 64 && size > 0,
                "Invalid main lifecycle source identity");
        std::string extra;
        require(!(record >> extra), "Unexpected main lifecycle receipt fields");
        source.Path = path.parent_path() / filename;
    }
    return result;
}

std::array<std::uint64_t, 13> state(Cpu::Machine& machine, std::uint64_t address) {
    std::array<std::uint64_t, 13> result;
    machine.Read(address, std::as_writable_bytes(std::span(result)));
    return result;
}

struct Session {
    Cpu::Machine Machine;
    Cpu::SceLifecycleImports KernelExit{Machine};
    std::unique_ptr<Cpu::SceModules> Modules;
    std::uint64_t State;

    explicit Session(const Inputs& inputs) : State(RuntimeBias + inputs.Runtime.State) {
        const std::array dependencies{Cpu::SceModuleFile{inputs.Runtime.Path, RuntimeBias}};
        const std::array hosts{Kernel};
        Modules = std::make_unique<Cpu::SceModules>(Machine, Cpu::SceModuleFile{inputs.Main.Path, MainBias}, dependencies, hosts,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                require(type == 2, "Main lifecycle fixture unexpectedly requires a host data service");
                const auto address = KernelExit.Resolve(import);
                if (!address) return std::nullopt;
                return Cpu::SceResolvedImport{*address, type};
            });
        require(Modules->Modules()[0].Init == MainBias + inputs.Main.Init &&
                Modules->Modules()[0].Fini == MainBias + inputs.Main.Fini,
                "Independent scalar main lifecycle metadata was lost");
        require(state(Machine, State) == std::array<std::uint64_t, 13>{0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0},
                "Module loading executed guest lifecycle callbacks");
        Cpu::SetupSceEntry(Machine, Modules->Main(), {"main-lifecycle"}, Modules->EntryTerminationGate());
        Modules->InitializeDependencies();
        require(state(Machine, State) == std::array<std::uint64_t, 13>{1, 1, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0},
                "Dependency initialization replayed main-owned initialization or termination");
    }
};

void compiledLifecycle(const Inputs& inputs) {
    Session session(inputs);
    require(session.Modules->RunMain(1000000, 100000) == Cpu::StopReason::Exit && session.Machine.ExitCode() == 0,
            "Actual guest lifecycle rejected callback order, stack preservation, or cleanup before kernel exit");
    require(state(session.Machine, session.State) ==
            std::array<std::uint64_t, 13>{12345678, 1, 1, 1, 1, 1, 1, 1, 1, 12120050, 0, 3, 1},
            "Actual main lifecycle disagreed with independent ordering, counters, or arithmetic result");
    rejects([&] { session.Modules->RunMain(); }, "fresh initialized graph");
    rejects([&] { session.Modules->FinalizeDependencies(); }, "successful initialization");
}

void stoppedLifecycle(const Inputs& inputs) {
    Session session(inputs);
    session.Machine.AddHostCall(session.Modules->Main().Entry, [](Cpu::Machine& machine) { machine.RequestStop(); });
    require(session.Modules->RunMain() == Cpu::StopReason::Requested,
            "An external stop request resumed guest execution as a termination callback");
    require(state(session.Machine, session.State) == std::array<std::uint64_t, 13>{1, 1, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0},
            "External stop advanced main code or normal dependency finalization");
    rejects([&] { session.Modules->RunMain(); }, "fresh initialized graph");
}

void expiredGraph(const Inputs& inputs) {
    Session session(inputs);
    const auto gate = session.Modules->EntryTerminationGate();
    session.Modules.reset();
    rejects([&] { session.Machine.Run(gate, 0, 10); }, "graph has expired");
}

Cpu::SceImport exitImport() {
    Cpu::SceImport result;
    result.Nid = "6Z83sYWFlA8";
    result.LibraryName = result.ModuleName = "libkernel";
    result.LibraryVersion = result.ModuleMajor = result.ModuleMinor = 1;
    result.LibraryId = 7;
    result.ModuleId = 11;
    return result;
}

void cumulativeEntryBudget(const Inputs& inputs) {
    for (const auto budget : {7ULL, 8ULL, 13ULL}) {
        Session session(inputs);
        const auto gate = session.KernelExit.Resolve(exitImport());
        require(bool(gate), "Instruction budget fixture requires typed kernel exit");
        std::array<std::uint8_t, 36> program{
            0x90, 0x90, 0x90, 0x90, 0x90, 0xff, 0xd6,
            0xb8, 1, 0, 0, 0, 0xb8, 2, 0, 0, 0, 0xb8, 3, 0, 0, 0,
            0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd0, 0x0f, 0x0b};
        for (unsigned index = 0; index < 8; ++index) program[24 + index] = *gate >> (8 * index);
        const auto entry = session.Modules->Main().Entry;
        session.Machine.Write(entry, std::as_bytes(std::span(program)));
        session.Machine.Set(Cpu::Register::Rax, 0);
        session.Machine.Set(Cpu::Register::Rdi, 0);
        const auto reason = session.Modules->RunMain(budget, 100000);
        require(state(session.Machine, session.State)[8] == 1,
                "Entry budget test did not finalize dependency at the deferred pause");
        if (budget == 13) {
            require(reason == Cpu::StopReason::Exit && session.Machine.ExitCode() == 0,
                    "Exact cumulative entry budget did not permit the literal thirteen-instruction exit");
        } else {
            require(reason == Cpu::StopReason::InstructionLimit,
                    "Resumed main received a fresh instruction budget instead of the remaining entry budget");
            require(session.Machine.Get(Cpu::Register::Rip) == entry + (budget == 7 ? 7 : 12) &&
                    session.Machine.Get(Cpu::Register::Rax) == (budget == 7 ? 0 : 1),
                    "Resumed main executed beyond the independently counted entry budget");
        }
    }
}

void kernelExitAbi() {
    Cpu::Machine machine;
    auto imports = std::make_unique<Cpu::SceLifecycleImports>(machine);
    const auto import = exitImport();
    const auto gate = imports->Resolve(import);
    require(gate && gate == imports->Resolve(import), "Repeated kernel exit identity changed its gate");
    auto local = import;
    local.LibraryId = 31; local.ModuleId = 41;
    require(imports->Resolve(local) != gate, "Kernel exit discarded importer-local identity");
    for (unsigned index = 0; index < 5; ++index) {
        auto wrong = import;
        switch (index) {
        case 0: wrong.LibraryName = "libc"; break;
        case 1: wrong.ModuleName = "other"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 2; break;
        }
        rejects([&] { imports->Resolve(wrong); }, "scope/version");
    }
    auto other = import;
    other.Nid = "8G2LB+A3rzg";
    require(!imports->Resolve(other), "Kernel exit provider replaced guest-owned atexit");
    machine.Map(0x1000, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);
    machine.Map(0x2000, 4096, Cpu::Permission::Read | Cpu::Permission::Write);
    machine.Map(0x4000, 4096, Cpu::Permission::Read | Cpu::Permission::Write);
    constexpr std::array<std::uint8_t, 8> caller{0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x0f, 0x0b};
    machine.Write(0x1000, std::as_bytes(std::span(caller)));
    machine.Write(0x2000, std::as_bytes(std::span(&*gate, 1)));
    for (const auto [argument, status] : std::array<std::pair<std::uint64_t, int>, 3>{{
             {0xabcdef0000000007, 7}, {0x12345678ffffffff, 255}, {0xabcdef0000000100, 0}}}) {
        machine.Set(Cpu::Register::Rdi, argument);
        machine.Set(Cpu::Register::Rsp, 0x4ff0);
        require(machine.Run(0x1000, 0, 100) == Cpu::StopReason::Exit && machine.ExitCode() == status &&
                machine.Get(Cpu::Register::Rsp) == 0x4fe8,
                "Kernel exit did not consume low32 signed status as a nonreturning process exit");
    }
    imports.reset();
    rejects([&] { machine.Run(*gate, 0, 10); }, "runtime has expired");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: SceMainLifecycleTests SceMainLifecycleReceipt.txt");
        const auto inputs = receipt(argv[1]);
        compiledLifecycle(inputs);
        cumulativeEntryBudget(inputs);
        stoppedLifecycle(inputs);
        expiredGraph(inputs);
        kernelExitAbi();
        std::cout << "PASS entry-owned main initialization/finalization, guest callback ordering, deferred cleanup before kernel exit, live stack preservation, and strict lifecycle boundaries\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
