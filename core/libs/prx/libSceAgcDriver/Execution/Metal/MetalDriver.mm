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

thread_local bool driverCallback = false;
class ReadablePublicationScope {
public:
    explicit ReadablePublicationScope(const void* owner) : owner(owner), previous(active) { active = this; }
    ~ReadablePublicationScope() { active = previous; }
    ReadablePublicationScope(const ReadablePublicationScope&) = delete;
    ReadablePublicationScope& operator=(const ReadablePublicationScope&) = delete;
    static bool Contains(const void* owner) {
        for (auto scope = active; scope != nullptr; scope = scope->previous) {
            if (scope->owner == owner) return true;
        }
        return false;
    }
private:
    const void* owner;
    const ReadablePublicationScope* previous;
    static thread_local const ReadablePublicationScope* active;
};

thread_local const ReadablePublicationScope* ReadablePublicationScope::active = nullptr;

class DriverCallbackScope {
public:
    DriverCallbackScope() : previous(driverCallback) { driverCallback = true; }
    ~DriverCallbackScope() { driverCallback = previous; }
private:
    bool previous;
};

class DriverStopped final : public std::runtime_error {
public:
    DriverStopped() : std::runtime_error("Metal driver has shut down") {}
};

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("Metal driver: ") + reason);
}

void rejectReadablePublicationReentry(const void* owner) {
    require(!ReadablePublicationScope::Contains(owner), "readable range publication cannot reenter its driver");
}

std::vector<NativeGuestMemory::BorrowedRange> changedMappings(
    std::span<const NativeGuestMemory::BorrowedRange> previous,
    std::span<const NativeGuestMemory::BorrowedRange> replacement) {
    std::vector<NativeGuestMemory::BorrowedRange> changed;
    for (const auto& old : previous) {
        const auto oldEnd = old.guestAddress + old.host.size();
        auto cursor = old.guestAddress;
        while (cursor < oldEnd) {
            auto next = std::upper_bound(replacement.begin(), replacement.end(), cursor,
                [](std::uint64_t address, const auto& range) { return address < range.guestAddress; });
            const NativeGuestMemory::BorrowedRange* current = nullptr;
            if (next != replacement.begin()) {
                const auto& candidate = *std::prev(next);
                if (cursor - candidate.guestAddress < candidate.host.size()) current = &candidate;
            }
            const auto end = current ? std::min(oldEnd, current->guestAddress + current->host.size()) :
                next == replacement.end() ? oldEnd : std::min(oldEnd, next->guestAddress);
            const auto offset = static_cast<std::size_t>(cursor - old.guestAddress);
            const bool same = current && old.writable == current->writable && old.identity == current->identity &&
                old.host.data() + offset == current->host.data() + static_cast<std::size_t>(cursor - current->guestAddress);
            if (!same) changed.push_back({cursor, old.host.subspan(offset, static_cast<std::size_t>(end - cursor)), old.writable, old.identity});
            cursor = end;
        }
    }
    return changed;
}

bool overlapsMappings(std::uint64_t address, std::size_t bytes,
    std::span<const NativeGuestMemory::BorrowedRange> changed) {
    if (bytes == 0) return false;
    return std::any_of(changed.begin(), changed.end(), [&](const auto& range) {
        return address <= range.guestAddress ? range.guestAddress - address < bytes : address - range.guestAddress < range.host.size();
    });
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
    std::span<const NativeGuestMemory::BorrowedRange> ranges, EopInterrupt interrupt, std::uint64_t initialGeneration) {
    rejectReadablePublicationReentry(impl.get());
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
    std::vector<NativeGuestMemory::BorrowedRange> captured(ranges.begin(), ranges.end());
    std::sort(captured.begin(), captured.end(), [](const auto& a, const auto& b) { return a.guestAddress < b.guestAddress; });
    auto backend = std::make_unique<MetalDevice>(nativeDevice, nativeLibrary);
    auto presentation = std::make_unique<MetalPresentation>(*backend, nativeLibrary);
    auto draw = std::make_unique<MetalDraw>(nativeDevice, nativeLibrary);
    auto registry = std::make_shared<DriverDetail::ShaderRegistry>();
    auto nullPixel = DriverDetail::CaptureNullPixelShader();
    registry->emplace(nullPixel->codeAddress, std::move(nullPixel));
    impl->ranges = std::move(captured);
    impl->rangeGeneration = initialGeneration;
    impl->nativeDevice = nativeDevice;
    impl->nativeLibrary = nativeLibrary;
    impl->backend = std::move(backend);
    impl->presentation = std::move(presentation);
    impl->draw = std::move(draw);
    impl->eopInterrupt = std::move(interrupt);
    impl->shaders = std::move(registry);
    impl->configured = true;
}

