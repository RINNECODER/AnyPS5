#include "MetalDriverInternal.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPresentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionValidation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Metal {
namespace {

class DriverStopped final : public std::runtime_error {
public:
    DriverStopped() : std::runtime_error("Metal driver has shut down") {}
};

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("Metal driver: ") + reason);
}

bool waitFree(const DriverDetail::Submission& submission) {
    if (submission.queue == 0 || submission.suspend || !submission.flips.empty() ||
        !submission.renderingWaits.empty() || submission.rewindTail != nullptr) return false;
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        const auto header = submission.commands[cursor];
        const auto opcode = (header >> 8u) & 0xffu;
        if (!Pm4::FillerPacket(header) && (opcode == 0x3c || opcode == 0x93)) return false;
        cursor += Pm4::FillerPacket(header) ? 1 : Pm4::PacketWords(header);
    }
    return true;
}

}

bool& MetalDriver::Impl::OnWorkerThread() {
    static thread_local bool worker = false;
    return worker;
}

MetalDriver::MetalDriver() : impl(std::make_unique<Impl>()) {}
MetalDriver::~MetalDriver() { impl->Stop(); }
MetalDriver& MetalDriver::Get() { static MetalDriver driver; return driver; }

void MetalDriver::Configure(void* device, void* library,
    std::span<const NativeGuestMemory::BorrowedRange> ranges, EopInterrupt interrupt) {
    require(!Impl::OnWorkerThread(), "worker cannot configure its driver");
    std::lock_guard gpuLock(impl->gpuMutex);
    std::lock_guard lock(impl->mutex);
    require(!impl->configured && impl->workers.empty() && impl->accepted == 0, "native resources may only be configured before submission");
    if (impl->failure) std::rethrow_exception(impl->failure);
    if (impl->stopping) throw DriverStopped{};
    auto nativeDevice = (__bridge id<MTLDevice>)device;
    auto nativeLibrary = (__bridge id<MTLLibrary>)library;
    require(nativeDevice != nil && nativeLibrary != nil && nativeLibrary.device == nativeDevice, "native device and library are missing or incompatible");
    NativeGuestMemory::BorrowedRangesScope scope(ranges);
    auto backend = std::make_unique<MetalDevice>(nativeDevice, nativeLibrary);
    auto presentation = std::make_unique<MetalPresentation>(*backend, nativeLibrary);
    auto draw = std::make_unique<MetalDraw>(nativeDevice, nativeLibrary);
    auto registry = std::make_shared<DriverDetail::ShaderRegistry>();
    auto nullPixel = DriverDetail::CaptureNullPixelShader();
    registry->emplace(nullPixel->codeAddress, std::move(nullPixel));
    impl->ranges.assign(ranges.begin(), ranges.end());
    impl->nativeDevice = nativeDevice;
    impl->nativeLibrary = nativeLibrary;
    impl->backend = std::move(backend);
    impl->presentation = std::move(presentation);
    impl->draw = std::move(draw);
    impl->eopInterrupt = std::move(interrupt);
    impl->shaders = std::move(registry);
    impl->configured = true;
}

void MetalDriver::Impl::CheckFailureAndStopping() {
    std::lock_guard lock(mutex);
    if (failure) std::rethrow_exception(failure);
    if (stopping) throw DriverStopped{};
    require(configured, "native resources have not been configured");
}

void MetalDriver::Impl::WaitForFlipRoom(const Submission& submission) {
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        const auto header = submission.commands[cursor];
        if (header == FlipPacketHeader) {
            std::shared_ptr<IVideoOutput> output;
            {
                std::lock_guard lock(mutex);
                const auto found = outputs.find(submission.commands[cursor + 1]);
                require(found != outputs.end(), "flip references an unregistered video output");
                output = found->second;
            }
            output->WaitForFlipRoom();
            CheckFailureAndStopping();
        }
        cursor += Pm4::FillerPacket(header) ? 1 : Pm4::PacketWords(header);
    }
}

