#include "MetalDriverInternal.hpp"
#include "AgcFrameCapture.hpp"
#include <pthread.h>
#include "prx/libSceAgcDriver/Graphics/Metal/BindTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPipelineManifest.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPresentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/SubmissionValidation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>

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
bool MetalDriver::OnDriverWorkerThread() { return Impl::OnWorkerThread(); }

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
    // Presentation uploads pixels the CPU already read: its own queue keeps a present from waiting
    // behind draws and dispatches the workers committed meanwhile.
    auto presentationBackend = std::make_unique<MetalDevice>(nativeDevice, nativeLibrary);
    auto presentation = std::make_unique<MetalPresentation>(*presentationBackend, nativeLibrary);
    auto draw = std::make_unique<MetalDraw>(*backend);
    draw->SetCpuReadHook([impl = impl.get()](std::uint64_t address, std::uint64_t bytes) { impl->BeforeCpuRead(address, bytes); });
    draw->SetMaterializeHook([impl = impl.get()](std::uint64_t address, std::uint64_t bytes) {
        impl->MaterializeCopyBacks(MetalLazyCopyBacks::Cause::GpuRead, address, bytes);
    });
    auto registry = std::make_shared<DriverDetail::ShaderRegistry>();
    auto nullPixel = DriverDetail::CaptureNullPixelShader();
    const auto nullPixelAddress = nullPixel->codeAddress;
    (*registry)[nullPixelAddress] = {std::move(nullPixel)};
    impl->ranges = std::move(captured);
    impl->rangeGeneration = initialGeneration;
    impl->nativeDevice = nativeDevice;
    impl->nativeLibrary = nativeLibrary;
    impl->backend = std::move(backend);
    impl->presentationBackend = std::move(presentationBackend);
    impl->presentation = std::move(presentation);
    impl->draw = std::move(draw);
    if (Impl::BatchCommandBuffers()) {
        impl->backend->SetBatching(true);
        impl->timeline.SetBatchDevice(impl->backend.get(), Impl::BatchMinimum());
    }
    // A worker with nothing to execute still retires submissions as their GPU work completes.
    impl->timeline.SetCompletionSignal([impl = impl.get()] {
        impl->progress.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(impl->mutex);
            impl->reapSignal = true;
        }
        impl->changed.notify_all();
    });
    impl->eopInterrupt = std::move(interrupt);
    impl->shaders = std::move(registry);
    impl->configured = true;
    StartMetalPipelineManifestReplay(nativeDevice);
}

bool MetalDriver::WaitBeforeHostWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    return impl->WaitBeforeHostWrites(ranges);
}

