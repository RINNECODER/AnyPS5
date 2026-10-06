#include <cpu/GuestThreads.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::uint64_t MainBias = 0x1000000;
constexpr std::uint64_t GuestBias = 0x2000000;
constexpr std::uint64_t Edge = 0x300000000000;
constexpr std::uint64_t ArithmeticResult = 33644544;
constexpr std::array InitArguments{0x1020304050607080ULL, 0x8877665544332211ULL, 0xabcdef0198765432ULL};
constexpr auto rw = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr std::array PreservedRegisters{Cpu::Register::Rax, Cpu::Register::Rbx, Cpu::Register::Rcx, Cpu::Register::Rdx,
    Cpu::Register::Rsi, Cpu::Register::Rdi, Cpu::Register::Rbp, Cpu::Register::Rsp, Cpu::Register::R8,
    Cpu::Register::R9, Cpu::Register::R10, Cpu::Register::R11, Cpu::Register::R12, Cpu::Register::R13,
    Cpu::Register::R14, Cpu::Register::R15, Cpu::Register::Rip, Cpu::Register::Rflags,
    Cpu::Register::FsBase, Cpu::Register::GsBase};

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

const Cpu::SceExport& exported(const Cpu::SceModuleRecord& module, const char* nid, unsigned type) {
    const auto found = std::find_if(module.Image.Exports.begin(), module.Image.Exports.end(), [&](const auto& item) {
        return item.Identity.Nid == nid && item.Type == type;
    });
    require(found != module.Image.Exports.end(), "Compiled thread fixture lost an expected typed export");
    return *found;
}

bool sameMappings(const std::vector<Cpu::Mapping>& first, const std::vector<Cpu::Mapping>& second) {
    if (first.size() != second.size()) return false;
    for (std::size_t index = 0; index < first.size(); ++index)
        if (first[index].Address != second[index].Address || first[index].Size != second[index].Size ||
            first[index].Permissions != second[index].Permissions || first[index].Borrowed != second[index].Borrowed) return false;
    return true;
}

struct Session {
    Cpu::Machine Machine;
    std::shared_ptr<Cpu::GuestThreads> Threads = std::make_shared<Cpu::GuestThreads>(Machine);
    Cpu::SceImports Libc{Machine};
    Cpu::SceThreadImports Kernel{Machine, Threads};
    Cpu::SceLifecycleImports Lifecycle{Machine};
    std::unique_ptr<Cpu::SceModules> Graph;
    std::uint64_t ReceiptAddress, EventsAddress, LifecycleAddress;
    std::array<std::byte, 16> EdgeGuard;
    unsigned TlsFactoryCalls = 0;
    bool ProcessExitObserved = false, ExitBeforeFini = false;
    unsigned ProcessExitCalls = 0;
    int ObservedExitStatus = -1;
    std::array<std::uint64_t, PreservedRegisters.size()> ExitRegisters{};
    std::uint64_t ExitFrameAddress = 0;
    std::vector<std::byte> ExitFrame;

