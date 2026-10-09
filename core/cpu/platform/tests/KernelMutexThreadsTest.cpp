#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

// Test-audit contract: actual SCE PLT mutex calls park/resume two GuestThreads with
// exclusive ownership, exact guest errors and unchanged suspended-call ABI. Removal
// of blocking/wake, early recursive wake, ownership transfer, frame validation or
// domain cancellation breaks independent payload/state/return oracles here.
// Existing flat KernelPrimitivesTest uses a host-supplied integer identity and
// explicitly rejects contention; GuestThreadsTests has joins, but no mutex owners.
// No production seam: SceModules' typed resolver and public scheduler own the boundary.
namespace {
constexpr std::uint64_t Bias = 0x1000000;
constexpr std::uint64_t Busy = 0x80020010, Perm = 0x80020001;
using State = std::array<std::uint64_t, 64>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* diagnostic) {
    try { action(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("Expected qualified mutex rejection");
}
Cpu::SceImport scoped(std::string_view nid) {
    Cpu::SceImport value;
    value.Nid = nid; value.LibraryName = value.ModuleName = "libkernel";
    value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
    value.LibraryId = value.ModuleId = 1;
    return value;
}
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports threadImports{machine, threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    std::unique_ptr<Cpu::Platform::KernelPrimitives> mutexes;
    std::unique_ptr<Cpu::SceModules> graph;
    std::uint64_t receiptAddress = 0;
    std::set<std::uint64_t> gates;
    bool withdrawn = false;
    Session(const char* path, unsigned mode, bool candidate = false) {
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
                "Mutex scheduler fixture requires native Modern QEMU TCG");
        if (candidate) mutexes = std::make_unique<Cpu::Platform::KernelPrimitives>(machine,
                [this] { return threads->ActiveThread(); });
        else mutexes = std::make_unique<Cpu::Platform::KernelPrimitives>(machine, threads);
        lifecycle.SetProcessExitHandler([runtime = std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
            const auto owner = runtime.lock();
            require(bool(owner), "Mutex fixture process-exit scheduler expired");
            owner->ProcessExitFromHostCall(code);
        });
        const std::array hosts{Cpu::SceHostModule{"libkernel.prx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}}};
        graph = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{path, Bias},
                std::span<const Cpu::SceModuleFile>{}, hosts,
                [&](const Cpu::SceImport& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                    require(type == 2, "Actual linked mutex import lost its mandatory ELF function type");
                    if (const auto gate = mutexes->Resolve(import, type)) {
                        gates.insert(*gate); return Cpu::SceResolvedImport{*gate, type};
                    }
                    if (const auto gate = threadImports.Resolve(import, type)) return Cpu::SceResolvedImport{*gate, type};
                    if (const auto gate = lifecycle.Resolve(import)) return Cpu::SceResolvedImport{*gate, type};
                    return std::nullopt;
                });
        const auto& parsed = graph->Modules()[0].Image;
        require(parsed.Imports.size() == 14 && parsed.NeededFiles.empty() && parsed.NeededModules.size() == 1 &&
                parsed.Tls && gates.size() == 9, "Linked fixture lost its actual kernel/thread import and TLS graph");
        require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(), parsed.RelocationTypes.end()) ==
                std::set<std::uint32_t>{7, 8}, "Mutex fixture lost its genuine PLT/RELATIVE relocations");
        for (const auto& item : parsed.Exports) if (item.Type == 1 && item.Size == sizeof(State)) {
            require(!receiptAddress, "Ambiguous compiled fixture receipt"); receiptAddress = Bias + item.Value;
        }
        require(receiptAddress && receipt() == State{}, "Loading ran guest fixture code or lost the receipt");
        put(receiptAddress + 63 * 8, mode);
        Cpu::SetupSceEntry(machine, graph->Main(), {"public-kernel-mutex-fixture"}, graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry, graph->InitialStack(), graph->Tls(), graph->ThreadTlsFactory()});
        graph->SetExecutor(threads->ModuleExecutor());
        graph->InitializeDependencies();
    }
    ~Session() { if (!withdrawn) threads->Withdraw(); }
    State receipt() const {
        State state{}; machine.Read(receiptAddress, std::as_writable_bytes(std::span(state))); return state;
    }
    void put(std::uint64_t address, std::uint64_t value) { machine.Write(address, std::as_bytes(std::span(&value, 1))); }
    std::uint64_t get(std::uint64_t address) const {
        std::uint64_t value; machine.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
    void withdraw() { threads->Withdraw(); withdrawn = true; }
};