MetalDriver::PendingWrites MetalDriver::PendingGuestWrites() {
    return impl->PendingGuestWrites();
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
        // #293: diff first. Only a change to the GPU-visible set needs the GPU at
        // all; only a removed or altered range lets accepted work drain first.
        std::vector<NativeGuestMemory::BorrowedRange> removed, changed;
        {
            std::lock_guard lock(impl->mutex);
            removed = changedMappings(impl->ranges, replacement);
            changed = changedMappings(replacement, impl->ranges);
        }
        const bool visible = !removed.empty() || !changed.empty();
        if (!removed.empty()) impl->AwaitMappingDrain();
        // Workers touch guest memory only with gpuMutex held and re-borrow the
        // ranges when rangesRevision moves, so holding it is enough to swap them.
        std::unique_lock<std::recursive_mutex> gpuLock;
        if (visible) {
            gpuLock = std::unique_lock(impl->gpuMutex);
            // Work committed without waiting still reads the old mapping: let it finish first.
            try {
                impl->MaterializeCopyBacks(MetalLazyCopyBacks::Cause::Mapping);
                impl->timeline.Drain(0x10d);
            } catch (...) {
                impl->ReportFailure(std::current_exception());
            }
        }
        std::shared_ptr<DriverDetail::ShaderRegistry> registry;
        {
            std::lock_guard lock(impl->mutex);
            if (impl->failure) std::rethrow_exception(impl->failure);
            if (impl->stopping) throw DriverStopped{};
            if (visible) {
                changed.insert(changed.end(), removed.begin(), removed.end());
                registry = std::make_shared<DriverDetail::ShaderRegistry>(*impl->shaders);
                std::vector<std::uint64_t> emptyAddresses;
                std::vector<std::pair<std::uint64_t, DriverDetail::ShaderRegistry::mapped_type>> replaced;
                for (const auto& [address, headers] : *registry) {
                    auto remaining = headers;
                    std::erase_if(remaining, [&](const auto& snapshotPointer) {
                        const auto& snapshot = *snapshotPointer;
                        if (snapshot.codeAddress == DriverDetail::NullPixelProgramAddress()) return false;
                        return overlapsMappings(snapshot.codeAddress, snapshot.code.size() * sizeof(std::uint32_t), changed) ||
                            overlapsMappings(snapshot.headerAddress, snapshot.header.size(), changed);
                    });
                    if (remaining.empty()) {
                        emptyAddresses.push_back(address);
                    } else if (remaining.size() != headers.size()) {
                        replaced.emplace_back(address, std::move(remaining));
                    }
                }
                for (auto& [address, headers] : replaced) {
                    (*registry)[address] = std::move(headers);
                }
                for (const auto address : emptyAddresses) {
                    registry->erase(address);
                }
                impl->draw->InvalidateBorrowedRanges(changed);
            }
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
            if (visible) {
                impl->shaders = std::move(registry);
                impl->ranges = std::move(replacement);
                ++impl->rangesRevision;
            }
            impl->rangeGeneration = generation;
            impl->rangeOwner = std::move(nextOwner);
            releasedOwners.swap(impl->retainedRangeOwners);
            impl->mappingUpdatePending = false;
            impl->mappingUpdateThread = {};
        }
        if (gpuLock.owns_lock()) gpuLock.unlock();
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

void MetalDriver::SetSkipInvalidCommandBuffers(bool skip) { impl->skipInvalidCommandBuffers.store(skip, std::memory_order_relaxed); }

void MetalDriver::SubmitCommandBuffer(std::uint64_t commandAddress, std::uint32_t wordCount,
    std::uint8_t flags, std::uint32_t queue) {
    submit({commandAddress, wordCount, flags}, queue, std::nullopt);
}

namespace {

// The GPU fetches a submitted command buffer some time after the submit call, so a title may still
// be writing the last words of one when it submits it (a job thread finishing a buffer the
// submitting thread hands over): the console reads them in time. This capture runs inside the
// call. One that fails validation is read again for up to RecaptureMs before the error stands;
// the log says whether the words changed, which tells a late writer from a malformed buffer.
constexpr int RecaptureMs = 20;

void CaptureAndValidate(DriverDetail::Submission& submission, std::uint64_t address, std::size_t words) {
    const auto capture = [&] {
        submission.rewindTail = nullptr;
        submission.rewindWords = 0;
        if (words != 0) CaptureSubmissionCommands(submission, address, words);
    };
    capture();
    std::exception_ptr rejected;
    try {
        ValidateSubmission(submission, address);
        return;
    } catch (const std::exception&) {
        rejected = std::current_exception();
    }
    if (words == 0) std::rethrow_exception(rejected);
    const auto first = submission.commands;
    bool changed = false;
    for (int waited = 1; waited <= RecaptureMs; ++waited) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        capture();
        if (submission.commands == first) continue;
        changed = true;
        try {
            ValidateSubmission(submission, address);
        } catch (const std::exception&) {
            continue;
        }
        std::fprintf(stderr, "[metal] submission at 0x%llx (%zu words) was still being written at submit: valid %d ms later\n",
            static_cast<unsigned long long>(address), words, waited);
        return;
    }
    std::fprintf(stderr, "[metal] submission at 0x%llx (%zu words) rejected; its words %s over %d ms\n", static_cast<unsigned long long>(address),
        words, changed ? "changed but stayed invalid" : "did not change", RecaptureMs);
    std::rethrow_exception(rejected);
}

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
        generation = impl->rangesRevision;
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        if (guestPacketAddress) {
            Packet guestDescriptor{};
            GuestMemory::Read(*guestPacketAddress, std::as_writable_bytes(std::span(&guestDescriptor, 1)), alignof(Packet));
            descriptor = {reinterpret_cast<std::uintptr_t>(guestDescriptor.addr), guestDescriptor.dw_num, guestDescriptor.flags};
        }
        require(descriptor.flags == 0, "nonzero submission flags are not implemented");
        try {
            CaptureAndValidate(submission, descriptor.address, descriptor.words);
            submission.commandAddress = descriptor.address;
            CaptureIndirectRegisterLists(submission);
        } catch (const DriverStopped&) {
            throw;
        } catch (const std::exception& error) {
            throw InvalidCommandBuffer(error.what());
        }
    }
    impl->WaitForFlipRoom(submission);
    {
        std::unique_lock lock(impl->mutex);
        impl->WaitForMappingAdmission(lock);
        require(generation == impl->rangesRevision, "borrowed ranges changed while submission was captured");
        require(impl->accepted != std::numeric_limits<std::uint64_t>::max() && impl->eventSerial != std::numeric_limits<std::uint64_t>::max(), "submission serial overflow");
        submission.serial = impl->accepted + 1;
        impl->ReserveOutputs(submission);
        submission.shaders = impl->shaders;
        submission.received = ++impl->eventSerial;
        impl->Enqueue(std::move(submission));
        ++impl->accepted;
    }
    impl->changed.notify_all();
    impl->WaitForQueueRoom(queue);
}

