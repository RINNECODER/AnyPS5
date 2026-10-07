#pragma once

#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include <chrono>
#include <functional>
#include <memory>

namespace Cpu {

using GuestThreadHandle = std::uint64_t;

// Source-backed SCE FIFO priority policy: lower numbers run first. Initial threads
// use 700; null-attribute creation inheritance is handled by the scheduler.
inline constexpr std::int32_t GuestThreadPriorityMin = 256;
inline constexpr std::int32_t GuestThreadPriorityMax = 767;
inline constexpr std::int32_t GuestThreadPriorityDefault = 700;

struct GuestInitialThread {
    std::uint64_t Entry;
    Mapping Stack;
    std::shared_ptr<SceTls> Tls;
    SceThreadTlsFactory CreateThreadTls;
    std::int32_t BasePriority = GuestThreadPriorityDefault;
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
        // Only inheritance mutexes publish an owner. Zero removes the object;
        // the scheduler derives donations from live blocked calls in this
        // domain rather than accepting provider-supplied waiter priorities.
        void SetInheritanceOwner(std::uint64_t key, GuestThreadHandle owner);
        std::int32_t EffectivePriority(GuestThreadHandle) const;
        // Queues completion; the scheduler restores and validates the matching
        // suspended gate before returning guestResult in guest RAX.
        bool Wake(GuestThreadHandle, std::uint64_t key, std::uint32_t guestResult);
        // Deferred guest writes and provider consumption occur only after the
        // exact suspended gate has been validated on the idle owner Machine.
        bool Wake(GuestThreadHandle, std::uint64_t key,
                  std::function<std::uint32_t()> ownerCompletion);
        // An exact live or queued wake is abandoned with terminal Requested,
        // without manufacturing a guest provider return value.
        bool Cancel(GuestThreadHandle, std::uint64_t key);
        // Called on the idle persistent owner between slices and on blocked
        // turns. Return true while external work/deadlines can wake a waiter.
        // Producers must retain receipts elsewhere; this callback is owner-only.
        void SetOwnerPump(std::function<bool()>);
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
    // A host/UI boundary hook runs across module initialization, entry and
    // finalization through the existing executor. The positive wall idle cap
    // is a diagnostic cancellation policy, never a guest timeout result.
    // It is cumulative across a drive call, even if runnable work intervenes.
    void SetOwnerBoundary(std::function<void(bool waiting)>,
                          std::chrono::milliseconds maximumIdleWait);
    void CheckIdleOwner() const;
    GuestThreadHandle ActiveThread() const;
    std::shared_ptr<SceTls> ActiveTls() const;
    std::uint64_t ActiveErrnoAddress() const;
    std::int32_t Equal(GuestThreadHandle, GuestThreadHandle) const;
    std::int32_t BasePriority(GuestThreadHandle) const;
    std::int32_t EffectivePriority(GuestThreadHandle) const;
    // Owned opaque thread attributes are bound to their original eight-byte
    // guest slot. A sched_param contains one signed four-byte priority. Only
    // FIFO policy 1 is supported; inherit 4 uses the creating thread's base
    // priority and explicit 0 uses the attribute's stored priority.
    std::uint32_t AttributeInit(std::uint64_t slot8);
    std::uint32_t AttributeDestroy(std::uint64_t slot8);
    std::uint32_t AttributeSetPriority(std::uint64_t slot8, std::uint64_t parameter4);
    std::uint32_t AttributeGetPriority(std::uint64_t slot8, std::uint64_t output4);
    std::uint32_t AttributeSetInherit(std::uint64_t slot8, std::int32_t inherit);
    std::uint32_t AttributeSetPolicy(std::uint64_t slot8, std::int32_t policy);
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