void contention(const char* path, unsigned mode, bool candidate) {
    Session session(path, mode, candidate);
    require(session.graph->RunMain(500000, 10000) == Cpu::StopReason::Exit && session.machine.ExitCode() == 0,
            "Real contending mutex threads did not finish");
    const auto s = session.receipt();
    require(s[0] == 0x4d5554455857414bULL && s[1] && s[2] && s[1] != s[2] && s[47] == s[2],
            "Mutex oracle did not execute two actual distinct guest identities");
    require(s[4] == 1 && s[18] == 1 && s[5] == 1 && s[16] == 1 && s[17] == 1 && s[51] == 1,
            "Waiter ran before final owner unlock or owner could not progress while waiter parked");
    require(s[10] == Busy && s[11] == Perm && s[12] == Busy && s[20] == Busy && s[21] == Perm && s[22] == Busy,
            "Foreign trylock/unlock/destroy changed the exclusive guest mutex ownership");
    require(s[7] == 0 && s[6] == (0x2131415161718191ULL ^ 0xa55aa55aa55aa55aULL),
            "Independent exclusive mutation/data oracle failed");
    require(s[13] == 0 && s[14] == 0 && s[15] == 0 && s[32] && s[33],
            "Blocked lock resumed with wrong gate return, callee-saved registers, stack or spill canary");
    require(s[23] == 0 && s[24] == 0x5566778899aabbccULL && s[29] == 2 && s[30] == 0,
            "Guest join return or final opaque mutex/attribute cleanup differs");
    for (const auto index : {3, 25, 26, 27, 28, 36, 37, 38, 39, 40, 46, 50})
        require(s[index] == 0, "Guest success status differs at lock/attribute/lifetime boundary");
    require(s[41] > 2 && s[41] != s[42], "Mutex did not publish an opaque indirect slot token");
    if (mode == 1) {
        require(s[19] == 1, "Recursive partial unlock woke the contending waiter before depth reached zero");
        for (const auto index : {43, 44, 45, 48}) require(s[index] == 0, "Recursive lock/trylock/unlock status differs");
    }
}

void corruptedContinuation(const char* path) {
    Session session(path, 2);
    rejects([&] { session.graph->RunMain(500000, 10000); }, "return word changed");
    const auto s = session.receipt();
    require(s[18] == 1 && s[32] && s[33] && session.get(s[32]) == 0x1122334455667788ULL,
            "Corruption control did not alter the blocked waiter's actual executable return word");
    session.machine.CheckAccess(s[33], 1, Cpu::Permission::Execute);
    require(s[16] == 0 && s[17] == 0 && s[6] == 0x1020304050607080ULL && s[0] == 0,
            "Corrupt lock continuation published waiter acquisition, payload mutation or guest completion");
    Cpu::GuestPhaseBudget retry(10000);
    require(session.threads->RunEntry(retry) == Cpu::StopReason::Requested && retry.Consumed() == 0 &&
            session.receipt() == s,
            "Rejected corrupt lock continuation retained a runnable success path or executed guest instructions");
}

