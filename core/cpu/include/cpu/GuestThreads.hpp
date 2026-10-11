#pragma once

#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace Cpu {

using GuestThreadHandle = std::uint64_t;

// Source-backed SCE FIFO/RR priorities: lower numbers run first. Initial threads
// use 700; null-attribute creation inheritance is handled by the scheduler.
inline constexpr std::int32_t GuestThreadPriorityMin = 256;
inline constexpr std::int32_t GuestThreadPriorityMax = 767;
inline constexpr std::int32_t GuestThreadPriorityDefault = 700;
inline constexpr std::int32_t GuestThreadPolicyFifo = 1;
inline constexpr std::int32_t GuestThreadPolicyRoundRobin = 3;
// SCHED_OTHER is accepted and scheduled as time-sharing (round robin).
inline constexpr std::int32_t GuestThreadPolicyOther = 2;

// Thread attribute fields beyond scheduling. Sizes and addresses are 8-byte
// guest values; detach state and policy/inherit are 4-byte signed values.
enum class GuestThreadAttributeField { StackSize, StackAddress, GuardSize, DetachState, Affinity, Policy, Inherit };
// Deterministic translated-instruction bound, not a vendor millisecond quantum
// or certification of native host real-time scheduling.
inline constexpr std::uint64_t GuestThreadRoundRobinQuantum = 4096;
// CPUs in the default guest thread affinity mask (0x1fff).
inline constexpr std::size_t GuestCpuCount = 13;

struct GuestInitialThread {
    std::uint64_t Entry;
    Mapping Stack;
    std::shared_ptr<SceTls> Tls;
    SceThreadTlsFactory CreateThreadTls;
    std::int32_t BasePriority = GuestThreadPriorityDefault;
};

