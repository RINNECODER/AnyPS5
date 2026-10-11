#include "KernelThreadExtras.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceKernelTimeImports.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <chrono>
#include <cstring>
#include <span>
#include <utility>
#include <iostream>
#include <stdexcept>
#include <string>

// Test-audit primary boundary: actual linked libkernel PLT calls from a public
// synthetic guest executing in GuestThreads. Each case checks guest-visible
// return codes, ordering traces and outputs that a regression would change:
// rwlock writer preference/recursion/EDEADLK/EPERM/EBUSY/ETIMEDOUT, once
// exactly-once with a blocked concurrent caller, TSD isolation, destructor
// iterations and key exhaustion, futex-like wait/wake/timeout, thread attribute
// fields honored at creation (4 MiB stack, caller stack, detach), thread
// controls returning SCE errors instead of host aborts, module information from
// the loaded image (eh_frame_hdr, segments), and debug exceptions continuing.
namespace {
constexpr std::uint64_t Bias = 0x1000000;
constexpr std::uint64_t Perm = 0x80020001, Search = 0x80020003, Deadlock = 0x8002000b, Fault = 0x8002000e,
    Busy = 0x80020010, Invalid = 0x80020016, Again = 0x80020023, TimedOut = 0x8002003c;
using State = std::array<std::uint64_t, 128>;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports threadImports{machine, threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    Cpu::SceKernelTimeImports time{machine, threads};
    std::unique_ptr<Cpu::Platform::KernelThreadExtras> extras;
    std::unique_ptr<Cpu::SceModules> graph;
    std::uint64_t address = 0;
    Session(const char* path, unsigned mode) {
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
                "Thread extras fixture requires native Modern QEMU TCG");
        extras = std::make_unique<Cpu::Platform::KernelThreadExtras>(machine, threads);
        lifecycle.SetProcessExitHandler([runtime = std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
            const auto owner = runtime.lock(); require(bool(owner), "Thread extras scheduler expired");
            owner->ProcessExitFromHostCall(code);
        });
        const std::array hosts{Cpu::SceHostModule{"libkernel.prx", {"libkernel", 1, 1, 1}, {{"libkernel", 1, 1}}}};
        graph = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{path, Bias},
            std::span<const Cpu::SceModuleFile>{}, hosts,
            [&](const Cpu::SceImport& row, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                if (const auto gate = extras->Resolve(row, type, 0)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = threadImports.ResolvePriority(row, type)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = threadImports.Resolve(row, type)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = lifecycle.Resolve(row)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = time.Resolve(row)) return Cpu::SceResolvedImport{*gate, type};
                return std::nullopt;
            });
        extras->SetModuleSource([this] { return graph->Modules(); });
        for (const auto& item : graph->Modules()[0].Image.Exports)
            if (item.Type == 1 && item.Size == sizeof(State)) address = Bias + item.Value;
        require(address != 0, "Thread extras fixture lost its receipt");
        put(address + 8, mode);
        Cpu::SetupSceEntry(machine, graph->Main(), {"public-thread-extras-fixture"}, graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry, graph->InitialStack(), graph->Tls(), graph->ThreadTlsFactory()});
        threads->SetOwnerBoundary([](bool) {}, std::chrono::milliseconds(2000));
        graph->SetExecutor(threads->ModuleExecutor()); graph->InitializeDependencies();
    }
    ~Session() { threads->Withdraw(); }
    void put(std::uint64_t where, std::uint64_t value) { machine.Write(where, std::as_bytes(std::span(&value, 1))); }
    State receipt() const { State value{}; machine.Read(address, std::as_writable_bytes(std::span(value))); return value; }
    State run() {
        Cpu::StopReason reason{};
        try { reason = graph->RunMain(50000000, 100000); }
        catch (...) { dump(); throw; }
        const auto r = receipt();
        if (reason != Cpu::StopReason::Exit || machine.ExitCode() != 0 || r[0] != 0x5854524153505448ULL) dump();
        require(reason == Cpu::StopReason::Exit && machine.ExitCode() == 0 && r[0] == 0x5854524153505448ULL && r[4] == 0,
                "Thread extras guest did not reach a clean process exit");
        return r;
    }
    void dump() const {
        const auto r = receipt();
        std::cerr << "thread extras receipt mode=" << r[1] << " failures=" << r[4] << " values:";
        for (unsigned i = 9; i < 64; ++i) std::cerr << ' ' << i << '=' << std::hex << r[i] << std::dec;
        std::cerr << " trace:";
        for (unsigned i = 0; i < r[5] && i < 16; ++i) std::cerr << ' ' << r[100 + i];
        std::cerr << '\n';
    }
};
void trace(const State& r, std::initializer_list<std::uint64_t> expected, const char* what) {
    require(r[5] == expected.size() && std::equal(expected.begin(), expected.end(), r.begin() + 100),
            std::string(what) + " ordering trace differs");
}
void rwlock(const char* path) {
    Session s(path, 1); const auto r = s.run();
    require(r[10] == 0 && r[11] == 0 && r[12] == Busy && r[13] == 0 && r[14] == 0 && r[15] == Perm,
            "Recursive readers, trywrlock EBUSY or unlock EPERM differ");
    require(r[16] == 0 && r[17] == Deadlock && r[18] == Busy && r[19] == Deadlock && r[20] == Busy && r[21] == 0,
            "Writer self-deadlock, tryrdlock EBUSY or busy destroy differ");
    require(r[9] > 1, "Static initializer did not publish a lazy handle");
    trace(r, {2, 3, 4}, "Writer preference");
    require(r[22] == 0, "A recursive reader was blocked behind a queued writer");
    require(r[23] == TimedOut && r[24] == TimedOut, "Timed rwlock waits did not expire with ETIMEDOUT");
    require(r[25] == 0 && r[26] == 1 && r[27] == Invalid && r[28] == 0 && r[29] == 0,
            "Destroyed sentinel, init or destroy differ");
    std::cout << "PASS rwlock recursion, writer preference, timed expiry and destroy\n";
}
void once(const char* path) {
    Session s(path, 2); const auto r = s.run();
    require(r[40] == 0 && r[41] == 0 && r[42] == 0 && r[45] == 1 && r[46] == 1,
            "Once ran its routine more than once or returned an error");
    trace(r, {10, 11, 12, 13}, "Concurrent once");
    require(r[43] == Invalid && r[44] == Invalid, "Null once control or routine was not EINVAL");
    std::cout << "PASS once exactly once with a blocked concurrent caller\n";
}
void tsd(const char* path) {
    Session s(path, 3); const auto r = s.run();
    require(r[10] == 0 && r[11] == 0 && r[12] && r[13] && r[12] != r[13], "Key creation differs");
    require(r[14] == 0 && r[15] == 0xabc && r[16] == 0xabc && r[60] == 0x1111 && r[61] == 0x3333,
            "Thread-specific values are not per thread");
    require(r[62] == 2 && r[50] == 0x1111 && r[51] == 0x2222, "Destructor did not rerun for a value set during destruction");
    require(r[17] == 0 && r[18] == 0 && r[19] == Invalid && r[20] == Invalid, "Deleted key was still usable");
    require(r[21] == 255 && r[22] == Again, "Key limit or EAGAIN differs");
    require(r[23] == 4, "Destructor iterations are not bounded at PTHREAD_DESTRUCTOR_ITERATIONS");
    require(r[24] == Invalid && r[25] == 0, "Invalid keys differ");
    std::cout << "PASS TSD isolation, destructor iterations and key limit\n";
}
void sync(const char* path) {
    Session s(path, 4); const auto r = s.run();
    require(r[10] == 0 && r[11] == 0 && r[12] == 0 && r[14] == 0 && r[15] == 0, "Wait/wake results differ");
    trace(r, {1, 2, 3, 4}, "SyncOnAddress wake");
    require(r[13] == TimedOut && r[16] == Invalid && r[17] == Invalid, "Timeout or invalid arguments differ");
    std::cout << "PASS SyncOnAddress compare-wait, wake, timeout and validation\n";
}
void controls(const char* path) {
    Session s(path, 5); const auto r = s.run();
    require(r[10] == 0 && r[11] == (4u << 20) && r[12] == Invalid && r[13] == 8192 && r[14] == 1 && r[15] == Invalid &&
            r[16] == 3 && r[17] == 0 && r[18] == 2 && r[19] == 4 && r[20] == 0, "Attribute setters/getters differ");
    require(r[30] == 3, "4 MiB stack attribute was not honored");
    require(r[21] == Invalid && r[22] == Search, "Joining a detached thread did not fail with EINVAL then ESRCH");
    require(r[25] == 0 && r[26] == 1 && r[27] == 1, "Thread name or rename differs");
    require(r[28] == 0 && r[29] == 500 && r[34] == Invalid && r[35] == 5, "Priority or affinity differ");
    require(r[23] == 0 && r[24] == Invalid, "Detach differs");
    require(r[36] == 0 && r[37] == 1 && r[38] == 5 && r[50] == (1u << 20), "AttrGet from a live thread differs");
    require(r[40] >= 0x10000 && r[49] >= 0x10000 && r[40] != r[49], "Thread ids are not distinct");
    require(r[41] == Search && r[42] == Search && r[43] == Invalid && r[51] == Deadlock,
            "Unknown thread, self join or unowned attribute was not an SCE error");
    require(r[44] == 0 && r[45] == 1, "Cancelled thread did not exit with PTHREAD_CANCELED");
    require(r[46] == 0 && r[48] == 1, "Caller-supplied stack was not used");
    std::cout << "PASS thread attributes and controls without host aborts\n";
}
void module(const char* path) {
    Session s(path, 6); const auto r = s.run();
    const auto& image = s.graph->Modules()[0].Image;
    std::uint64_t header = 0, first = 0;
    for (const auto& segment : image.Segments) {
        if (segment.Type == 0x6474e550) header = Bias + segment.Address;
        if (segment.Type == 1 && !first) first = Bias + segment.Address;
    }
    require(header != 0, "Fixture lost PT_GNU_EH_FRAME");
    require(r[10] == 0 && r[11] == header && r[12] > Bias && r[13] >= 2 && r[14] == first && r[19] != 0,
            "Module information from address differs from the loaded image");
    char name[17]{}; std::memcpy(name, &r[15], 16);
    require(std::string(name) == "kernel-thread-ex", "Module name differs");
    require(r[17] == 0 && r[18] == 1, "Module information for unwind differs");
    require(r[20] == Search && r[21] == Fault && r[22] == Invalid, "Module information errors differ");
    // sceSysmoduleGetModuleInfoForUnwind answers from the same guest module graph as the
    // libkernel row, only inside the libSceSysmodule scope.
    Cpu::SceImport kernelRow{"RpQJJVKTiFM", "libkernel", 0, "libkernel", 0, 1, 1, 1};
    Cpu::SceImport sysmoduleRow{"4fU5yvOkVG4", "libSceSysmodule", 0, "libSceSysmodule", 0, 1, 1, 1};
    const auto kernelGate = s.extras->Resolve(kernelRow, 2, 0), sysmoduleGate = s.extras->Resolve(sysmoduleRow, 2, 0);
    require(kernelGate && sysmoduleGate && *sysmoduleGate != *kernelGate, "libSceSysmodule unwind row did not resolve");
    sysmoduleRow.ModuleName = "libkernel";
    require(!s.extras->Resolve(sysmoduleRow, 2, 0), "libSceSysmodule unwind row resolved outside its own module");
    constexpr std::uint64_t Code = 0x60000000, Stack = Code + 0x1000, Output = Code + 0x2000;
    s.machine.Map(Code, 0x1000, Cpu::Permission::Read | Cpu::Permission::Execute);
    s.machine.Map(Stack, 0x3000, Cpu::Permission::Read | Cpu::Permission::Write);
    constexpr std::array<std::uint8_t, 7> call{0xff, 0x15, 0xfa, 0x0f, 0x00, 0x00, 0x90};  // call [rip+0x1000-6]
    s.machine.Write(Code, std::as_bytes(std::span(call)));
    const auto unwind = [&](std::uint64_t gate, std::uint64_t address, std::uint64_t output) {
        s.machine.Write(Code + 0x1000, std::as_bytes(std::span(&gate, 1)));
        s.machine.Set(Cpu::Register::Rdi, address); s.machine.Set(Cpu::Register::Rsi, 0);
        s.machine.Set(Cpu::Register::Rdx, output); s.machine.Set(Cpu::Register::Rsp, Stack + 0xff0);
        require(s.machine.Run(Code, Code + 6, 100) == Cpu::StopReason::Address, "Unwind gate did not return");
        std::array<std::byte, 0x130> bytes{};
        if (output) s.machine.Read(output, bytes);
        return std::pair{s.machine.Get(Cpu::Register::Rax), bytes};
    };
    const auto [kernelStatus, kernelInfo] = unwind(*kernelGate, first + 0x10, Output);
    const auto [sysmoduleStatus, sysmoduleInfo] = unwind(*sysmoduleGate, first + 0x10, Output + 0x200);
    require(kernelStatus == 0 && sysmoduleStatus == 0 && kernelInfo == sysmoduleInfo,
            "libSceSysmodule unwind record differs from sceKernelGetModuleInfoForUnwind");
    require(unwind(*sysmoduleGate, 0x10, Output).first == Search, "libSceSysmodule unwind found an unmapped address");
    std::cout << "PASS module information from the loaded image\n";
}
void debug(const char* path) {
    Session s(path, 7); const auto r = s.run();
    require(r[10] == 0 && r[11] == 0 && r[12] == 0, "Debug exceptions did not continue");
    std::cout << "PASS debug exceptions continue\n";
}
void posix(const char* path) {
    Session s(path, 8); const auto r = s.run();
    constexpr std::uint64_t NoEntry = 0x80020002;
    require(r[10] == 0 && r[11] == 0, "Loading an already present module by path did not return its handle and start result");
    require(r[12] == NoEntry && r[13] == Fault, "Unknown module or null path did not return ENOENT or EFAULT");
    require(r[14] == 0 && r[15] == 1 && r[16] == Search && r[17] == Search, "Dlsym differs from the module export table");
    require(r[18] == 0 && r[19] == Search, "Stop/unload differs");
    require(r[20] == 0 && r[21] == 0 && r[22] == 1 && r[23] == Invalid, "Trinity mode or UUID differ");
    require(r[24] == Invalid && r[25] == 0 && r[26] == 0 && r[27] == Invalid && r[28] == Search,
            "Thread-atexit references are not balanced per image");
    require(r[29] == 1, "getpid did not return a positive process id");
    require(r[30] == 0 && r[31] == 0 && r[32] == 0xffffffff && r[33] == 60, "Semaphore wait/timedwait on an empty semaphore differs");
    require(r[34] == 0 && r[35] == 0 && r[36] == 0 && r[37] == 0xffffffff && r[38] == 22, "Semaphore post/destroy differs");
    require(r[39] == 0xffffffff && r[59] == 22, "Oversized semaphore value was accepted");
    require(r[41] == 0 && r[42] == 1 && r[60] == 0, "Blocked sem_wait did not resume exactly after sem_post");
    require(r[62] == 0xffffffff && r[63] == 60, "Expired sem_timedwait did not report ETIMEDOUT in the waiter's errno");
    require(r[43] == 0 && r[44] == 0xffffffff, "getrusage differs");
    require(r[45] == 0 && r[46] == 0x1234 && r[47] == 0xffffffffffffffffULL && r[48] == 22, "signal() dispositions differ");
    require(r[49] == 0 && r[50] == 0 && r[51] == 0 && r[52] == 6 && r[53] == 0 && r[54] == 4 && r[55] == 0xffffffff,
            "Signal mask block/unblock/set differs");
    require(r[56] == 1 && r[57] == 0 && r[58] == 0, "Program-header address match differs");
    // libSceCoredump rows resolve only inside their own library scope.
    Cpu::SceImport row{"dei8oUx6DbU", "libSceCoredump", 0, "libkernel", 0, 1, 1, 1};
    require(s.extras->Resolve(row, 2, 0).has_value(), "libSceCoredump row did not resolve");
    row.LibraryName = "libkernel";
    require(!s.extras->Resolve(row, 2, 0).has_value(), "libSceCoredump row resolved in the libkernel scope");
    std::cout << "PASS module loading, process, semaphore, signal and coredump services\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: KernelThreadExtrasTest packaged.elf case");
        const std::string name = argv[2];
        if (name == "rwlock") rwlock(argv[1]);
        else if (name == "once") once(argv[1]);
        else if (name == "tsd") tsd(argv[1]);
        else if (name == "sync") sync(argv[1]);
        else if (name == "controls") controls(argv[1]);
        else if (name == "module") module(argv[1]);
        else if (name == "debug") debug(argv[1]);
        else if (name == "posix") posix(argv[1]);
        else throw std::runtime_error("Unknown thread extras case");
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    return 0;
}