    Session(const std::filesystem::path& main, const std::filesystem::path& guest,
            unsigned mode = 0, std::uint64_t oracle = ArithmeticResult, bool returnParentTls = false) {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "Translated thread fixture requires native modern TCG");
        const auto processExit = [this, runtime = std::weak_ptr<Cpu::GuestThreads>(Threads)](int status) {
            const auto owner = runtime.lock();
            if (!owner) throw std::runtime_error("Guest thread process exit owner expired");
            const auto state = lifecycle();
            ProcessExitObserved = true;
            ++ProcessExitCalls;
            ObservedExitStatus = status;
            ExitBeforeFini = state[0] == 1 && state[1] == 0 && (state[2] == 2 || state[2] == 3);
            for (std::size_t index = 0; index < PreservedRegisters.size(); ++index)
                ExitRegisters[index] = Machine.Get(PreservedRegisters[index]);
            const auto rsp = Machine.Get(Cpu::Register::Rsp);
            const auto mappings = Machine.Mappings();
            const auto stack = std::find_if(mappings.begin(), mappings.end(), [&](const auto& mapping) {
                return !mapping.Borrowed && mapping.Permissions == rw && rsp >= mapping.Address &&
                    rsp - mapping.Address >= 128 && rsp - mapping.Address < mapping.Size;
            });
            require(stack != mappings.end(), "Process exit lacks its actual owned writable caller stack");
            ExitFrameAddress = rsp - 128;
            ExitFrame.resize(stack->Size - (ExitFrameAddress - stack->Address));
            Machine.Read(ExitFrameAddress, ExitFrame);
            owner->ProcessExitFromHostCall(status);
        };
        Libc.SetProcessExitHandler(processExit);
        Lifecycle.SetProcessExitHandler(processExit);
        const std::array dependencies{Cpu::SceModuleFile{guest, GuestBias}};
        const std::array hosts{
            Cpu::SceHostModule{"libkernel.prx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}},
            Cpu::SceHostModule{"libc.prx", {"libc", 0, 1, 1}, {{"libc", 0, 1}}}};
        Graph = std::make_unique<Cpu::SceModules>(Machine, Cpu::SceModuleFile{main, MainBias}, dependencies, hosts,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                require(type == 2, "Thread fixture attempted a host data or TLS provider");
                if (const auto gate = Kernel.Resolve(import, type)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = Lifecycle.Resolve(import)) return Cpu::SceResolvedImport{*gate, type};
                return Cpu::SceResolvedImport{Libc.Resolve(import), type};
            });
        require(Graph->Tls() && Graph->Tls()->ModuleCount() == 2, "Thread fixture lost its genuine two-module TLS graph");
        ReceiptAddress = MainBias + exported(Graph->Modules()[0], "eaCMHxii4rw", 1).Value;
        EventsAddress = MainBias + exported(Graph->Modules()[0], "QIQLlUDAU-k", 1).Value;
        LifecycleAddress = GuestBias + exported(Graph->Modules()[1], "ij1xrwBH1O0", 1).Value;
        require(receipt() == std::array<std::uint64_t, 36>{}, "Loading executed guest thread code or callback bodies");
        require(lifecycle() == std::array<std::uint64_t, 24>{}, "Loading executed dependency lifecycle code");
        Cpu::SetupSceEntry(Machine, Graph->Main(), {"thread-homebrew", std::to_string(mode), std::to_string(oracle)},
                           Graph->EntryTerminationGate());
        auto factory = Graph->ThreadTlsFactory();
        if (returnParentTls) factory = [this](std::uint64_t) {
            ++TlsFactoryCalls;
            return Graph->Tls();
        };
        Threads->AdoptInitial({Graph->Main().Entry, Graph->InitialStack(), Graph->Tls(), std::move(factory)});
        Graph->SetExecutor(Threads->ModuleExecutor());
        std::array<std::uint64_t, PreservedRegisters.size()> entryRegisters{};
        for (std::size_t index = 0; index < PreservedRegisters.size(); ++index)
            entryRegisters[index] = Machine.Get(PreservedRegisters[index]);
        const auto frame = Machine.Get(Cpu::Register::Rsp) - 128;
        const auto stack = Graph->InitialStack();
        std::vector<std::byte> entryFrame(stack.Address + stack.Size - frame);
        Machine.Read(frame, entryFrame);
        const auto initialThread = Threads->ActiveThread();
        Graph->InitializeDependencies(InitArguments[0], InitArguments[1], InitArguments[2]);
        for (std::size_t index = 0; index < PreservedRegisters.size(); ++index)
            require(Machine.Get(PreservedRegisters[index]) == entryRegisters[index], "Actual dependency initializer corrupted an entry register");
        std::vector<std::byte> observedFrame(entryFrame.size());
        Machine.Read(frame, observedFrame);
        require(observedFrame == entryFrame, "Actual dependency initializer corrupted entry arguments, return word or red zone");
        const auto initialized = lifecycle();
        require(Graph->Modules()[1].Init && Graph->Modules()[1].Fini && initialized[0] == 1 &&
                initialized[1] == 0 && initialized[2] == 0 && initialized[3] == initialThread &&
                initialized[4] == Graph->Tls()->FsBase() && initialized[5] == Threads->ActiveErrnoAddress() &&
                initialized[6] == 0 && initialized[7] == 0x8877665544332211ULL && initialized[8] == 0 &&
                initialized[9] == 0x59687786 && initialized[17] == InitArguments[0] &&
                initialized[18] == InitArguments[1] && initialized[19] == InitArguments[2] && initialized[23] == 0,
                "Compiled DT_INIT did not run once on the adopted initial guest with pristine relocated TLS and actual ABI arguments");
        require(receipt() == std::array<std::uint64_t, 36>{}, "Dependency initialization reached main entry prematurely");
        Machine.Map(Edge, 8192, rw);
        for (std::size_t index = 0; index < EdgeGuard.size(); ++index) EdgeGuard[index] = std::byte(0xa0 + index);
        Machine.Write(Edge + 4094, EdgeGuard);
        Machine.Protect(Edge + 4096, 4096, Cpu::Permission::Read);
    }

    bool Withdrawn = false;
    void withdraw() { Threads->Withdraw(); Withdrawn = true; }
    ~Session() { if (!Withdrawn) Threads->Withdraw(); }

    std::array<std::uint64_t, 36> receipt() const {
        std::array<std::uint64_t, 36> data{};
        Machine.Read(ReceiptAddress, std::as_writable_bytes(std::span(data)));
        return data;
    }

    std::array<std::uint64_t, 6> events() const {
        std::array<std::uint64_t, 6> data{};
        Machine.Read(EventsAddress, std::as_writable_bytes(std::span(data)));
        return data;
    }

    std::array<std::uint64_t, 24> lifecycle() const {
        std::array<std::uint64_t, 24> data{};
        Machine.Read(LifecycleAddress, std::as_writable_bytes(std::span(data)));
        return data;
    }

    void edgeUnchanged() const {
        std::array<std::byte, 16> observed{};
        Machine.Read(Edge + 4094, observed);
        require(observed == EdgeGuard, "Rejected cross-page output published a partial handle or join result");
    }
};