void MetalDriver::ReplaceBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation) {
    replaceBorrowedRanges(ranges, generation, {}, {}, {});
}

void MetalDriver::MutateBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation,
    std::function<void()> mutateCpu, std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner) {
    require(static_cast<bool>(mutateCpu) && previousOwner && nextOwner, "mapping mutation requires a callback and retained previous and next host mappings");
    replaceBorrowedRanges(ranges, generation, mutateCpu, std::move(previousOwner), std::move(nextOwner));
}

void MetalDriver::replaceBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation,
    const std::function<void()>& mutateCpu, std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner) {
    require(!Impl::OnWorkerThread(), "worker cannot replace its borrowed ranges");
    impl->RejectMappingReentry();
    std::unique_lock mappingLock(impl->mappingMutex);
    std::vector<NativeGuestMemory::BorrowedRange> replacement(ranges.begin(), ranges.end());
    std::sort(replacement.begin(), replacement.end(), [](const auto& a, const auto& b) { return a.guestAddress < b.guestAddress; });
    {
        NativeGuestMemory::BorrowedRangesScope scope(replacement);
    }
    std::vector<std::shared_ptr<const void>> releasedOwners;
    releasedOwners.reserve(3);
    {
        std::lock_guard lock(impl->mutex);
        if (impl->failure) std::rethrow_exception(impl->failure);
        if (impl->stopping) throw DriverStopped{};
        require(impl->configured, "native resources have not been configured");
        require(!impl->mappingUpdatePending, "borrowed range transaction is already active");
        require(generation > impl->rangeGeneration, "borrowed range generation must increase");
        impl->mappingUpdatePending = true;
        impl->mappingUpdateThread = std::this_thread::get_id();
    }
    bool callbackStarted = false;
    try {
        WaitIdle();
        std::unique_lock gpuLock(impl->gpuMutex);
        std::shared_ptr<DriverDetail::ShaderRegistry> registry;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->failure) std::rethrow_exception(impl->failure);
            if (impl->stopping) throw DriverStopped{};
            require(impl->accepted == impl->completed &&
                std::none_of(impl->workers.begin(), impl->workers.end(), [](const auto& item) {
                    return item.second.active || !item.second.pending.empty();
                }), "borrowed range replacement requires drained submissions");
            auto changed = changedMappings(impl->ranges, replacement);
            auto added = changedMappings(replacement, impl->ranges);
            changed.insert(changed.end(), added.begin(), added.end());
            registry = std::make_shared<DriverDetail::ShaderRegistry>(*impl->shaders);
            std::erase_if(*registry, [&](const auto& entry) {
                const auto& snapshot = *entry.second;
                if (snapshot.codeAddress == DriverDetail::NullPixelProgramAddress()) return false;
                return overlapsMappings(snapshot.codeAddress, snapshot.code.size() * sizeof(std::uint32_t), changed) ||
                    overlapsMappings(snapshot.headerAddress, snapshot.header.size(), changed);
            });
            impl->draw->InvalidateBorrowedRanges(changed);
            if (impl->rangeOwner) releasedOwners.push_back(impl->rangeOwner);
            if (previousOwner) releasedOwners.push_back(std::move(previousOwner));
            if (nextOwner) releasedOwners.push_back(nextOwner);
            impl->retainedRangeOwners.swap(releasedOwners);
        }
        if (mutateCpu) {
            callbackStarted = true;
            mutateCpu();
        }
        {
            std::lock_guard lock(impl->mutex);
            if (impl->failure) std::rethrow_exception(impl->failure);
            if (impl->stopping) throw DriverStopped{};
            impl->shaders = std::move(registry);
            impl->ranges = std::move(replacement);
            impl->rangeGeneration = generation;
            impl->rangeOwner = std::move(nextOwner);
            releasedOwners.swap(impl->retainedRangeOwners);
            impl->mappingUpdatePending = false;
            impl->mappingUpdateThread = {};
        }
        gpuLock.unlock();
        mappingLock.unlock();
        impl->changed.notify_all();
    } catch (...) {
        const auto error = std::current_exception();
        if (callbackStarted) {
            {
                std::lock_guard lock(impl->mutex);
                if (!impl->failure) impl->failure = error;
                impl->mappingUpdateThread = {};
            }
            mappingLock.unlock();
            impl->ReportFailure(error);
        } else {
            {
                std::lock_guard lock(impl->mutex);
                releasedOwners.swap(impl->retainedRangeOwners);
                impl->mappingUpdatePending = false;
                impl->mappingUpdateThread = {};
            }
            mappingLock.unlock();
            impl->changed.notify_all();
        }
        std::rethrow_exception(error);
    }
}