// The guest builds frames against a GPU that keeps up: it reuses per-frame vertex and constant
// rings (the UI renderer's, for one) as soon as its own pacing allows, which on hardware is long
// after the GPU consumed them. This worker executes far slower, so without a bound the guest ran
// several submissions ahead and rewrote ring entries before the draws that read them ran: UI
// draws then rendered with later vertex or constant data, clearing or filling the wrong regions
// of a layer the guest only repaints where it changed. Submitting therefore waits until at most
// `ANYPS5_METAL_MAX_QUEUED` (default 1) submissions of the queue are waiting or running. A guest
// that submits work gated on a label it writes only after the submit returns still makes progress:
// the wait also ends once the queue's worker is parked on such a host-satisfied wait (WAIT_REG_MEM,
// RenderingWait). It used to end after 250 ms instead, which a title loading its map outran: a
// dispatch that takes longer let the guest rewrite the constants of work still queued, and a
// dispatch that then read a tile table as its loop bounds never finished. Giving up after 10 s
// without worker progress (60 s at most) only guards against waits this does not model. A negative value turns the bound off.
void MetalDriver::Impl::WaitForQueueRoom(std::uint32_t queue) {
    static const long maxQueued = [] {
        const char* value = std::getenv("ANYPS5_METAL_MAX_QUEUED");
        return value != nullptr && *value != '\0' ? std::strtol(value, nullptr, 10) : 1L;
    }();
    if (maxQueued < 0 || OnWorkerThread()) return;
    std::unique_lock lock(mutex);
    const auto found = workers.find(queue);
    if (found == workers.end()) return;
    const auto& worker = found->second;
    const auto hasRoom = [&] {
        return stopping || failure || static_cast<long>(worker.pending.size() + worker.unretired) <= maxQueued ||
            (worker.active && !worker.running);
    };
    // Gives up when the workers made no progress for 10 s (a wait this does not model): a worker
    // busy converting a burst of new shaders can take longer than that to retire one submission,
    // and submitting past it then let the guest rewrite data queued work reads. `progress` is
    // shared by every queue, so another queue's work could keep it moving while this one is stuck
    // on such a wait: 60 s in all ends the wait regardless.
    bool room = false;
    bool stalled = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (auto seen = progress.load(std::memory_order_relaxed);;) {
        room = changed.wait_for(lock, std::chrono::seconds(10), hasRoom);
        const auto now = progress.load(std::memory_order_relaxed);
        stalled = now == seen;
        if (room || stalled || std::chrono::steady_clock::now() >= deadline) break;
        seen = now;
    }
    static std::atomic<unsigned> reports{0};
    if (!room && reports.fetch_add(1) < 4) {
        std::fprintf(stderr, "[metal] queue %u %s with %zu submissions waiting; submitting anyway\n", queue,
            stalled ? "made no progress for 10 s" : "had no room for 60 s", worker.pending.size());
    }
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

bool MetalDriver::Impl::GpuParkedOnHost() const {
    bool parked = false;
    for (const auto& [id, worker] : workers) {
        static_cast<void>(id);
        if (worker.active && !worker.running) { parked = true; continue; }
        if (worker.active || !worker.pending.empty()) return false;
    }
    return parked;
}

// #293: a change that removes or alters a GPU-visible range first lets accepted
// work drain, so work submitted before an unmap still sees the old mapping. It
// never waits on a GPU that waits on the CPU: once every unfinished queue is
// parked on a host-satisfied wait (WAIT_REG_MEM, rewind, RenderingWait), more
// waiting would deadlock this guest thread, so the change proceeds and the parked
// work resumes on the new mapping. The wait is bounded for the same reason; going
// on early is safe because workers re-borrow ranges under gpuMutex.
void MetalDriver::Impl::AwaitMappingDrain() {
    constexpr auto limit = std::chrono::seconds(2);
    std::unique_lock lock(mutex);
    const auto target = accepted;
    const bool settled = changed.wait_for(lock, limit, [&] {
        return failure || stopping || completed >= target || GpuParkedOnHost();
    });
    if (failure || stopping || completed >= target) return;
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) >= 8) return;
    if (settled) {
        std::fprintf(stderr, "[agc] mapping change did not wait for GPU work parked on a CPU-written wait "
            "(queue0 label 0x%llx); the work resumes on the new mapping\n", static_cast<unsigned long long>(queue0Awaited));
    } else {
        std::fprintf(stderr, "[agc] mapping change waited %lld ms for %llu GPU submissions to drain; proceeding, "
            "the work resumes on the new mapping\n", static_cast<long long>(std::chrono::milliseconds(limit).count()),
            static_cast<unsigned long long>(target - completed));
    }
}
void MetalDriver::Impl::CompletePriorGpuWorkAndCopyBack(std::uint32_t cause) {
    std::lock_guard lock(gpuMutex);
    MaterializeCopyBacks(MetalLazyCopyBacks::Cause::Drain);
    if (!timeline.Empty()) ++drainsBy[std::min<std::uint32_t>(cause, drainsBy.size() - 1)];
    timeline.Drain(cause);
}


