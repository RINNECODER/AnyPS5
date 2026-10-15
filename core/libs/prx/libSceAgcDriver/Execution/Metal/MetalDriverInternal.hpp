#pragma once

#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalDraw.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalTimeline.hpp"
#include "PacketStoreRange.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

namespace ShaderRecompiler { struct RecompileResult; }

namespace AgcDriver::Metal {

class MetalComputePipeline;

class MetalPresentation;

struct MetalDriver::Impl {
    using Submission = DriverDetail::Submission;
    struct Worker {
        std::deque<Submission> pending;
        std::map<std::uint64_t, std::uint32_t> unfinishedWrites;
        QueueState state;
        std::thread thread;
        bool running = false;
        bool active = false;
        // Submissions taken from `pending` whose GPU work has not retired yet (RetireSubmission).
        std::uint32_t unretired = 0;
    };
    std::mutex mutex;
    std::mutex shutdownMutex;
    std::mutex mappingMutex;
    std::recursive_mutex gpuMutex;
    std::condition_variable changed;
    std::map<std::uint32_t, Worker> workers;
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    std::map<std::uint64_t, std::vector<std::shared_ptr<IVideoOutput>>> submissionOutputs;
    std::shared_ptr<const DriverDetail::ShaderRegistry> shaders = std::make_shared<DriverDetail::ShaderRegistry>();
    std::vector<NativeGuestMemory::BorrowedRange> ranges;
    // Changes only when `ranges` content changes, always with gpuMutex and mutex
    // held, so a worker holding gpuMutex may read it and `ranges` (#293).
    std::uint64_t rangesRevision = 0;
    std::uint64_t rangeGeneration = 0;
    bool mappingUpdatePending = false;
    std::thread::id mappingUpdateThread;
    std::shared_ptr<const void> rangeOwner;
    std::vector<std::shared_ptr<const void>> retainedRangeOwners;
    id<MTLDevice> nativeDevice = nil;
    id<MTLLibrary> nativeLibrary = nil;
    std::unique_ptr<MetalDevice> backend;
    std::unique_ptr<MetalDevice> presentationBackend;
    std::unique_ptr<MetalPresentation> presentation;
    std::unique_ptr<MetalDraw> draw;
    EopInterrupt eopInterrupt;
    std::exception_ptr failure;
    std::atomic<bool> stopping{false};
    std::atomic<bool> skipInvalidCommandBuffers{false};
    bool configured = false;
    bool stopped = false;
    bool resetGraphics = false;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::uint64_t eventSerial = 0;
    std::atomic<std::uint64_t> frameSerial{0};
    // Moves with every packet a worker executes and every timeline completion: WaitForQueueRoom
    // keeps waiting while it moves.
    std::atomic<std::uint64_t> progress{0};
    std::uint64_t queue0Executing = 0;
    std::uint64_t queue0Awaited = 0;
    std::set<std::uint64_t> completedOutOfOrder;
    // Fail-soft bookkeeping (#289). Its own mutex: skips are recorded with gpuMutex held.
    mutable std::mutex skipMutex;
    SkippedWorkDiagnostics skipped;
    static bool& OnWorkerThread();
    void CheckFailureAndStopping();
    void RejectMappingReentry();
    void WaitForMappingAdmission(std::unique_lock<std::mutex>& lock);
    std::unique_lock<std::recursive_mutex> LockForGuestCapture();
    void ReportFailure(std::exception_ptr error);
    void Run(std::uint32_t queue) noexcept;
    void Enqueue(Submission submission);
    bool Queue0Before(std::uint64_t received) const;
    bool OrderReleased(std::uint32_t queue, std::uint64_t received) const;
    void NoteWaitBlocked(std::uint32_t queue, std::uint64_t address, bool blocked);
    bool GpuParkedOnHost() const;
    void AwaitMappingDrain();
    void NotifyLabelStore();
    // `cause` only feeds the ANYPS5_METAL_GPU_TIMING drain statistics (a packet opcode or 0x100+).
    // Materializes every lazy copy-back first (MetalLazyCopyBacks.hpp, rule 2).
    void CompletePriorGpuWorkAndCopyBack(std::uint32_t cause = 0x10f);
    // Encodes the lazy copy-backs (every one, or those over guest [address, address + bytes)) into
    // the open command buffer and commits them to the timeline, which seals them in the image cache
    // once they completed. GPU-side: nothing waits here. Called with gpuMutex held.
    void MaterializeCopyBacks(MetalLazyCopyBacks::Cause cause);
    void MaterializeCopyBacks(MetalLazyCopyBacks::Cause cause, std::uint64_t address, std::uint64_t bytes);
    // Clears the draw executor's image cache (GuestImageCache.hpp), or drops the entries a store
    // overlaps. The reason only feeds the ANYPS5_METAL_GPU_TIMING statistics (0 is a mapping change).
    enum class ForgetReason : unsigned { Other, Submission, Wait, Flip, Event, Failure, Store };
    void ForgetCachedImages(ForgetReason reason);
    void ForgetCachedImages(const GuestStoreRange& store);
    // Label bytes this driver stored itself; read and written with gpuMutex held.
    OwnLabelStores ownLabels;
    // Draws and dispatches committed without waiting (MetalTimeline.hpp); used with gpuMutex held.
    // CompletePriorGpuWorkAndCopyBack drains it.
    MetalTimeline timeline;
    // Drains the timeline before the worker reads guest [address, address + bytes) on the CPU when
    // committed work may have written it. Called with gpuMutex held.
    void BeforeCpuRead(std::uint64_t address, std::uint64_t bytes);
    // Work whose deferred completion found a working-set fault and ran again synchronously.
    std::uint64_t deferredReruns = 0;
    std::array<std::uint64_t, 0x110> drainsBy{};
    // A label store (RELEASE_MEM, WRITE_DATA) behind committed work runs at the next drain instead
    // of draining now; returns false when the packet must run now. Called with gpuMutex held.
    bool DeferLabelStore(std::span<const std::uint32_t> packet, std::uint32_t queue);
    // Whether a WAIT_REG_MEM is satisfied by the latest deferred label store to its address. Such a
    // wait is a GPU-to-GPU barrier: everything after it is committed after the work it waits for.
    bool PendingLabelSatisfies(std::span<const std::uint32_t> packet) const;
    // A deferred label store not run yet: its own completion removes it (by serial) once it stored,
    // after which the guest may change the label and only guest memory tells its value.
    struct PendingLabel {
        std::uint64_t address;
        std::array<std::byte, 64> bytes;
        std::size_t size;
        std::uint64_t serial;
    };
    std::vector<PendingLabel> pendingLabels;
    std::uint64_t labelSerial = 0;
    // ANYPS5_METAL_DEFER=0 waits for every draw and dispatch as before.
    static bool DeferWork();
    // Indirect draws and dispatches whose arguments the GPU reads (on unless ANYPS5_METAL_GPU_INDIRECT=0
    // or ANYPS5_METAL_DEFER=0).
    static bool GpuIndirect();
    // WAIT_REG_MEM on a value no pending store of ours provides waits for the value, not for the
    // GPU to idle (on unless ANYPS5_METAL_GPU_WAITS=0 or ANYPS5_METAL_DEFER=0).
    static bool GpuWaits();
    static bool GpuIndirectDispatch(std::uint32_t initiator, std::uint64_t arguments);
    static bool GpuIndirectDrawPath(const Graphics::State& state, const Pm4::DrawParameters::IndirectDraw& indirect);
    static bool GpuReadsIndirectRecords(const ShaderRecompiler::RecompileResult& result, std::uint32_t firstUserSgpr,
        std::span<const std::uint32_t> userData, Pm4::DrawParameters& parameters);
    // Indirect draws whose records the GPU read, and those that fell back to reading them on the CPU.
    std::uint64_t gpuIndirectDraws = 0;
    std::uint64_t cpuIndirectDraws = 0;
    // Command buffer batching (MetalDevice::SetBatching): on unless ANYPS5_METAL_BATCH=0.
    static bool BatchCommandBuffers();
    // Works in the open batch before an idle GPU commits it early (ANYPS5_METAL_BATCH_MIN, 8).
    static std::uint32_t BatchMinimum();
    // Compute pipelines by recompile result (ExecuteDispatchSynchronously); the weak pointer tells
    // a live result from a new one at a reused address.
    struct ComputePipelineEntry {
        std::weak_ptr<const ShaderRecompiler::RecompileResult> guest;
        std::shared_ptr<const MetalComputePipeline> pipeline;
        bool bdaStores = false;
    };
    std::mutex computePipelinesMutex;
    std::unordered_map<const ShaderRecompiler::RecompileResult*, ComputePipelineEntry> computePipelines;
    void DeliverEopInterrupt(std::uint32_t queue);
    std::uint64_t barrierWaits = 0;
    void ExecuteSubmission(const Submission& submission, QueueState& queue);
    // `indirectArguments`: DISPATCH_INDIRECT whose {x, y, z} group counts the GPU reads at that guest
    // address (the packet then carries placeholder counts); see GpuIndirectDispatch.
    void ExecuteDispatchSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission,
        std::optional<std::uint64_t> indirectArguments = std::nullopt);
    void ExecuteDrawSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission);
    void ExecuteRewindTail(const Submission& submission, QueueState& queue);
    void WaitForQueueRoom(std::uint32_t queue);
    template <typename Work>
    void Tolerate(std::string_view kind, Work&& work);
    void RecordSkip(std::string_view kind, std::string_view what);
    void ReserveOutputs(Submission& submission);
    void WaitForFlipRoom(const Submission& submission);
    void MarkCompleted(std::uint64_t serial);
    // Bookkeeping once a submission's GPU work completed: completion serial, retained outputs, label
    // writes in flight, queue room. Runs with `mutex` held.
    void RetireSubmission(Worker& worker, const Submission& submission, std::vector<std::shared_ptr<IVideoOutput>>& releasedOutputs);
    // Set (with `mutex` held) when a command buffer behind a deferred retirement completed, so an
    // idle worker reaps the timeline.
    bool reapSignal = false;
    // Runs the timeline completions whose GPU work finished (gpuMutex taken here).
    void ReapTimeline();
    void Stop();
};

}
