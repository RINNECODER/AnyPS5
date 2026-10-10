#include "KernelEventFlags.hpp"
#include "KernelEvents.hpp"
#include "KernelSemaphores.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

// Test-audit primary boundary: genuine linked SCE PLT calls from guest threads
// into the libkernel sync providers (#279). The guest records every observed
// return code, output word and timeout remainder; the harness asserts that no
// expectation failed and re-checks the values that distinguish a real
// implementation from a stub (cancel counts, consumed patterns, filters).
// Credible regressions: rejected FIFO/PRIO attributes, timed waits that return
// EINVAL or never expire, waiters stalling other guest threads, Cancel or
// Delete leaving waiters parked, wrong AND/OR/clear semantics, user events
// never waking a blocked WaitEqueue, timers that never fire, and parked waits
// that ignore a host stop request.
namespace {
constexpr std::uint64_t Bias = 0x1000000, Magic = 0x53594e4352454350ULL;
constexpr std::uint64_t Canceled = 0x80020055, Timedout = 0x8002003c, Access = 0x8002000d;
using State = std::array<std::uint64_t, 128>;
void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports threadImports{machine, threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    std::unique_ptr<Cpu::Platform::KernelSemaphores> semaphores;
    std::unique_ptr<Cpu::Platform::KernelEventFlags> flags;
    std::unique_ptr<Cpu::Platform::KernelEvents> queues;
    std::unique_ptr<Cpu::SceModules> graph;
    std::uint64_t address = 0;
    Session(const char* path, unsigned mode, unsigned variant = 0) {
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
                "Sync fixture requires native Modern QEMU TCG");
        semaphores = std::make_unique<Cpu::Platform::KernelSemaphores>(machine, threads);
        flags = std::make_unique<Cpu::Platform::KernelEventFlags>(machine, threads);
        queues = std::make_unique<Cpu::Platform::KernelEvents>(machine, threads);
        lifecycle.SetProcessExitHandler([runtime = std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
            const auto owner = runtime.lock();
            require(bool(owner), "Sync scheduler expired");
            owner->ProcessExitFromHostCall(code);
        });
        const std::array hosts{Cpu::SceHostModule{"libkernel.prx", {"libkernel", 1, 1, 1}, {{"libkernel", 1, 1}}}};
        graph = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{path, Bias},
            std::span<const Cpu::SceModuleFile>{}, hosts,
            [&](const Cpu::SceImport& row, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                if (const auto gate = semaphores->Resolve(row, type, 0)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = flags->Resolve(row, type, 0)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = queues->Resolve(row, type, 0)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = threadImports.Resolve(row, type)) return Cpu::SceResolvedImport{*gate, type};
                if (const auto gate = lifecycle.Resolve(row)) return Cpu::SceResolvedImport{*gate, type};
                return std::nullopt;
            });
        const auto& parsed = graph->Modules()[0].Image;
        require(parsed.Imports.size() == 34 && parsed.Tls, "Sync fixture lost its genuine import/TLS graph");
        for (const auto& item : parsed.Exports)
            if (item.Type == 1 && item.Size == sizeof(State)) address = Bias + item.Value;
        require(address && receipt() == State{}, "Loading ran sync guest code or lost receipt");
        put(address + 8, mode);
        put(address + 16, variant);
        Cpu::SetupSceEntry(machine, graph->Main(), {"public-sync-fixture"}, graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry, graph->InitialStack(), graph->Tls(), graph->ThreadTlsFactory()});
        threads->SetOwnerBoundary([](bool) {}, std::chrono::milliseconds(2000));
        graph->SetExecutor(threads->ModuleExecutor());
        graph->InitializeDependencies();
    }
    ~Session() { threads->Withdraw(); }
    State receipt() const {
        State value{};
        machine.Read(address, std::as_writable_bytes(std::span(value)));
        return value;
    }
    void put(std::uint64_t where, std::uint64_t value) { machine.Write(where, std::as_bytes(std::span(&value, 1))); }
    std::string describe() const {
        const auto r = receipt();
        std::string text = "mode=" + std::to_string(r[1]) + " magic=" + std::to_string(r[0]) +
            " failures=" + std::to_string(r[4]) + " first_failed_slot=" + std::to_string(r[3]);
        if (r[3] < r.size()) text += " value=0x" + [](std::uint64_t v) {
            char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(v));
            return std::string(buffer); }(r[r[3]]);
        return text;
    }
    State finish() {
        const auto reason = graph->RunMain(2000000000, 10000);
        if (reason != Cpu::StopReason::Exit || machine.ExitCode() != 0 || receipt()[0] != Magic)
            throw std::runtime_error("Sync guest failed: " + describe());
        return receipt();
    }
};

void semaphore(const char* path) {
    Session s(path, 1);
    const auto r = s.finish();
    require(r[47] == 2 && r[41] == Canceled && r[42] == Canceled, "Semaphore Cancel did not release both waiters");
    require(r[20] == Timedout && r[21] == 0 && r[31] == 0 && r[32] > 0, "Semaphore timed wait result/remainder differs");
    require(r[61] == Timedout && r[63] > 0, "Semaphore timed wait stalled the other guest thread");
    std::cout << "PASS semaphore FIFO/PRIO attrs, Poll, Cancel, FIFO order and timed waits\n";
}
void eventflag(const char* path) {
    Session s(path, 2);
    const auto r = s.finish();
    require(r[22] == 0x7 && r[28] == 0x1 && r[42] == 0x21, "Event flag AND/OR/clear pattern differs");
    require(r[51] == Canceled && r[52] == 0x55 && r[54] == 1, "Event flag Cancel differs");
    require(r[57] == Access && r[46] == Timedout && r[69] > 0, "Event flag delete/timeout differs");
    std::cout << "PASS event flags AND/OR, CLEAR_ALL/CLEAR_PAT, Poll, Cancel, Delete, single waiter and timed waits\n";
}
void equeue(const char* path) {
    Session s(path, 3);
    const auto r = s.finish();
    require(r[16] == 7 && r[17] == 0xfffffff5 && r[19] == 0x1234, "User event record/accessors differ");
    require(r[61] == 0 && r[62] == 1 && r[67] == 0x77, "Blocked WaitEqueue was not woken by a user trigger");
    require(r[39] == 0xfffffff9 && r[40] >= 1 && r[50] == 0xfffffff1, "Timer/HR timer events differ");
    std::cout << "PASS equeue USER (level/edge), TIMER, HRTIMER and GetEvent* accessors\n";
}
void stop(const char* path) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        Session s(path, 4, variant);
        s.threads->SetOwnerBoundary([&](bool waiting) {
            if (waiting && s.receipt()[11]) s.machine.RequestStop();
        }, std::chrono::milliseconds(0));
        const auto start = std::chrono::steady_clock::now();
        const auto reason = s.graph->RunMain(2000000000, 10000);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto r = s.receipt();
        require(reason == Cpu::StopReason::Requested && r[11] == 1 && r[12] == 0 && r[13] == 0 && r[0] == 0,
                "Parked wait ignored a stop request or fabricated a return: variant " + std::to_string(variant));
        require(elapsed < std::chrono::seconds(20), "Stop request waited for the guest timeout");
    }
    std::cout << "PASS parked event flag, equeue and timed semaphore waits honour host stop requests\n";
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: KernelSyncTest packaged.elf case");
        const std::string name = argv[2];
        if (name == "semaphore") semaphore(argv[1]);
        else if (name == "eventflag") eventflag(argv[1]);
        else if (name == "equeue") equeue(argv[1]);
        else if (name == "stop") stop(argv[1]);
        else throw std::runtime_error("Unknown sync case");
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