void cancellationAndReplacement(const char* path) {
    Session session(path, 3);
    Cpu::GuestPhaseBudget budget(30000);
    require(session.threads->RunEntry(budget) == Cpu::StopReason::InstructionLimit,
            "Bounded owner spin did not park real owner and mutex waiter");
    const auto before = session.receipt();
    require(before[18] == 1 && before[49] && before[16] == 0 && before[41] > 2,
            "Lifetime control did not retain a real blocked mutex continuation");
    const auto oldGates = session.gates;
    session.mutexes.reset();
    for (const auto gate : oldGates) rejects([&] {
        session.machine.CheckAccess(gate, 1, Cpu::Permission::Execute);
    }, "Guest access denied");
    session.mutexes = std::make_unique<Cpu::Platform::KernelPrimitives>(session.machine, session.threads);
    const auto replaced = session.mutexes->Resolve(scoped("9UK1vLZQft4"), 2);
    require(replaced && oldGates.contains(*replaced), "Replacement control did not reuse the retired gate page");
    Cpu::GuestPhaseBudget after(10000);
    require(session.threads->RunEntry(after) == Cpu::StopReason::Requested && after.Consumed() == 0,
            "Provider withdrawal resumed an old waiter through a replacement gate");
    require(session.receipt() == before && session.get(before[42]) == before[41],
            "Provider cancellation fabricated success, mutated data or rewrote a stale opaque slot");
    session.withdraw();
    require(session.receipt() == before, "Runtime withdrawal fabricated guest lock completion");
    session.mutexes.reset(); /* Safe even after the scheduler domain has been withdrawn. */
}

void ownerExit(const char* path) {
    Session session(path, 4);
    rejects([&] { session.graph->RunMain(500000, 10000); }, "no runnable work and cannot make progress");
    const auto s = session.receipt();
    require(s[1] && s[2] && s[1] != s[2] && s[10] == 0 && s[35] == 1 && s[16] == 0 &&
            s[6] == 0x1020304050607080ULL && s[0] == 0,
            "Unsupported owner exit fabricated lock acquisition, unlock recovery or payload mutation");
}

void targetAdmission() {
    Cpu::Machine machine;
    auto threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::Platform::TargetKernelMutexes target(machine, threads);
    for (const auto& item : Cpu::Platform::KernelPrimitiveInventory()) {
        auto exact = scoped(item.Nid);
        exact.LibraryId = 44; exact.ModuleId = 24;
        require(target.Resolve(exact, 2).has_value(), "Mutex row rejected");
        // Import-table ids are per-image: any title, libc or module row is admitted.
        for (const auto ids : {std::pair{0, 1}, std::pair{4, 5}, std::pair{7, 9}}) {
            auto other = exact; other.LibraryId = ids.first; other.ModuleId = ids.second;
            require(target.Resolve(other, 2).has_value(), "Another image's mutex import-table ids were refused");
        }
        for (const auto type : {0, 1, 6}) rejects([&] { target.Resolve(exact, type); }, "scope/version/type");
        for (unsigned field = 0; field < 5; ++field) {
            auto wrong = exact;
            if (field == 0) wrong.ModuleName = "libc";
            if (field == 1) wrong.LibraryName = "libc";
            if (field == 2) wrong.LibraryVersion = 2;
            if (field == 3) wrong.ModuleMajor = 2;
            if (field == 4) wrong.ModuleMinor = 2;
            rejects([&] { target.Resolve(wrong, 2); }, "scope/version/type");
        }
    }
    require(!target.Resolve(scoped("AAAAAAAAAAA"), 2), "Target mutex provider fabricated another family");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--candidate"),
                "Usage: KernelMutexThreadsTest packaged.elf [--candidate]");
        contention(argv[1], 0, argc == 3);
        contention(argv[1], 1, false);
        corruptedContinuation(argv[1]);
        cancellationAndReplacement(argv[1]);
        ownerExit(argv[1]);
        targetAdmission();
        std::cout << "PASS actual linked SCE guest mutex contention, exclusivity, recursive depth, ABI, cancellation, owner-exit rejection and exact target admission\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