namespace {

void commitMaterialized(MetalTimeline& timeline, id<MTLCommandBuffer> commands,
    std::vector<MetalLazyCopyBacks::Materialized> done, GuestImageCache<MetalTexture>* cache) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    written.reserve(done.size());
    for (const auto& entry : done) written.emplace_back(entry.guestAddress, entry.guestAddress + entry.bytes);
    // The completion runs in timeline order: before any label, interrupt or retirement deferred
    // after this point, and it keeps the textures alive until the GPU ran the copy-backs.
    timeline.Commit(commands, [done = std::move(done), cache] {
        if (cache == nullptr) return;
        for (const auto& entry : done) cache->Seal(entry.host, entry.texture.get());
    }, false, std::move(written));
}

}

void MetalDriver::Impl::MaterializeCopyBacks(MetalLazyCopyBacks::Cause cause) {
    std::lock_guard lock(gpuMutex);
    if (draw == nullptr || backend == nullptr || draw->LazyCopyBacks().Empty()) return;
    id<MTLCommandBuffer> commands = backend->CommandBuffer();
    MetalBatchedWork batched(*backend, commands);
    auto done = draw->LazyCopyBacks().EncodeAll(commands, cause);
    batched.Done();
    commitMaterialized(timeline, commands, std::move(done), draw->ImageCache());
}

bool MetalDriver::Impl::MaterializeCopyBacks(MetalLazyCopyBacks::Cause cause, std::uint64_t address, std::uint64_t bytes) {
    std::lock_guard lock(gpuMutex);
    if (draw == nullptr || backend == nullptr || bytes == 0 || draw->LazyCopyBacks().Empty()) return false;
    bool encoded = false;
    const auto end = bytes > ~std::uint64_t{0} - address ? ~std::uint64_t{0} : address + bytes;
    for (const auto& range : ranges) {
        const auto begin = std::max(address, range.guestAddress);
        const auto limit = std::min(end, range.guestAddress + range.host.size());
        if (begin >= limit) continue;
        const auto* host = range.host.data() + (begin - range.guestAddress);
        const auto size = static_cast<std::size_t>(limit - begin);
        if (!draw->LazyCopyBacks().Overlaps(host, size)) continue;
        id<MTLCommandBuffer> commands = backend->CommandBuffer();
        MetalBatchedWork batched(*backend, commands);
        auto done = draw->LazyCopyBacks().EncodeOverlapping(commands, host, size, cause);
        batched.Done();
        encoded = encoded || !done.empty();
        commitMaterialized(timeline, commands, std::move(done), draw->ImageCache());
    }
    return encoded;
}