void freshExecution(Session& session, const std::array<std::uint64_t, 36>& state,
                    const std::array<std::uint64_t, 24>& lifecycle) {
    session.withdraw();
    constexpr std::uint64_t fresh = 0x4000000, result = fresh + 4096;
    constexpr std::array<unsigned char, 11> code{0xb8, 0x29, 0, 0, 0, 0x83, 0xc0, 1, 0x48, 0x89, 0x07};
    session.Machine.Map(fresh, 4096, rw);
    session.Machine.Map(result, 4096, rw);
    session.Machine.Write(fresh, std::as_bytes(std::span(code)));
    session.Machine.Protect(fresh, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);
    session.Machine.Set(Cpu::Register::Rsp, result + 4096 - 16);
    session.Machine.Set(Cpu::Register::Rdi, result);
    session.Machine.Set(Cpu::Register::Rax, 0xfeedface);
    require(session.Machine.Run(fresh, fresh + code.size(), 100) == Cpu::StopReason::Address &&
            session.Machine.Get(Cpu::Register::Rip) == fresh + code.size() &&
            session.Machine.Get(Cpu::Register::Rax) == 42,
            "Withdraw after compiled terminal exit retained a suspended guard or resumed old guest code");
    std::uint64_t stored{};
    session.Machine.Read(result, std::as_writable_bytes(std::span(&stored, 1)));
    require(stored == 42 && session.receipt() == state && session.lifecycle() == lifecycle,
            "Fresh guest run failed its independent result or replayed withdrawn thread callbacks");
}

