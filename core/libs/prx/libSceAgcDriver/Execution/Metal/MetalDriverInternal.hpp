#pragma once

#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalDraw.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace AgcDriver::Metal {

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
    };
    std::mutex mutex;
    std::mutex shutdownMutex;
    std::recursive_mutex gpuMutex;
    std::condition_variable changed;
    std::map<std::uint32_t, Worker> workers;
    std::map<std::uint32_t, std::shared_ptr<IVideoOutput>> outputs;
    std::map<std::uint64_t, std::vector<std::shared_ptr<IVideoOutput>>> submissionOutputs;
    std::shared_ptr<const DriverDetail::ShaderRegistry> shaders = std::make_shared<DriverDetail::ShaderRegistry>();
    std::vector<NativeGuestMemory::BorrowedRange> ranges;
    std::uint64_t rangeGeneration = 0;
    id<MTLDevice> nativeDevice = nil;
    id<MTLLibrary> nativeLibrary = nil;
    std::unique_ptr<MetalDevice> backend;
    std::unique_ptr<MetalPresentation> presentation;
    std::unique_ptr<MetalDraw> draw;
    EopInterrupt eopInterrupt;
    std::exception_ptr failure;
    std::atomic<bool> stopping{false};
    bool configured = false;
    bool stopped = false;
    bool resetGraphics = false;
    std::uint64_t accepted = 0;
    std::uint64_t completed = 0;
    std::uint64_t eventSerial = 0;
    std::atomic<std::uint64_t> frameSerial{0};
    std::uint64_t queue0Executing = 0;
    std::uint64_t queue0Awaited = 0;
    std::set<std::uint64_t> completedOutOfOrder;
    static bool& OnWorkerThread();
    void CheckFailureAndStopping();
    void ReportFailure(std::exception_ptr error);
    void Run(std::uint32_t queue) noexcept;
    void Enqueue(Submission submission);
    bool Queue0Before(std::uint64_t received) const;
    bool OrderReleased(std::uint32_t queue, std::uint64_t received) const;
    void NoteWaitBlocked(std::uint32_t queue, std::uint64_t address, bool blocked);
    void NotifyLabelStore();
    void CompletePriorGpuWorkAndCopyBack();
    void DeliverEopInterrupt(std::uint32_t queue);
    void ExecuteSubmission(const Submission& submission, QueueState& queue);
    void ExecuteDispatchSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission);
    void ExecuteDrawSynchronously(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission);
    void ExecuteRewindTail(const Submission& submission, QueueState& queue);
    void ReserveOutputs(Submission& submission);
    void WaitForFlipRoom(const Submission& submission);
    void MarkCompleted(std::uint64_t serial);
    void Stop();
};

}
