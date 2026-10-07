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
    // A provider owns one domain. Its keys must identify provider-local wait
    // objects and must not be reused while a suspended call still exists.
    class WaitDomain {
    public:
        WaitDomain();
        ~WaitDomain();
        WaitDomain(WaitDomain&&) noexcept;
        WaitDomain& operator=(WaitDomain&&) noexcept;
        WaitDomain(const WaitDomain&) = delete;
        WaitDomain& operator=(const WaitDomain&) = delete;

        GuestThreadHandle ActiveThread() const;
        void BlockFromHostCall(std::uint64_t key);
        bool IsWaiting(GuestThreadHandle, std::uint64_t key) const;
        // Queues completion; the scheduler restores and validates the matching
        // suspended gate before returning guestResult in guest RAX.
        bool Wake(GuestThreadHandle, std::uint64_t key, std::uint32_t guestResult);
        // Call only while guest execution is idle. Pending calls, including
        // already queued wakes, cause a terminal Requested cancellation rather
        // than return through a provider gate that may be about to disappear.
        // Safe after scheduler withdrawal/destruction; expired domains cannot
        // access a replacement scheduler or provider.
        void Withdraw();
    private:
        struct State;
        std::unique_ptr<State> state;
        explicit WaitDomain(std::unique_ptr<State>);
        friend class GuestThreads;
    };

    explicit GuestThreads(Machine&);
    ~GuestThreads();
    GuestThreads(const GuestThreads&) = delete;
    GuestThreads& operator=(const GuestThreads&) = delete;

    GuestThreadHandle AdoptInitial(GuestInitialThread);
    GuestThreadHandle ActiveThread() const;
    std::shared_ptr<SceTls> ActiveTls() const;
    std::uint64_t ActiveErrnoAddress() const;
    std::int32_t Equal(GuestThreadHandle, GuestThreadHandle) const;
    // Called once per thread after normal finalization, or when continuation is
    // cancelled. Providers can remove waits and poison nonrobust owned objects;
    // this notification does not imply successful mutex release or acquisition.
    WaitDomain CreateWaitDomain(Machine& associatedMachine,
                                std::function<void(GuestThreadHandle)> onThreadStopped);
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