void translatedLifecycle(const std::filesystem::path& main, const std::filesystem::path& guest, bool wrongOracle,
                         unsigned mode = 0) {
    Session session(main, guest, mode, ArithmeticResult + (wrongOracle ? 1 : 0));
    require(session.Graph->RunMain(2000000, 100000) == Cpu::StopReason::Exit &&
            session.Machine.ExitCode() == (wrongOracle ? 77 : 0),
            "Compiled thread program rejected scheduling, TLS, guard, arithmetic or independent result oracle");
    const auto state = session.receipt();
    require(session.events() == std::array<std::uint64_t, 6>{1, 2, 3, 4, 5, 6} && state[0] == 6 && state[1] == 0,
            "Guest did not execute P-setup/C-start/P-resume/C-resume/C-dtor/P-join exactly once in order");
    require(state[2] && state[3] && state[2] != state[3] && state[4] && state[5] && state[4] != state[5],
            "Created guest shared its parent's FS or errno address");
    require(state[6] && state[7] && state[6] != state[7] && state[8] == state[7],
            "Translated destructor lost the active child identity");
    require(state[9] == 1 && state[10] == 0 && state[11] == 0,
            "Thread finalization ran late, more than once, or invoked count/report without a caller");
    require(state[12] == ArithmeticResult && state[13] == 0x8877665544332211ULL,
            "Compiled arithmetic or full 64-bit child return disagrees with independent literal results");
    require(state[14] == 0x1122334455667798ULL && state[15] == 0x8877665544332231ULL && state[16] == 7 && state[17] == 17,
            "Parent TLS or errno changed while a child was scheduled and finalized");
    require(state[18] == 0x11223344556677c4ULL && state[19] == 0x8877665544332260ULL && state[20] == 15 && state[21] == 44,
            "Child destructor did not use retained child TLS, BSS and errno");
    require(state[22] == 0x13579bdf2468ace0ULL && state[23] == 0xeca86420fdb97531ULL &&
            state[24] == 0x1020304050607080ULL && state[25] == 0x8070605040302010ULL,
            "Create or join wrote outside its eight-byte output");
    require(state[26] == 0xabcdef0198765432ULL && state[27] != state[28] && state[29] != state[30],
            "Child argument or compiler-generated active TLS addresses were truncated or shared");
    require(state[33] == 0x22334455667788c9ULL && state[34] == 0x22334455667788d9ULL,
            "Compiler TLSGD resolver was pinned to a parent or mutable template");
    const auto lifecycle = session.lifecycle();
    require(session.ProcessExitObserved && session.ExitBeforeFini && lifecycle[0] == 1 && lifecycle[1] == 1 &&
            lifecycle[2] == 2 && lifecycle[3] == state[6] && lifecycle[10] == state[6] &&
            lifecycle[4] == state[2] && lifecycle[11] == state[2] &&
            lifecycle[5] == state[4] && lifecycle[12] == state[4] && lifecycle[13] == 17 &&
            lifecycle[14] == 0x8877665544332231ULL && lifecycle[15] == 7 && lifecycle[16] == 0x59687786 &&
            lifecycle[20] == 0 && lifecycle[21] == 0 && lifecycle[22] == 0 && lifecycle[23] == 0,
            "Compiled DT_FINI ran before real process exit or lost the retained initial guest identity, TLS, errno or ABI");
    for (std::size_t index = 0; index < PreservedRegisters.size(); ++index)
        require(session.Machine.Get(PreservedRegisters[index]) == session.ExitRegisters[index],
                "Actual dependency finalizer corrupted the paused entry's registers");
    std::vector<std::byte> exitFrame(session.ExitFrame.size());
    session.Machine.Read(session.ExitFrameAddress, exitFrame);
    require(exitFrame == session.ExitFrame, "Actual dependency finalizer corrupted the paused entry frame or red zone");
    session.edgeUnchanged();
    if (mode == 12) freshExecution(session, state, lifecycle);
}

void workerProcessExit(const std::filesystem::path& main, const std::filesystem::path& guest) {
    for (const auto mode : {13u, 14u}) {
        Session session(main, guest, mode);
        const auto expectedStatus = mode == 13 ? 23 : 24;
        require(session.Graph->RunMain(2000000, 100000) == Cpu::StopReason::Exit &&
                session.Machine.ExitCode() == expectedStatus && session.ProcessExitCalls == 1 &&
                session.ObservedExitStatus == expectedStatus && session.ProcessExitObserved && session.ExitBeforeFini,
                "Compiled worker process exit did not terminate the process with its exact requested status");
        const auto state = session.receipt();
        const auto lifecycle = session.lifecycle();
        require(session.events() == std::array<std::uint64_t, 6>{1, 2, 3, 4, 0, 0} && state[0] == 4 &&
                state[1] == 0 && state[9] == 0 && state[10] == 0 && state[11] == 0 && state[13] == 0 &&
                state[14] == 0 && state[15] == 0 && state[16] == 0 && state[17] == 0 && state[18] == 0 &&
                state[19] == 0 && state[20] == 0 && state[21] == 0 && state[24] == 0 && state[25] == 0 &&
                state[33] == 0 && state[34] == 0,
                "Process exit returned, resumed the joining parent, or fabricated thread destructors or join results");
        require(state[12] == ArithmeticResult && state[26] == 0xabcdef0198765432ULL &&
                state[2] && state[3] && state[2] != state[3] && state[4] && state[5] && state[4] != state[5] &&
                state[6] && state[7] && state[6] != state[7] && state[8] == 0 &&
                state[22] == 0x13579bdf2468ace0ULL && state[23] == 0xeca86420fdb97531ULL &&
                state[27] != state[28] && state[29] != state[30],
                "Worker process exit lost actual arithmetic, full-width argument, child identity or independent TLS");
        require(lifecycle[0] == 1 && lifecycle[1] == 1 && lifecycle[2] == 3 &&
                lifecycle[3] == state[6] && lifecycle[10] == state[7] && lifecycle[4] == state[2] &&
                lifecycle[11] == state[3] && lifecycle[5] == state[4] && lifecycle[12] == state[5] &&
                lifecycle[13] == 29 && lifecycle[14] == 0x8877665544332255ULL && lifecycle[15] == 13 &&
                lifecycle[16] == 0x59687786 && lifecycle[20] == 0 && lifecycle[21] == 0 &&
                lifecycle[22] == 0 && lifecycle[23] == 0,
                "Compiled DT_FINI failed to run exactly once on the exiting worker with its actual TLS, errno and ABI");
        for (std::size_t index = 0; index < PreservedRegisters.size(); ++index)
            require(session.Machine.Get(PreservedRegisters[index]) == session.ExitRegisters[index],
                    "Worker process finalizer corrupted the paused exit registers");
        std::vector<std::byte> frame(session.ExitFrame.size());
        session.Machine.Read(session.ExitFrameAddress, frame);
        require(frame == session.ExitFrame, "Worker process finalizer corrupted its actual exit frame or red zone");
        std::int32_t parentError{};
        session.Machine.Read(state[4], std::as_writable_bytes(std::span(&parentError, 1)));
        require(parentError == 17, "Worker process finalizer modified the parked parent's errno");
        session.edgeUnchanged();
        freshExecution(session, state, lifecycle);
    }
}