void MetalDriver::Impl::BeforeCpuRead(std::uint64_t address, std::uint64_t bytes) {
    const auto end = bytes > ~std::uint64_t{0} - address ? ~std::uint64_t{0} : address + bytes;
    // A surface whose texture is newer than these bytes is copied back first (and then drained).
    // The copy-back is recorded under the surface's own guest address: a read through another guest
    // alias of the same host bytes would not match it in MayHaveWritten, so it drains anyway.
    const bool materialized = MaterializeCopyBacks(MetalLazyCopyBacks::Cause::CpuRead, address, bytes);
    if (materialized || timeline.MayHaveWritten(address, end)) {
        if (BindTrace::Active()) {
            BindTrace::Log("  drain before CPU read 0x%llx +0x%llx%s", static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(bytes), timeline.UnknownWritesPending() ? " (unknown writes pending)" : "");
        }
        ++drainsBy[0x102];
        timeline.Drain(0x102);
    }
}

bool MetalDriver::Impl::WaitBeforeHostWrites(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    std::lock_guard lock(gpuMutex);
    if (ranges.empty()) return false;
    // Bytes a recorded copy-back would write are materialized first: the host's write has to be the
    // one that stays, and the copy-back is committed ahead of it on the timeline like any other.
    bool materialized = false;
    if (draw != nullptr && backend != nullptr && !draw->LazyCopyBacks().Empty() && CopyBacksOverlap(ranges)) {
        MaterializeCopyBacks(MetalLazyCopyBacks::Cause::HostWrite);
        materialized = true;
    }
    if (!materialized && !timeline.MayHaveWrittenAny(ranges)) return false;
    ++drainsBy[0x10e];
    timeline.Drain(0x10e);
    return true;
}

bool MetalDriver::Impl::CopyBacksOverlap(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    // The copy-backs match at the host addresses of the borrowed ranges, so translate the guest
    // ranges the caller passes through them, as MaterializeCopyBacks does.
    if (draw == nullptr) return false;
    std::vector<std::pair<std::uintptr_t, std::size_t>> hosts;
    for (const auto& borrowed : this->ranges) {
        const auto rangeBegin = borrowed.guestAddress;
        const auto rangeEnd = rangeBegin + borrowed.host.size();
        for (const auto& [address, bytes] : ranges) {
            if (bytes > ~std::uint64_t{0} - address) continue;
            const auto begin = std::max(address, rangeBegin);
            const auto limit = std::min(address + bytes, rangeEnd);
            if (begin >= limit) continue;
            const auto host = reinterpret_cast<std::uintptr_t>(borrowed.host.data() + (begin - rangeBegin));
            hosts.emplace_back(host, static_cast<std::size_t>(limit - begin));
        }
    }
    std::sort(hosts.begin(), hosts.end());
    return draw->LazyCopyBacks().OverlapsAny(hosts);
}

MetalDriver::PendingWrites MetalDriver::Impl::PendingGuestWrites() {
    std::lock_guard lock(gpuMutex);
    PendingWrites pending;
    // Work whose stores cannot be named (BDA pointers) covers everything: the caller cannot tell
    // which of its ranges the GPU may write and waits as before.
    if (timeline.UnknownWritesPending()) {
        pending.unknown = true;
        return pending;
    }
    pending.ranges = timeline.WriteRanges();
    return pending;
}

bool MetalDriver::Impl::BatchCommandBuffers() {
    static const bool batch = [] {
        const char* value = std::getenv("ANYPS5_METAL_BATCH");
        return value == nullptr || *value != '0';
    }();
    return batch;
}

std::uint32_t MetalDriver::Impl::BatchMinimum() {
    static const std::uint32_t minimum = [] {
        const char* value = std::getenv("ANYPS5_METAL_BATCH_MIN");
        if (value == nullptr || *value == '\0') return std::uint32_t{8};
        const auto parsed = std::strtoul(value, nullptr, 10);
        return parsed == 0 ? std::numeric_limits<std::uint32_t>::max() : static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 1u << 20));
    }();
    return minimum;
}