void MetalDriver::Impl::ReserveOutputs(Submission& submission) {
    std::vector<std::shared_ptr<IVideoOutput>> retained;
    for (std::size_t cursor = 0; cursor < submission.commands.size();) {
        const auto header = submission.commands[cursor];
        const auto packet = std::span(submission.commands).subspan(cursor,
            Pm4::FillerPacket(header) ? 1 : Pm4::PacketWords(header));
        if (header == RenderingWaitPacketHeader) {
            const auto found = outputs.find(packet[1]);
            require(found != outputs.end(), "rendering wait references an unregistered video output");
            auto wait = found->second->CaptureRenderingWait(packet[2]);
            require(wait != nullptr, "video output returned a null rendering wait");
            submission.renderingWaits.emplace(cursor, std::move(wait));
            retained.push_back(found->second);
        }
        if (header == FlipPacketHeader) {
            const auto found = outputs.find(packet[1]);
            require(found != outputs.end(), "flip references an unregistered video output");
            const FlipInfo info{packet[1], std::bit_cast<std::int32_t>(packet[2]), packet[3],
                std::bit_cast<std::int64_t>(std::uint64_t{packet[4]} | (std::uint64_t{packet[5]} << 32u))};
            auto flip = found->second->Reserve(info);
            require(flip != nullptr, "video output returned a null flip reservation");
            submission.flips.emplace(cursor, std::move(flip));
            retained.push_back(found->second);
        }
        if (!Pm4::FillerPacket(header)) {
            const auto opcode = (header >> 8u) & 0xffu;
            if (opcode == 0x49 || opcode == 0x37) {
                if (const auto label = Pm4::DecodeLabelWrite(packet)) {
                    const auto bytes = label->Bytes();
                    if (label->address % 4 == 0 && bytes.size() <= 64) {
                        for (std::size_t offset = 0; offset < bytes.size(); offset += 4) submission.labelWrites.push_back(label->address + offset);
                    }
                }
            }
        }
        cursor += packet.size();
    }
    if (!retained.empty()) {
        auto& captured = submissionOutputs[submission.serial];
        captured.insert(captured.end(), retained.begin(), retained.end());
    }
}