void arithmeticBudgetStop(const std::filesystem::path& main, const std::filesystem::path& guest) {
    Session session(main, guest);
    const auto& arithmetic = exported(session.Graph->Modules()[0], "8h9-NrHxAh8", 2);
    require(session.Graph->RunMain(16000, 100000) == Cpu::StopReason::InstructionLimit,
            "Bounded compiled thread arithmetic did not preserve the shared instruction budget");
    const auto pc = session.Machine.Get(Cpu::Register::Rip);
    require(pc >= MainBias + arithmetic.Value && pc - MainBias - arithmetic.Value < arithmetic.Size,
            "Budget keeper did not stop in actual compiled vector/long-double arithmetic");
    const auto state = session.receipt();
    require(state[0] == 3 && state[9] == 0 && state[12] == 0 && state[13] == 0,
            "Interrupted arithmetic invented child completion, destructor execution or join result");
    require(session.events() == std::array<std::uint64_t, 6>{1, 2, 3, 0, 0, 0},
            "Budget interruption crossed the child finalization or join boundary");
}

void rejectedFactoryPreservesParent(const std::filesystem::path& main, const std::filesystem::path& guest) {
    Session session(main, guest, 0, ArithmeticResult, true);
    const auto before = session.Machine.Mappings();
    const auto parent = session.Graph->Tls();
    const auto allocation = parent->Allocation();
    std::string diagnostic;
    try { session.Graph->RunMain(2000000, 100000); }
    catch (const std::exception& error) { diagnostic = error.what(); }
    require(!diagnostic.empty() && session.TlsFactoryCalls == 1,
            "Foreign TLS factory did not reject at the actual compiled guest create");
    require(sameMappings(before, session.Machine.Mappings()),
            "Rejected TLS factory retired the parent's allocation or leaked child mappings");
    session.Machine.CheckAccess(allocation.Address, allocation.Size, rw);
    require(session.Machine.Get(Cpu::Register::FsBase) == parent->FsBase(),
            "Rejected TLS factory lost the live parent FS context");
    const auto word = [&](std::uint64_t address) {
        std::uint64_t value{};
        session.Machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    };
    const auto tlsWord = [&](std::size_t module, const char* nid) {
        const auto& record = session.Graph->Modules()[module];
        return word(parent->Resolve(record.TlsModuleId, exported(record, nid, 6).Value));
    };
    require(tlsWord(0, "GvF-Bi4Awf8") == 0x1122334455667798ULL &&
            tlsWord(0, "yDUus272Ruw") == 0x22334455667788c9ULL &&
            tlsWord(0, "VegplOr1lzM") == 7 &&
            tlsWord(1, "Y4HdaqFKUqM") == 0x8877665544332231ULL &&
            tlsWord(1, "Y4GBk7lhoKQ") == 7,
            "Rejected TLS factory destroyed or restored the parent's actual guest TLS mutations");
    const auto state = session.receipt();
    std::int32_t error{};
    session.Machine.Read(state[4], std::as_writable_bytes(std::span(&error, 1)));
    require(error == 17 && state[2] == parent->FsBase() && state[0] == 1 && state[1] == 0 &&
            state[7] == 0 && state[9] == 0 && state[10] == 0 && state[11] == 0 && state[13] == 0 &&
            session.events() == std::array<std::uint64_t, 6>{1, 0, 0, 0, 0, 0},
            "Rejected TLS factory executed a child, callback, join or ordinary guest completion");
    session.edgeUnchanged();
    require(diagnostic.find("guest TLS factory did not return a new owned allocation at the requested address") != std::string::npos,
            diagnostic.c_str());
}