bool MetalDriver::Impl::DeferWork() {
    static const bool defer = [] {
        const char* value = std::getenv("ANYPS5_METAL_DEFER");
        return value == nullptr || *value == '\0' || *value != '0';
    }();
    return defer;
}

void MetalDriver::Impl::ForgetCachedImages(ForgetReason reason) {
    if (draw == nullptr || draw->ImageCache() == nullptr) return;
    // The guest may have written memory at these points: cached surfaces prove their bytes before
    // their next use (GuestImageCache::Expire). ANYPS5_METAL_IMAGE_CACHE_EXPIRE=0 drops them instead.
    static const bool expire = [] {
        const char* value = std::getenv("ANYPS5_METAL_IMAGE_CACHE_EXPIRE");
        return value == nullptr || *value != '0';
    }();
    const bool guestPoint = reason == ForgetReason::Submission || reason == ForgetReason::Wait || reason == ForgetReason::Flip ||
        reason == ForgetReason::Event;
    if (expire && guestPoint) draw->ImageCache()->Expire(static_cast<unsigned>(reason));
    else draw->ImageCache()->Clear(static_cast<unsigned>(reason));
}

void MetalDriver::Impl::ForgetCachedImages(const GuestStoreRange& store) {
    if (draw == nullptr || draw->ImageCache() == nullptr || store.bytes == 0) return;
    if (store.bytes == GuestStoreRange::Everything().bytes) return ForgetCachedImages(ForgetReason::Store);
    for (const auto& range : ranges) {
        if (!store.Overlaps(range.guestAddress, range.host.size())) continue;
        const auto begin = std::max(store.begin, range.guestAddress);
        const auto end = std::min(store.begin + std::min(store.bytes, ~std::uint64_t{0} - store.begin), range.guestAddress + range.host.size());
        draw->ImageCache()->Invalidate(range.host.data() + (begin - range.guestAddress), static_cast<std::size_t>(end - begin),
            Metal::GuestImageCache<Metal::MetalTexture>::Cause::DriverStore);
    }
}

bool MetalDriver::Impl::DeliverGuestEopInterrupt(std::uint32_t queue) {
    if (!eopInterrupt) throw std::runtime_error("Metal driver EOP interrupt requires a configured native host callback");
    if (!eopInterrupt(queue)) return false;
    std::lock_guard lock(eopSelfSignalMutex);
    eopSelfSignalFlips[queue] = 0;
    return true;
}

void MetalDriver::Impl::DeliverFlipEopInterrupt(std::uint32_t queue) {
    {
        std::lock_guard lock(eopSelfSignalMutex);
        const auto latched = eopSelfSignalFlips.find(queue);
        if (latched != eopSelfSignalFlips.end()) {
            if (++latched->second <= eopSelfSignalFlipLimit) return;
            eopSelfSignalFlips.erase(latched);
        }
    }
    DeliverGuestEopInterrupt(queue);
}

void MetalDriver::Impl::RetireSubmission(Worker& worker, const Submission& submission,
    std::vector<std::shared_ptr<IVideoOutput>>& releasedOutputs) {
    MarkCompleted(submission.serial);
    const auto retained = submissionOutputs.find(submission.serial);
    if (retained != submissionOutputs.end()) {
        releasedOutputs = std::move(retained->second);
        submissionOutputs.erase(retained);
    }
    for (const auto address : submission.labelWrites) {
        const auto found = worker.unfinishedWrites.find(address);
        if (found != worker.unfinishedWrites.end() && --found->second == 0) worker.unfinishedWrites.erase(found);
    }
    if (worker.unretired != 0) --worker.unretired;
}