// Stall diagnostics: one guest thread as the scheduler owner saw it between
// slices. Registers are the thread's last saved context; Frames are return
// addresses from a bounded frame-pointer walk through guest memory reads.
struct GuestThreadSnapshot {
    GuestThreadHandle Id = 0;
    std::int32_t ThreadId = 0;
    std::string Name;
    std::string State;
    bool Active = false;
    // In the run queue (a runnable thread that is not queued can never run again).
    bool Queued = false;
    std::int32_t Policy = 0, BasePriority = 0, EffectivePriority = 0;
    std::uint64_t Rip = 0, Rsp = 0, Rbp = 0;
    // Slices run and instructions retired since the previous snapshot.
    std::uint64_t Slices = 0, Instructions = 0;
    // Guest address of the last host import trampoline the thread entered (0: none yet).
    std::uint64_t LastHostEntry = 0;
    std::uint64_t HostEntries = 0;
    // Provider wait domain and key while parked in a host wait, else 0.
    std::uint64_t WaitDomain = 0, WaitKey = 0;
    std::vector<std::uint64_t> Frames;
};
using GuestThreadDumpSink = std::function<void(const std::vector<GuestThreadSnapshot>&)>;

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
        // Move an exact parked call to another object in the same provider
        // domain without making it runnable or refreshing its RR quantum.
        // Condition signaling uses this to await the original mutex owner.
        bool TransferWait(GuestThreadHandle, std::uint64_t key, std::uint64_t destinationKey);
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
        // Owner-idle only: withdraw an exact queued wake before it resumes.
        // The original suspended call remains blocked in this live domain,
        // with no guest writes, return value, or lifecycle cancellation.
        bool RetractWake(GuestThreadHandle, std::uint64_t key);
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
    // finalization through the existing executor. A positive idle cap is a
    // diagnostic cancellation policy, never a guest timeout result. It bounds
    // one continuous idle stretch and resets whenever guest work runs. Zero
    // disables idle cancellation; the owner still pumps every 1 ms turn.
    void SetOwnerBoundary(std::function<void(bool waiting)>,
                          std::chrono::milliseconds maximumIdleWait);
    void CheckIdleOwner() const;
    // Guest CPUs the single host owner emulates. With one CPU (the default) the best
    // effective priority always runs next. With n CPUs the n best-priority runnable
    // threads are all on a CPU at once, so the owner time-shares them least recently
    // run first: a thread busy-waiting for a sibling no longer starves it.
    void SetGuestCores(std::size_t cores);
    // Async-signal-safe: the scheduler owner emits one snapshot at its next turn.
    static void RequestThreadDump() noexcept;
    // Owner only. The sink runs on the scheduler owner between slices; a positive
    // period also emits a snapshot that often. An empty sink disables tracking.
    void SetThreadDump(GuestThreadDumpSink sink, std::chrono::milliseconds period);
    std::vector<GuestThreadSnapshot> SnapshotThreads() const;
    GuestThreadHandle ActiveThread() const;
    std::shared_ptr<SceTls> ActiveTls() const;
    std::uint64_t ActiveErrnoAddress() const;
    std::int32_t Equal(GuestThreadHandle, GuestThreadHandle) const;
    std::int32_t BasePriority(GuestThreadHandle) const;
    std::int32_t EffectivePriority(GuestThreadHandle) const;
    // Owned opaque thread attributes are bound to their original eight-byte
    // guest slot. A sched_param contains one signed four-byte priority. Only
    // FIFO policy 1 and logical RR policy 3 are supported; inherit 4 uses the
    // creating thread's base policy/priority and explicit 0 copies both stored
    // attribute values. Donation never changes inherited policy/priority.
    std::uint32_t AttributeInit(std::uint64_t slot8);
    std::uint32_t AttributeDestroy(std::uint64_t slot8);
    std::uint32_t AttributeSetPriority(std::uint64_t slot8, std::uint64_t parameter4);
    std::uint32_t AttributeGetPriority(std::uint64_t slot8, std::uint64_t output4);
    std::uint32_t AttributeSetInherit(std::uint64_t slot8, std::int32_t inherit);
    std::uint32_t AttributeSetPolicy(std::uint64_t slot8, std::int32_t policy);
    // Return SCE error codes (EINVAL/ESRCH/EDEADLK), never host aborts.
    std::uint32_t AttributeSet(std::uint64_t slot8, GuestThreadAttributeField, std::uint64_t value);
    std::uint32_t AttributeGet(std::uint64_t slot8, GuestThreadAttributeField, std::uint64_t output);
    // pthread_attr_get_np: copies a live thread's attributes into an owned attribute.
    std::uint32_t AttributeFromThread(GuestThreadHandle, std::uint64_t slot8);
    std::uint32_t Detach(GuestThreadHandle);
    std::uint32_t SetPriority(GuestThreadHandle, std::int32_t priority);
    std::uint32_t GetPriority(GuestThreadHandle, std::uint64_t output4);
    std::uint32_t SetAffinity(GuestThreadHandle, std::uint64_t mask);
    std::uint32_t GetAffinity(GuestThreadHandle, std::uint64_t output8);
    std::uint32_t Rename(GuestThreadHandle, std::uint64_t name);
    // Writes the NUL-terminated name into a 32-byte guest buffer.
    std::uint32_t GetName(GuestThreadHandle, std::uint64_t output32);
    std::int32_t ThreadId(GuestThreadHandle) const;
    // Deferred cancellation: the request is observed by the target at Testcancel.
    std::uint32_t Cancel(GuestThreadHandle);
    bool CancelRequested() const;
    // Validation that runs before a join is queued; nonzero is the guest result.
    std::uint32_t JoinCheck(GuestThreadHandle) const;
    // Validation that runs before a creation is queued; nonzero is the guest result.
    std::uint32_t CreateCheck(std::uint64_t nullableAttrSlot8) const;
    // A provider-owned guest routine that every finishing created thread runs
    // first (e.g. TSD destructors). It must chain to ThreadDtors() itself.
    void SetThreadExitHook(std::uint64_t guestPc);
    std::uint64_t ThreadDtors() const;
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
