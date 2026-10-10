#pragma once
#include <cpu/Cpu.hpp>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Cpu { struct SceImport; class GuestThreads; }
namespace Cpu::Platform {
struct KernelEventImport { std::string_view Nid, Name; };
std::span<const KernelEventImport> KernelEventInventory();
// Source ABI record; guest addresses and handles remain numeric values. The
// implementation writes this exact 32-byte layout, never a host KernelEvent.
struct KernelEventRecord {
    std::uint64_t Ident;
    std::int16_t Filter;
    std::uint16_t Flags;
    std::uint32_t Fflags;
    std::int64_t Data;
    std::uint64_t Udata;
};
static_assert(sizeof(KernelEventRecord) == 32);
static_assert(offsetof(KernelEventRecord, Ident) == 0 && offsetof(KernelEventRecord, Filter) == 8 &&
              offsetof(KernelEventRecord, Flags) == 10 && offsetof(KernelEventRecord, Fflags) == 12 &&
              offsetof(KernelEventRecord, Data) == 16 && offsetof(KernelEventRecord, Udata) == 24);

class KernelEvents {
public:
    // Machine and scheduler must outlive this provider. Construction, API
    // resolution, subscriptions, pumping and Shutdown belong to their persistent
    // owner. Only the callback returned by EopPublisher may run on producers.
    KernelEvents(Machine&, const std::shared_ptr<GuestThreads>&,
                 std::uint64_t gateBase = 0x7ffdc2000000);
    ~KernelEvents();
    KernelEvents(const KernelEvents&) = delete;
    KernelEvents& operator=(const KernelEvents&) = delete;
    // Explicit synthetic/source-contract provider; this API is not target
    // selection. Target callers must pass the separately qualified wrapper.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType,
                                         std::uint64_t symbolSize = 0);
    std::int32_t AddGraphicsEvent(std::uint64_t handle, std::int32_t id, std::uint64_t udata);
    std::int32_t DeleteGraphicsEvent(std::uint64_t handle, std::int32_t id);
    // EVFILT_VIDEO_OUT (-13) subscriptions. The ident is the VideoOut event
    // kind (0 flip, 1 vblank); output is the opaque VideoOut handle whose
    // completions feed it. A re-add replaces udata/output, as in kqueue. Both
    // return false when queue is not a live queue of this provider.
    bool AddVideoOutEvent(std::uint64_t queue, std::int32_t output, std::int32_t kind, std::uint64_t udata);
    // Removes the queue's registration of kind only when output owns it.
    bool DeleteVideoOutEvent(std::uint64_t queue, std::int32_t output, std::int32_t kind);
    // sceVideoOutWaitVblank: call only from a guest host call. Parks the
    // calling guest thread (other guest threads keep running) until the next
    // vblank that VideoOutPublisher reports for output; it then returns 0.
    void WaitVideoOutVblank(std::int32_t output);
    // Retains host mailbox/subscription receipts, never Machine access. Safe to
    // retain after shutdown. Completed EOPs coalesce per live subscription.
    std::function<void(std::uint32_t)> EopPublisher() const;
    // Same retained-mailbox contract for VideoOut flip/vblank completions;
    // payload is the flip argument or vblank count delivered in event data.
    std::function<void(std::int32_t output, std::int32_t kind, std::int64_t payload)> VideoOutPublisher() const;
    // Call after the executor returns, while Machine is idle. During a drive
    // owner hook request Machine stop instead, then withdraw at this boundary.
    void Shutdown();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

enum class KernelEventContract { CreateEqueue, DeleteEqueue, WaitEqueue };
struct KernelEventAdmission { KernelEventContract Contract; std::string_view Evidence; };
std::span<const KernelEventAdmission> TargetKernelEventAdmissions();
// Empty admission denies every contract. The native runner selects the
// title-agnostic profile; admission never depends on the importing image.
class TargetKernelEvents {
public:
    TargetKernelEvents(Machine&, const std::shared_ptr<GuestThreads>&,
                       std::span<const KernelEventAdmission> admissions = {});
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t type,
                                         std::uint64_t size);
    KernelEvents& Provider() { return provider; }
private:
    KernelEvents provider;
    std::vector<KernelEventAdmission> admissions;
};
}