void MetalDriver::Impl::ReapTimeline() {
    std::lock_guard lock(gpuMutex);
    NativeGuestMemory::BorrowedRangesScope scope(ranges);
    timeline.Reap();
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
    {
        // Named for profiles (ANYPS5_METAL_GPU_TIMING thread breakdown, Instruments).
        char name[32];
        std::snprintf(name, sizeof name, "agc-queue-%u", id);
        pthread_setname_np(name);
    }
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
                    changed.wait(lock, [&] { return stopping || failure || !worker->pending.empty() || reapSignal; });
                    if (failure) std::rethrow_exception(failure);
                    if (stopping) break;
                    if (worker->pending.empty()) {
                        // GPU work behind a deferred retirement finished while this worker was idle.
                        reapSignal = false;
                        lock.unlock();
                        ReapTimeline();
                        continue;
                    }
                    submission = std::move(worker->pending.front());
                    worker->pending.pop_front();
                    worker->active = true;
                    ++worker->unretired;
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
                bool retireLater = false;
                if (submission.suspend) {
                    // A suspend point needs the GPU idle.
                    NativeGuestMemory::BorrowedRangesScope scope(capturedRanges);
                    CompletePriorGpuWorkAndCopyBack();
                } else {
                    NativeGuestMemory::BorrowedRangesScope scope(capturedRanges);
                    const AgcCapture::Context capture{capturedRanges, gpuMutex, [this] { CompletePriorGpuWorkAndCopyBack(); }};
                    AgcCapture::BeforeExecute(submission, worker->state, capture);
                    ExecuteSubmission(submission, worker->state);
                    AgcCapture::AfterExecute(submission, capture);
                    // GPU work may still be running: retire the submission (and so let the guest
                    // submit past it, MAX_QUEUED) when it completed, while this worker goes on.
                    std::lock_guard gpuLock(gpuMutex);
                    // The retirement is guest-visible (the guest may then read what the submission
                    // rendered): its copy-backs are committed ahead of it (MetalLazyCopyBacks rule 1).
                    MaterializeCopyBacks(MetalLazyCopyBacks::Cause::Retire);
                    // The GPU runs the submission's batched work while this worker waits or goes on.
                    timeline.Flush();
                    if (!timeline.Empty()) {
                        retireLater = true;
                        auto retired = std::make_shared<Submission>(std::move(submission));
                        timeline.Defer([this, worker, retired] {
                            std::vector<std::shared_ptr<IVideoOutput>> outputs;
                            {
                                std::lock_guard lock(mutex);
                                RetireSubmission(*worker, *retired, outputs);
                            }
                            changed.notify_all();
                        });
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    if (failure) std::rethrow_exception(failure);
                    if (!retireLater) RetireSubmission(*worker, submission, releasedOutputs);
                    worker->running = false;
                    worker->active = false;
                    if (id == 0) { queue0Executing = 0; queue0Awaited = 0; }
                }
                submission = Submission{};
                changed.notify_all();
            }
        }
    } catch (const DriverStopped&) {
        const auto error = std::make_exception_ptr(ProcessShutdown{});
        for (const auto& [offset, flip] : submission.flips) { static_cast<void>(offset); flip->Fail(error); }
    } catch (const ProcessShutdown&) {
        // VideoOut throws this once the process stop fires (RenderingWait::Wait,
        // FlipRequest::GpuReady). It is a requested stop, not a driver failure:
        // never make it sticky, or Shutdown() rethrows a non-std::exception.
        // Stop admission so no caller waits on workers that have exited.
        const auto error = std::current_exception();
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
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

void MetalDriver::ReplaceShaderRegistryForReplay(std::shared_ptr<const DriverDetail::ShaderRegistry> registry) {
    require(registry != nullptr, "replay shader registry is null");
    auto merged = std::make_shared<DriverDetail::ShaderRegistry>(*registry);
    const auto nullPixelAddress = DriverDetail::NullPixelProgramAddress();
    std::lock_guard lock(impl->mutex);
    // Keeps the driver's own null pixel program (and what it prepared) across replacements.
    if (const auto found = impl->shaders->find(nullPixelAddress); found != impl->shaders->end()) (*merged)[nullPixelAddress] = found->second;
    else (*merged)[nullPixelAddress] = {DriverDetail::CaptureNullPixelShader()};
    impl->shaders = std::move(merged);
}

void MetalDriver::ReplaceQueueStateForReplay(std::uint32_t queue, const QueueState& state) {
    std::lock_guard lock(impl->mutex);
    auto& worker = impl->workers[queue];
    require(!worker.active && worker.pending.empty(), "replay queue state changes need an idle queue");
    worker.state = state;
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
        generation = impl->rangesRevision;
        NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
        snapshot = DriverDetail::ReadRegisteredShader(reinterpret_cast<std::uintptr_t>(shader));
    }
    std::unique_lock lock(impl->mutex);
    impl->WaitForMappingAdmission(lock);
    require(generation == impl->rangesRevision, "borrowed ranges changed while shader was captured");
    auto registry = std::make_shared<DriverDetail::ShaderRegistry>(*impl->shaders);
    const auto snapshotAddress = snapshot->codeAddress;
    (*registry)[snapshotAddress] = {std::move(snapshot)};
    impl->shaders = std::move(registry);
}

