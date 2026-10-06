#pragma once

#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include <functional>
#include <memory>

namespace Cpu {

using GuestThreadHandle = std::uint64_t;

struct GuestInitialThread {
    std::uint64_t Entry;
    Mapping Stack;
    std::shared_ptr<SceTls> Tls;
    SceThreadTlsFactory CreateThreadTls;
};

class GuestThreads {
public:
    explicit GuestThreads(Machine&);
    ~GuestThreads();
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;

    GuestThreadHandle AdoptInitial(GuestInitialThread);
    GuestThreadHandle ActiveThread() const;
    std::shared_ptr<SceTls> ActiveTls() const;
    std::uint64_t ActiveErrnoAddress() const;
    std::int32_t Equal(GuestThreadHandle, GuestThreadHandle) const;
    void RegisterThreadDtors(std::uint64_t guestPc);
    void RegisterThreadAtexitCount(std::uint64_t guestPc);
    void RegisterThreadAtexitReport(std::uint64_t guestPc);
    void CreateFromHostCall(std::uint64_t output8, std::uint64_t nullableAttrSlot8,
                            std::uint64_t guestEntry, std::uint64_t argument,
                            std::uint64_t nullableName);
    void YieldFromHostCall();
    void JoinFromHostCall(GuestThreadHandle, std::uint64_t nullableRetval8);
    void ExitThreadFromHostCall(std::uint64_t retval);
    void EntryTerminationFromHostCall();
    void ProcessExitFromHostCall(int exitCode);
    GuestEntryControl PendingEntryControl() const;
    void CompleteEntryControl();
    SceModuleExecutor ModuleExecutor();
    StopReason RunEntry(GuestPhaseBudget&);
    GuestCallResult InvokeModule(const GuestModuleCall&, GuestPhaseBudget&);
    void Withdraw();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