void MetalDriver::Impl::CheckFailureAndStopping() {
    rejectReadablePublicationReentry(this);
    std::lock_guard lock(mutex);
    if (failure) std::rethrow_exception(failure);
    if (stopping) throw DriverStopped{};
    require(configured, "native resources have not been configured");
}

void MetalDriver::Impl::RejectMappingReentry() {
    rejectReadablePublicationReentry(this);
    std::lock_guard lock(mutex);
    require(!mappingUpdatePending || (!driverCallback && mappingUpdateThread != std::this_thread::get_id()),
        "mapping transaction callback cannot reenter its driver");
}

void MetalDriver::Impl::WaitForMappingAdmission(std::unique_lock<std::mutex>& lock) {
    if (failure) std::rethrow_exception(failure);
    if (stopping) throw DriverStopped{};
    if (mappingUpdatePending && (OnWorkerThread() || driverCallback || mappingUpdateThread == std::this_thread::get_id())) {
        throw std::runtime_error("Metal driver: mapping transaction blocks reentrant admission");
    }
    changed.wait(lock, [&] { return !mappingUpdatePending || failure || stopping; });
    if (failure) std::rethrow_exception(failure);
    if (stopping) throw DriverStopped{};
    require(configured, "native resources have not been configured");
}

std::unique_lock<std::recursive_mutex> MetalDriver::Impl::LockForGuestCapture() {
    for (;;) {
        {
            std::unique_lock lock(mutex);
            WaitForMappingAdmission(lock);
        }
        std::unique_lock gpuLock(gpuMutex);
        {
            std::lock_guard lock(mutex);
            if (failure) std::rethrow_exception(failure);
            if (stopping) throw DriverStopped{};
            if (!mappingUpdatePending) return gpuLock;
            require(!OnWorkerThread() && !driverCallback && mappingUpdateThread != std::this_thread::get_id(),
                "mapping transaction blocks reentrant capture");
        }
    }
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

void MetalDriver::WithValidatedReadableRanges(std::span<const ReadableGuestRange> ranges,
    const std::function<void()>& publish) {
    rejectReadablePublicationReentry(impl.get());
    require(!Impl::OnWorkerThread(), "worker cannot publish readable guest ranges");
    require(static_cast<bool>(publish), "readable range publication requires a callback");
    for (;;) {
        auto gpuLock = impl->LockForGuestCapture();
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        for (const auto& range : ranges) {
            require(range.address != 0 && range.bytes != 0, "readable guest range is null or empty");
            require(range.bytes <= std::numeric_limits<std::uint64_t>::max() - range.address,
                "readable guest range overflows");
            require(NativeGuestMemory::ReadableBorrowedBytes(range.address, range.bytes) == range.bytes,
                "readable guest range is not fully borrowed");
        }
        std::lock_guard lock(impl->mutex);
        if (impl->failure) std::rethrow_exception(impl->failure);
        if (impl->stopping) throw DriverStopped{};
        if (impl->mappingUpdatePending) continue;
        ReadablePublicationScope publication(impl.get());
        publish();
        return;
    }
}

void MetalDriver::Submit(const Packet* packet, std::uint32_t queue) {
    submit({}, queue, reinterpret_cast<std::uintptr_t>(packet));
}

void MetalDriver::SubmitCommandBuffer(std::uint64_t commandAddress, std::uint32_t wordCount,
    std::uint8_t flags, std::uint32_t queue) {
    submit({commandAddress, wordCount, flags}, queue, std::nullopt);
}

void MetalDriver::submit(CommandBufferSubmission descriptor, std::uint32_t queue,
    std::optional<std::uint64_t> guestPacketAddress) {
    impl->CheckFailureAndStopping();
    require(queue == 0 || (queue >= 0x20 && queue < 0x58), "unsupported compute queue");
    Impl::Submission submission{};
    submission.queue = queue;
    std::uint64_t generation = 0;
    {
        auto gpuLock = impl->LockForGuestCapture();
        generation = impl->rangeGeneration;
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        if (guestPacketAddress) {
            Packet guestDescriptor{};
            GuestMemory::Read(*guestPacketAddress, std::as_writable_bytes(std::span(&guestDescriptor, 1)), alignof(Packet));
            descriptor = {reinterpret_cast<std::uintptr_t>(guestDescriptor.addr), guestDescriptor.dw_num, guestDescriptor.flags};
        }
        require(descriptor.flags == 0, "nonzero submission flags are not implemented");
        if (descriptor.words != 0) CaptureSubmissionCommands(submission, descriptor.address, descriptor.words);
        ValidateSubmission(submission, descriptor.address);
    }
    impl->WaitForFlipRoom(submission);
    {
        std::unique_lock lock(impl->mutex);
        impl->WaitForMappingAdmission(lock);
        require(generation == impl->rangeGeneration, "borrowed ranges changed while submission was captured");
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
    rejectReadablePublicationReentry(impl.get());
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
        std::unique_lock lock(impl->mutex);
        impl->WaitForMappingAdmission(lock);
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
    std::uint64_t generation = 0;
    {
        auto gpuLock = impl->LockForGuestCapture();
        generation = impl->rangeGeneration;
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        snapshot = DriverDetail::ReadRegisteredShader(reinterpret_cast<std::uintptr_t>(shader));
    }
    std::unique_lock lock(impl->mutex);
    impl->WaitForMappingAdmission(lock);
    require(generation == impl->rangeGeneration, "borrowed ranges changed while shader was captured");
    auto registry = std::make_shared<DriverDetail::ShaderRegistry>(*impl->shaders);
    registry->insert_or_assign(snapshot->codeAddress, std::move(snapshot));
    impl->shaders = std::move(registry);
}

void MetalDriver::RegisterShaderWithPublication(std::uint64_t guestHeaderAddress,
    std::span<const ReadableGuestRange> ranges, const std::function<void()>& publish,
    const std::function<void()>& rollback) {
    impl->CheckFailureAndStopping();
    require(!Impl::OnWorkerThread(), "worker cannot publish a guest shader");
    require(!ranges.empty() && static_cast<bool>(publish) && static_cast<bool>(rollback),
        "shader publication requires readable ranges and publication/rollback callbacks");
    for (;;) {
        auto gpuLock = impl->LockForGuestCapture();
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        for (const auto& range : ranges) {
            require(range.address != 0 && range.bytes != 0, "readable guest range is null or empty");
            require(range.bytes <= std::numeric_limits<std::uint64_t>::max() - range.address,
                "readable guest range overflows");
            require(NativeGuestMemory::ReadableBorrowedBytes(range.address, range.bytes) == range.bytes,
                "readable guest range is not fully borrowed");
        }
        std::lock_guard lock(impl->mutex);
        if (impl->failure) std::rethrow_exception(impl->failure);
        if (impl->stopping) throw DriverStopped{};
        if (impl->mappingUpdatePending) continue;
        ReadablePublicationScope publication(impl.get());
        try {
            publish();
            const auto snapshot = DriverDetail::ReadRegisteredShader(guestHeaderAddress);
            auto registry = std::make_shared<DriverDetail::ShaderRegistry>(*impl->shaders);
            registry->insert_or_assign(snapshot->codeAddress, snapshot);
            impl->shaders = std::move(registry);
        } catch (...) {
            try { rollback(); }
            catch (...) {
                impl->failure = std::current_exception();
                impl->changed.notify_all();
                throw;
            }
            throw;
        }
        return;
    }
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
    rejectReadablePublicationReentry(impl.get());
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

void MetalDriver::ReportFailure(std::exception_ptr error) {
    rejectReadablePublicationReentry(impl.get());
    impl->ReportFailure(error);
}

void MetalDriver::Impl::Stop() {
    rejectReadablePublicationReentry(this);
    require(!OnWorkerThread(), "worker cannot stop itself");
    std::unique_lock shutdownLock(shutdownMutex);
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
    std::vector<std::shared_ptr<const void>> releasedOwners;
    std::shared_ptr<const void> releasedOwner;
    {
        std::lock_guard lock(mutex);
        releasedOutputs.swap(submissionOutputs);
        releasedOwners.swap(retainedRangeOwners);
        releasedOwner = std::move(rangeOwner);
        ranges.clear();
    }
    stopped = true;
    shutdownLock.unlock();
}

void MetalDriver::Shutdown() {
    impl->RejectMappingReentry();
    impl->Stop();
    std::lock_guard lock(impl->mutex);
    if (impl->failure) std::rethrow_exception(impl->failure);
}

void MetalDriver::Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
    void (*gpuReady)(void*), void* context) {
    require(gpuReady != nullptr && context != nullptr, "presentation requires a GPU completion callback and context");
    impl->RejectMappingReentry();
    impl->CheckFailureAndStopping();
    DriverCallbackScope callbackScope;
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
    rejectReadablePublicationReentry(impl.get());
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