void MetalDriver::RegisterShaderWithPublication(std::uint64_t guestHeaderAddress,
    std::span<const ReadableGuestRange> ranges, const std::function<void()>& publish,
    const std::function<void()>& rollback) {
    impl->CheckFailureAndStopping();
    require(!Impl::OnWorkerThread(), "worker cannot publish a guest shader");
    require(!ranges.empty() && static_cast<bool>(publish) && static_cast<bool>(rollback),
        "shader publication requires readable ranges and publication/rollback callbacks");
    std::exception_ptr rollbackFailure;
    try {
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
                (*registry)[snapshot->codeAddress] = {snapshot};
                impl->shaders = std::move(registry);
            } catch (...) {
                try { rollback(); }
                catch (...) {
                    rollbackFailure = std::current_exception();
                    // Close admission before releasing capture/publication,
                    // but defer output callbacks until every lock is released.
                    if (!impl->failure) impl->failure = rollbackFailure;
                    throw;
                }
                throw;
            }
            return;
        }
    } catch (...) {
        // Also wakes submission-retained outputs and retires pending flips.
        // ReadablePublicationScope and both driver/GPU locks have unwound.
        if (rollbackFailure) impl->ReportFailure(rollbackFailure);
        throw;
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
    // Waiters on outputs/flips see the same requested-stop signal VideoOut already handles, never a driver failure.
    const auto error = std::make_exception_ptr(ProcessShutdown{});
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
        timeline.SetBatchDevice(nullptr);
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
    // Only reading the display buffer needs the guest ranges (gpuMutex). The upload, the wait for a
    // drawable and for the present run on the presentation's own queue without it, so the queue
    // workers keep encoding meanwhile.
    std::vector<std::byte> pixels;
    // Whether `pixels` holds the display buffer's pixels, or is empty because the window is
    // minimized (an empty drawable presents nothing, so nothing is read: the display buffer may
    // not be readable then). Otherwise Present validates the request and throws.
    bool decided = false;
    {
        std::unique_lock gpuLock(impl->gpuMutex);
        impl->CheckFailureAndStopping();
        if (buffer != nullptr && window.context != nullptr && window.getDrawableSize != nullptr && buffer->width == window.width &&
            buffer->height == window.height) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            window.getDrawableSize(window.context, &width, &height);
            decided = true;
            if (width != 0 && height != 0) {
                NativeGuestMemory::BorrowedRangesScope scope(impl->ranges);
                // The flip drained, so its display buffer is normally in guest memory already; a
                // surface still newer on the GPU is copied back (and waited for) first.
                impl->BeforeCpuRead(buffer->address, DisplayBufferSize(*buffer));
                pixels = MetalPresentation::ReadPixels(*buffer);
            }
        }
        if (AgcCapture::Active()) {
            const AgcCapture::Context capture{impl->ranges, impl->gpuMutex, [impl = impl.get()] { impl->CompletePriorGpuWorkAndCopyBack(); }};
            AgcCapture::Presented(buffer, decided && !pixels.empty() ? &pixels : nullptr, capture);
        }
    }
    bool ready = false;
    impl->presentation->Present(window, buffer, opaque, gpuReady ? +[](void* value) { *static_cast<bool*>(value) = true; } : nullptr, &ready,
        decided ? &pixels : nullptr);
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
                NativeGuestMemory::BorrowedRangesScope current(ranges);
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
        NativeGuestMemory::BorrowedRangesScope current(ranges);
        try {
            CaptureAndValidate(tail, tailAddress, stalled.rewindWords);
            CaptureIndirectRegisterLists(tail);
        } catch (const DriverStopped&) {
            throw;
        } catch (const std::exception& error) {
            if (!skipInvalidCommandBuffers.load(std::memory_order_relaxed)) throw;
            std::fprintf(stderr, "[metal] skipped the REWIND tail at 0x%llx (%zu words, queue %u): %s\n",
                static_cast<unsigned long long>(tailAddress), stalled.rewindWords, stalled.queue, error.what());
            return;
        }
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