void MetalDriver::Submit(const Packet* packet, std::uint32_t queue) {
    impl->CheckFailureAndStopping();
    require(queue == 0 || (queue >= 0x20 && queue < 0x58), "unsupported compute queue");
    Impl::Submission submission{};
    submission.queue = queue;
    Packet descriptor{};
    {
        std::lock_guard gpuLock(impl->gpuMutex);
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        GuestMemory::Read(reinterpret_cast<std::uintptr_t>(packet), std::as_writable_bytes(std::span(&descriptor, 1)), alignof(Packet));
        require(descriptor.flags == 0, "nonzero submission flags are not implemented");
        if (descriptor.dw_num != 0) CaptureSubmissionCommands(submission, reinterpret_cast<std::uintptr_t>(descriptor.addr), descriptor.dw_num);
        ValidateSubmission(submission, reinterpret_cast<std::uintptr_t>(descriptor.addr));
    }
    impl->WaitForFlipRoom(submission);
    {
        std::lock_guard lock(impl->mutex);
        if (impl->failure) std::rethrow_exception(impl->failure);
        if (impl->stopping) throw DriverStopped{};
        require(impl->accepted != std::numeric_limits<std::uint64_t>::max() && impl->eventSerial != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        submission.serial = impl->accepted + 1;
        impl->ReserveOutputs(submission);
        submission.shaders = impl->shaders;
        submission.received = ++impl->eventSerial;
        impl->Enqueue(std::move(submission));
        ++impl->accepted;
    }
    impl->changed.notify_all();
}

void MetalDriver::Impl::Enqueue(Submission submission) {
    const auto queue = submission.queue;
    submission.waitFree = waitFree(submission);
    auto& worker = workers[queue];
    for (const auto address : submission.labelWrites) ++worker.unfinishedWrites[address];
    worker.pending.push_back(std::move(submission));
    if (!worker.thread.joinable()) worker.thread = std::thread([this, queue] { Run(queue); });
}

bool MetalDriver::Impl::Queue0Before(std::uint64_t received) const {
    if (queue0Executing != 0 && queue0Executing < received) return true;
    const auto graphics = workers.find(0);
    if (graphics == workers.end()) return false;
    for (const auto& pending : graphics->second.pending) {
        if (!pending.suspend) return pending.received < received;
    }
    return false;
}

bool MetalDriver::Impl::OrderReleased(std::uint32_t queue, std::uint64_t received) const {
    if (!Queue0Before(received)) return true;
    if (queue0Awaited == 0) return false;
    if (workers.at(queue).unfinishedWrites.contains(queue0Awaited & ~std::uint64_t{3})) return true;
    for (const auto& [id, worker] : workers) {
        static_cast<void>(id);
        if (worker.running) return false;
    }
    return true;
}

void MetalDriver::Impl::NoteWaitBlocked(std::uint32_t queue, std::uint64_t address, bool blocked) {
    std::lock_guard lock(mutex);
    workers.at(queue).running = !blocked;
    if (queue == 0) queue0Awaited = blocked ? address : 0;
    changed.notify_all();
}

void MetalDriver::Impl::NotifyLabelStore() { changed.notify_all(); }
void MetalDriver::Impl::CompletePriorGpuWorkAndCopyBack() { std::lock_guard lock(gpuMutex); }

void MetalDriver::Impl::DeliverEopInterrupt(std::uint32_t queue) {
    if (!eopInterrupt) throw std::runtime_error("Metal driver EOP interrupt requires a configured native host callback");
    eopInterrupt(queue);
}

void MetalDriver::Impl::MarkCompleted(std::uint64_t serial) {
    completedOutOfOrder.insert(serial);
    while (!completedOutOfOrder.empty() && *completedOutOfOrder.begin() == completed + 1) {
        completed = *completedOutOfOrder.begin();
        completedOutOfOrder.erase(completedOutOfOrder.begin());
    }
}

void MetalDriver::Impl::Run(std::uint32_t id) noexcept {
    OnWorkerThread() = true;
    Submission submission{};
    try {
        for (;;) {
            @autoreleasepool {
                std::vector<NativeGuestMemory::BorrowedRange> capturedRanges;
                std::vector<std::shared_ptr<IVideoOutput>> releasedOutputs;
                Worker* worker = nullptr;
                {
                    std::unique_lock lock(mutex);
                    worker = &workers.at(id);
                    changed.wait(lock, [&] { return stopping || failure || !worker->pending.empty(); });
                    if (failure) std::rethrow_exception(failure);
                    if (stopping) break;
                    submission = std::move(worker->pending.front());
                    worker->pending.pop_front();
                    worker->active = true;
                    if (id == 0) queue0Executing = submission.suspend ? 0 : submission.received;
                    if (submission.waitFree) {
                        changed.wait(lock, [&] { return stopping || failure || OrderReleased(id, submission.received); });
                        if (failure) std::rethrow_exception(failure);
                        if (stopping) throw DriverStopped{};
                    }
                    worker->running = true;
                    capturedRanges = ranges;
                    if (submission.suspend) resetGraphics = true;
                    else if (id == 0 && resetGraphics) {
                        worker->state = QueueState{};
                        resetGraphics = false;
                    }
                }
                if (!submission.suspend) {
                    NativeGuestMemory::BorrowedRangesScope scope(capturedRanges);
                    ExecuteSubmission(submission, worker->state);
                }
                {
                    std::lock_guard lock(mutex);
                    if (failure) std::rethrow_exception(failure);
                    MarkCompleted(submission.serial);
                    const auto retained = submissionOutputs.find(submission.serial);
                    if (retained != submissionOutputs.end()) {
                        releasedOutputs = std::move(retained->second);
                        submissionOutputs.erase(retained);
                    }
                    for (const auto address : submission.labelWrites) {
                        const auto found = worker->unfinishedWrites.find(address);
                        if (found != worker->unfinishedWrites.end() && --found->second == 0) worker->unfinishedWrites.erase(found);
                    }
                    worker->running = false;
                    worker->active = false;
                    if (id == 0) { queue0Executing = 0; queue0Awaited = 0; }
                }
                submission = Submission{};
                changed.notify_all();
            }
        }
    } catch (const DriverStopped&) {
        const auto error = std::current_exception();
        for (const auto& [offset, flip] : submission.flips) { static_cast<void>(offset); flip->Fail(error); }
    } catch (...) {
        const auto error = std::current_exception();
        ReportFailure(error);
        for (const auto& [offset, flip] : submission.flips) { static_cast<void>(offset); flip->Fail(error); }
    }
    {
        std::lock_guard lock(mutex);
        auto& worker = workers.at(id);
        worker.running = false;
        worker.active = false;
        if (id == 0) { queue0Executing = 0; queue0Awaited = 0; }
    }
    OnWorkerThread() = false;
    changed.notify_all();
}

void MetalDriver::WaitIdle() {
    require(!Impl::OnWorkerThread(), "worker cannot wait for itself");
    std::unique_lock lock(impl->mutex);
    const auto target = impl->accepted;
    impl->changed.wait(lock, [&] { return impl->failure || impl->stopping || impl->completed >= target; });
    if (impl->failure || impl->stopping) {
        impl->changed.wait(lock, [&] {
            return std::none_of(impl->workers.begin(), impl->workers.end(), [](const auto& item) { return item.second.active; });
        });
        if (impl->failure) std::rethrow_exception(impl->failure);
        throw DriverStopped{};
    }
}

void MetalDriver::SuspendPoint() {
    require(!Impl::OnWorkerThread(), "worker cannot suspend itself");
    impl->CheckFailureAndStopping();
    {
        std::lock_guard lock(impl->mutex);
        if (impl->failure) std::rethrow_exception(impl->failure);
        if (impl->stopping) throw DriverStopped{};
        require(impl->accepted != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        Impl::Submission boundary{};
        boundary.queue = 0;
        boundary.serial = impl->accepted + 1;
        boundary.suspend = true;
        impl->Enqueue(std::move(boundary));
        ++impl->accepted;
    }
    impl->changed.notify_all();
}

void MetalDriver::RegisterShader(const Shader* shader) {
    impl->CheckFailureAndStopping();
    std::shared_ptr<const DriverDetail::ShaderSnapshot> snapshot;
    {
        std::lock_guard gpuLock(impl->gpuMutex);
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        snapshot = DriverDetail::ReadRegisteredShader(reinterpret_cast<std::uintptr_t>(shader));
    }
    std::lock_guard lock(impl->mutex);
    if (impl->failure) std::rethrow_exception(impl->failure);
    if (impl->stopping) throw DriverStopped{};
    auto registry = std::make_shared<DriverDetail::ShaderRegistry>(*impl->shaders);
    registry->insert_or_assign(snapshot->codeAddress, std::move(snapshot));
    impl->shaders = std::move(registry);
}

void MetalDriver::RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    require(output != nullptr, "null video output");
    impl->CheckFailureAndStopping();
    std::lock_guard lock(impl->mutex);
    if (impl->failure) std::rethrow_exception(impl->failure);
    if (impl->stopping) throw DriverStopped{};
    require(impl->outputs.emplace(handle, output).second, "video output already registered");
}

void MetalDriver::UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    std::lock_guard lock(impl->mutex);
    const auto found = impl->outputs.find(handle);
    require(found != impl->outputs.end() && found->second == output, "video output registration mismatch");
    impl->outputs.erase(found);
}