void rejectedJoinPreservesChild(const std::filesystem::path& main, const std::filesystem::path& guest) {
    Session session(main, guest, 11);
    std::string diagnostic;
    try { session.Graph->RunMain(2000000, 100000); }
    catch (const std::exception& error) { diagnostic = error.what(); }
    require(diagnostic.find("guest join output overlaps the target's retiring allocation") != std::string::npos,
            "Join into target-owned storage reported success or an unrelated failure");
    const auto state = session.receipt();
    require(state[0] == 3 && state[1] == 0 && state[7] && state[9] == 0 && state[12] == 0 && state[13] == 0 &&
            state[10] == 0 && state[11] == 0 &&
            session.events() == std::array<std::uint64_t, 6>{1, 2, 3, 0, 0, 0},
            "Rejected target-owned join output finalized, retired or resumed its child");
    require(state[5] && state[5] != state[4], "Compiled child did not publish its actual independent errno address");
    session.Machine.CheckAccess(state[5], 8, rw);
    std::uint64_t errorStorage{};
    session.Machine.Read(state[5], std::as_writable_bytes(std::span(&errorStorage, 1)));
    require(errorStorage == 29, "Rejected join modified or unmapped the child's return buffer");
    require(session.Machine.Get(Cpu::Register::FsBase) == state[2],
            "Rejected join left the child context active instead of its caller");
    session.edgeUnchanged();
}

void preflightFailures(const std::filesystem::path& main, const std::filesystem::path& guest) {
    struct Case { unsigned Mode; const char* Diagnostic; bool NoChild; };
    constexpr std::array cases{
        Case{2, "callback PC is null", true}, Case{3, "registration is duplicated", true},
        Case{4, "permissions 4", true}, Case{5, "permissions 2", true},
        Case{6, "attributes are unsupported", true}, Case{7, "permissions 2", false},
        Case{8, "cannot join itself", true}, Case{9, "unknown or retired guest thread handle", true},
        Case{10, "unknown or retired guest thread handle", false}};
    for (const auto& value : cases) {
        Session session(main, guest, value.Mode);
        const auto before = session.Machine.Mappings();
        bool rejected = false;
        try { session.Graph->RunMain(2000000, 100000); }
        catch (const std::exception& error) {
            require(std::string_view(error.what()).find(value.Diagnostic) != std::string_view::npos, error.what());
            rejected = true;
        }
        require(rejected, "Invalid thread registration, entry, output or join reached ordinary guest completion");
        session.edgeUnchanged();
        const auto state = session.receipt();
        require(state[10] == 0 && state[11] == 0, "Rejected thread operation invoked count/report");
        if (value.NoChild) {
            require(sameMappings(before, session.Machine.Mappings()), "Rejected preflight published child stack or TLS mappings");
            require(state[7] == 0 && state[9] == 0 && state[0] <= 1, "Rejected preflight executed a child or destructor");
        }
        if (value.Mode == 7)
            require(state[0] == 1 && state[7] == 0 && state[9] == 0,
                    "Invalid join output blocked and executed its child before preflight");
        if (value.Mode == 10)
            require(state[0] == 5 && state[9] == 1, "Double join replayed or skipped actual child finalization");
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: GuestThreadsTests main.elf ThreadGuest.prx");
        translatedLifecycle(argv[1], argv[2], false);
        translatedLifecycle(argv[1], argv[2], true);
        translatedLifecycle(argv[1], argv[2], false, 12);
        workerProcessExit(argv[1], argv[2]);
        arithmeticBudgetStop(argv[1], argv[2]);
        preflightFailures(argv[1], argv[2]);
        rejectedFactoryPreservesParent(argv[1], argv[2]);
        rejectedJoinPreservesChild(argv[1], argv[2]);
        std::cout << "PASS actual compiled guest create/yield/blocked join, immutable TLS/errno, arithmetic slices, finalization and preflight\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