void MetalDriver::Impl::ReportFailure(std::exception_ptr error) {
    require(error != nullptr, "null asynchronous failure");
    std::vector<std::shared_ptr<IVideoOutput>> failedOutputs;
    std::vector<std::shared_ptr<IFlipRequest>> failedFlips;
    {
        std::lock_guard lock(mutex);
        if (!failure) failure = error;
        error = failure;
        for (const auto& [handle, output] : outputs) { static_cast<void>(handle); failedOutputs.push_back(output); }
        for (const auto& [serial, retained] : submissionOutputs) {
            static_cast<void>(serial);
            failedOutputs.insert(failedOutputs.end(), retained.begin(), retained.end());
        }
        for (auto& [queue, worker] : workers) {
            static_cast<void>(queue);
            for (const auto& pending : worker.pending) {
                for (const auto& [offset, flip] : pending.flips) { static_cast<void>(offset); failedFlips.push_back(flip); }
            }
            worker.pending.clear();
            worker.unfinishedWrites.clear();
        }
    }
    changed.notify_all();
    std::sort(failedOutputs.begin(), failedOutputs.end());
    failedOutputs.erase(std::unique(failedOutputs.begin(), failedOutputs.end()), failedOutputs.end());
    for (const auto& output : failedOutputs) output->Fail(error);
    for (const auto& flip : failedFlips) flip->Fail(error);
}

void MetalDriver::ReportFailure(std::exception_ptr error) { impl->ReportFailure(error); }

void MetalDriver::Impl::Stop() {
    require(!OnWorkerThread(), "worker cannot stop itself");
    std::lock_guard shutdownLock(shutdownMutex);
    if (stopped) return;
    const auto error = std::make_exception_ptr(DriverStopped{});
    std::vector<std::shared_ptr<IVideoOutput>> failedOutputs;
    std::vector<std::shared_ptr<IFlipRequest>> failedFlips;
    {
        std::lock_guard lock(mutex);
        stopping = true;
        for (const auto& [handle, output] : outputs) { static_cast<void>(handle); failedOutputs.push_back(output); }
        for (const auto& [serial, retained] : submissionOutputs) {
            static_cast<void>(serial);
            failedOutputs.insert(failedOutputs.end(), retained.begin(), retained.end());
        }
        for (auto& [queue, worker] : workers) {
            static_cast<void>(queue);
            for (const auto& pending : worker.pending) {
                for (const auto& [offset, flip] : pending.flips) { static_cast<void>(offset); failedFlips.push_back(flip); }
            }
            worker.pending.clear();
            worker.unfinishedWrites.clear();
        }
    }
    changed.notify_all();
    std::sort(failedOutputs.begin(), failedOutputs.end());
    failedOutputs.erase(std::unique(failedOutputs.begin(), failedOutputs.end()), failedOutputs.end());
    for (const auto& output : failedOutputs) output->Fail(error);
    for (const auto& flip : failedFlips) flip->Fail(error);
    for (auto& [queue, worker] : workers) { static_cast<void>(queue); if (worker.thread.joinable()) worker.thread.join(); }
    {
        std::lock_guard gpuLock(gpuMutex);
        draw.reset();
        presentation.reset();
        backend.reset();
        nativeLibrary = nil;
        nativeDevice = nil;
    }
    std::map<std::uint64_t, std::vector<std::shared_ptr<IVideoOutput>>> releasedOutputs;
    {
        std::lock_guard lock(mutex);
        releasedOutputs.swap(submissionOutputs);
    }
    stopped = true;
}

void MetalDriver::Shutdown() {
    impl->Stop();
    std::lock_guard lock(impl->mutex);
    if (impl->failure) std::rethrow_exception(impl->failure);
}

void MetalDriver::Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
    void (*gpuReady)(void*), void* context) {
    require(gpuReady != nullptr && context != nullptr, "presentation requires a GPU completion callback and context");
    impl->CheckFailureAndStopping();
    std::unique_lock gpuLock(impl->gpuMutex);
    impl->CheckFailureAndStopping();
    bool ready = false;
    {
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        impl->presentation->Present(window, buffer, opaque, gpuReady ? +[](void* value) { *static_cast<bool*>(value) = true; } : nullptr, &ready);
    }
    gpuLock.unlock();
    if (ready) gpuReady(context);
}

void MetalDriver::ReleaseWindow(void* window) {
    std::lock_guard gpuLock(impl->gpuMutex);
    if (impl->presentation) impl->presentation->ReleaseWindow(window);
}

void MetalDriver::Impl::ExecuteRewindTail(const Submission& stalled, QueueState& queue) {
    const auto tailAddress = reinterpret_cast<std::uintptr_t>(stalled.rewindTail);
    require(tailAddress >= sizeof(std::uint32_t), "rewind control address underflows");
    const auto controlAddress = tailAddress - sizeof(std::uint32_t);
    NoteWaitBlocked(stalled.queue, controlAddress, true);
    try {
        for (;;) {
            CheckFailureAndStopping();
            std::uint32_t control = 0;
            {
                std::lock_guard gpuLock(gpuMutex);
                GuestMemory::Read(controlAddress, std::as_writable_bytes(std::span(&control, 1)), 4);
            }
            if ((control & 0x80000000u) != 0) break;
            std::unique_lock lock(mutex);
            changed.wait_for(lock, std::chrono::milliseconds(1));
        }
    } catch (...) {
        NoteWaitBlocked(stalled.queue, 0, false);
        throw;
    }
    NoteWaitBlocked(stalled.queue, 0, false);
    Submission tail{};
    tail.queue = stalled.queue;
    {
        std::lock_guard gpuLock(gpuMutex);
        CaptureSubmissionCommands(tail, tailAddress, stalled.rewindWords);
        ValidateSubmission(tail, tailAddress);
    }
    WaitForFlipRoom(tail);
    {
        std::lock_guard lock(mutex);
        if (failure) std::rethrow_exception(failure);
        if (stopping) throw DriverStopped{};
        tail.serial = stalled.serial;
        ReserveOutputs(tail);
        tail.shaders = shaders;
        require(eventSerial != std::numeric_limits<std::uint64_t>::max(), "submission event serial overflows");
        tail.received = ++eventSerial;
    }
    ExecuteSubmission(tail, queue);
}

}
