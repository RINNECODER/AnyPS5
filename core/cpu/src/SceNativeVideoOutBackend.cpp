#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include <array>
#include <atomic>
#include <algorithm>
#include <map>
#include <vector>
#include <bit>
#include <chrono>
#include <functional>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Cpu {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

VideoOutBufferAttribute2 nativeAttribute(const SceVideoOutAttribute& attribute) {
    static_assert(sizeof(VideoOutBufferAttribute2) == sizeof(SceVideoOutAttribute));
    static_assert(offsetof(VideoOutBufferAttribute2, option) == offsetof(SceVideoOutAttribute, Option));
    static_assert(offsetof(VideoOutBufferAttribute2, pixel_format) == offsetof(SceVideoOutAttribute, PixelFormat));
    static_assert(offsetof(VideoOutBufferAttribute2, dcc_cb_register_clear_color) == offsetof(SceVideoOutAttribute, DccClearColor));
    static_assert(offsetof(VideoOutBufferAttribute2, dcc_control) == offsetof(SceVideoOutAttribute, DccControl));
    static_assert(offsetof(VideoOutBufferAttribute2, reserved1) == offsetof(SceVideoOutAttribute, Reserved1));
    return std::bit_cast<VideoOutBufferAttribute2>(attribute);
}

thread_local const void* videoOutMappingCallbackContext = nullptr;
void rejectVideoOutMappingReentry() {
    require(videoOutMappingCallbackContext == nullptr, "SCE VideoOut mapping callback reentry is unsupported");
}

static_assert(VideoOutError::InvalidValue == VIDEO_OUT_ERROR_INVALID_VALUE &&
              VideoOutError::InvalidHandle == VIDEO_OUT_ERROR_INVALID_HANDLE &&
              VideoOutError::InvalidIndex == VIDEO_OUT_ERROR_INVALID_INDEX &&
              VideoOutError::SlotOccupied == VIDEO_OUT_ERROR_SLOT_OCCUPIED &&
              VideoOutError::ResourceBusy == VIDEO_OUT_ERROR_RESOURCE_BUSY &&
              VideoOutError::FlipQueueFull == VIDEO_OUT_ERROR_FLIP_QUEUE_FULL &&
              VideoOutError::InvalidCategory == VIDEO_OUT_ERROR_INVALID_CATEGORY &&
              VideoOutError::InvalidEventQueue == VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE &&
              VideoOutError::UnavailableOutputMode == VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE);
static_assert(VideoOutEventFlip == VIDEO_OUT_EVENT_FLIP && VideoOutEventVblank == VIDEO_OUT_EVENT_VBLANK);
static_assert(sizeof(SceVideoOutFlipStatus) == sizeof(VideoOutFlipStatus) &&
              offsetof(SceVideoOutFlipStatus, FlipPendingNum) == offsetof(VideoOutFlipStatus, flipPendingNum) &&
              offsetof(SceVideoOutFlipStatus, SubmitProcessTimeCounter) == offsetof(VideoOutFlipStatus, submitProcessTimeCounter));
static_assert(sizeof(SceVideoOutVblankStatus) == sizeof(VideoOutVblankStatus) &&
              offsetof(SceVideoOutVblankStatus, Flags) == offsetof(VideoOutVblankStatus, flags));

}

struct SceNativeVideoOutBackend::Impl {
    static inline thread_local Impl* workerContext = nullptr;
    struct MappingCallbackScope {
        const void* previous;
        explicit MappingCallbackScope(Impl* context) : previous(videoOutMappingCallbackContext) { videoOutMappingCallbackContext = context; }
        ~MappingCallbackScope() { videoOutMappingCallbackContext = previous; }
    };
    void rejectMappingReentry() const {
        rejectVideoOutMappingReentry();
    }
    struct WorkerScope {
        Impl* previous;
        explicit WorkerScope(Impl* context) : previous(workerContext) { workerContext = context; }
        ~WorkerScope() { workerContext = previous; }
    };
    Machine& machine;
    AgcDriver::PresentationWindow window;
    VideoOutCompletionCallbacks completion;
    // Forwards each completed flip to the owner's callback, then to the equeue.
    VideoOutCompletionCallbacks presented;
    SceVideoOutEventSink events;
    std::atomic<std::uint64_t> cpuFlipSerial{0};
    std::stop_source stop;
    std::shared_ptr<FlipQueue> queue = std::make_shared<FlipQueue>();
    mutable std::mutex stateMutex;
    std::mutex shutdownMutex;
    std::map<std::int32_t, std::shared_ptr<VideoOutConfig>> configs;
    std::map<std::int32_t, std::shared_ptr<AgcDriver::IVideoOutput>> outputs;
    std::array<std::int32_t, 3> ports{};
    static inline std::atomic<std::uint64_t> nextHandle{1};
    std::mutex mappingMutex;
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> mappings;
    std::shared_ptr<const void> mappingOwner;
    std::uint64_t mappingGeneration = 0;
    struct Segment {
        std::uint64_t address;
        std::span<std::byte> bytes;
        std::uint64_t identity;
    };
    struct Registration {
        std::vector<Segment> segments;
        std::shared_ptr<const void> owner;
        std::uint64_t generation;
    };
    std::map<std::pair<std::int32_t, std::int32_t>, Registration> registrations;
    std::jthread presenter;
    std::jthread vblank;
    std::unique_ptr<std::stop_callback<std::function<void()>>> externalStop;
    bool stopped = false;
    std::exception_ptr shutdownFailure;

    Impl(Machine& guest, const AgcDriver::PresentationWindow& target,
         const VideoOutCompletionCallbacks& callbacks, const SceVideoOutMemoryConfiguration& memory,
         SceVideoOutEventSink sink) :
        machine(guest), window(target), completion(callbacks), events(std::move(sink)),
        mappings(memory.Ranges.begin(), memory.Ranges.end()), mappingOwner(memory.Owner), mappingGeneration(memory.Generation) {
        require(mappings.empty() || mappingOwner != nullptr, "SCE VideoOut mappings require genuine backing ownership");
        AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(mappings);
        require(window.context && window.getDrawableSize && window.metalLayer && window.width && window.height,
                "Unsupported SCE VideoOut native window: missing stable Metal presentation context");
        require(window.metalLayer(window.context) != nullptr,
                "Unsupported SCE VideoOut native window: missing Metal layer");
        require(completion.processTime && completion.processTimeCounter && completion.flipEvent,
                "Unsupported SCE VideoOut native completion: missing clock or event dispatch");
        presented = {this, &Impl::processTime,
                     &Impl::processTimeCounter, &Impl::flipCompleted};
    }
    static std::uint64_t processTime(void* context) {
        const auto& outer = static_cast<Impl*>(context)->completion;
        return outer.processTime(outer.context);
    }
    static std::uint64_t processTimeCounter(void* context) {
        const auto& outer = static_cast<Impl*>(context)->completion;
        return outer.processTimeCounter(outer.context);
    }
    // Runs under the config mutex from CompleteFlip. open() makes a port's
    // generation equal to its opaque handle, so the generation names the output.
    static void flipCompleted(void* context, VideoOutConfig& config, std::int64_t argument) {
        auto& self = *static_cast<Impl*>(context);
        self.completion.flipEvent(self.completion.context, config, argument);
        if (self.events.Publish)
            self.events.Publish(static_cast<std::int32_t>(config.generation), VIDEO_OUT_EVENT_FLIP, argument);
    }

    void start(std::stop_token processStop) {
        try {
            presenter = std::jthread([this] { presentLoop(); });
            vblank = std::jthread([this] { vblankLoop(); });
            externalStop = std::make_unique<std::stop_callback<std::function<void()>>>(
                processStop, std::function<void()>([this] { requestStop(); }));
        } catch (...) {
            requestStop();
            if (presenter.joinable()) presenter.join();
            if (vblank.joinable()) vblank.join();
            throw;
        }
    }

    void checkQueue() const {
        std::lock_guard lock(queue->mutex);
        if (queue->failure) std::rethrow_exception(queue->failure);
        if (queue->stopping || stop.stop_requested()) throw ProcessShutdown{};
    }

    std::shared_ptr<VideoOutConfig> getConfig(std::int32_t handle) const {
        auto config = find(handle);
        require(config != nullptr, "SCE VideoOut invalid output handle");
        return config;
    }

    // Null for a handle that was never opened or is already closed.
    std::shared_ptr<VideoOutConfig> find(std::int32_t handle) const {
        checkQueue();
        std::shared_ptr<VideoOutConfig> config;
        {
            std::lock_guard lock(stateMutex);
            const auto found = configs.find(handle);
            if (handle <= 0 || found == configs.end()) return nullptr;
            config = found->second;
        }
        std::lock_guard lock(config->mutex);
        config->Check();
        return config;
    }

    std::int32_t open(std::int32_t user, std::int32_t bus, std::int32_t index,
                      const std::optional<SceVideoOutOpenParam>& param) {
        rejectMappingReentry();
        if (!((user == 0 || user == 255) && bus >= 0 && bus <= 2 && index == 0))
            return VIDEO_OUT_ERROR_INVALID_VALUE;
        // Host presentation threads have no guest priority or affinity; a valid
        // request is accepted and has no further effect.
        if (param && (param->FirstWord != 16 || param->SetPriority > 1 || param->SetAffinity > 1))
            return VIDEO_OUT_ERROR_INVALID_VALUE;
        std::lock_guard lock(stateMutex);
        checkQueue();
        if (ports[bus] != 0) return VIDEO_OUT_ERROR_RESOURCE_BUSY;
        const auto numericHandle = nextHandle.fetch_add(1);
        require(numericHandle <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()),
                "SCE VideoOut opaque handle space exhausted");
        const auto handle = static_cast<std::int32_t>(numericHandle);
        const auto generation = numericHandle;
        auto config = std::make_shared<VideoOutConfig>(stop.get_token());
        config->generation = generation;
        config->width = window.width;
        config->height = window.height;
        config->opened = true;
        config->flipStatus.flipArg = -1;
        config->flipStatus.currentBuffer = -1;
        auto output = CreateVideoOutput(config, queue);
        configs.emplace(handle, config);
        try {
            outputs.emplace(handle, output);
            AgcDriverRegisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), output);
            if (stop.stop_requested()) {
                AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), output);
                throw ProcessShutdown{};
            }
        } catch (...) {
            outputs.erase(handle); configs.erase(handle);
            throw;
        }
        ports[bus] = handle;
        return handle;
    }

    std::int32_t close(std::int32_t handle) {
        rejectMappingReentry();
        std::vector<Registration> retired;
        retired.reserve(VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX);
        std::unique_lock mappingLock(mappingMutex);
        std::unique_lock lock(stateMutex);
        checkQueue();
        if (handle <= 0 || !outputs.contains(handle)) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        const auto config = configs.at(handle);
        {
            std::lock_guard configLock(config->mutex);
            config->Check();
            require(config->flipStatus.flipPendingNum == 0,
                    "SCE VideoOut cannot close an output with a pending flip");
            config->closing = true; // Stop new reservation before driver removal.
        }
        try {
            AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), outputs.at(handle));
        } catch (...) {
            std::lock_guard configLock(config->mutex);
            config->closing = false;
            throw;
        }
        outputs.erase(handle);
        configs.erase(handle);
        for (auto& port : ports) if (port == handle) port = 0;
        {
            std::lock_guard configLock(config->mutex);
            config->opened = false;
            config->groups = {};
            config->buffers = {};
            config->vblankCond.notify_all();
        }
        for (auto found = registrations.begin(); found != registrations.end();) {
            if (found->first.first == handle) {
                retired.push_back(std::move(found->second));
                found = registrations.erase(found);
            } else ++found;
        }
        lock.unlock(); mappingLock.unlock();
        queue->changed.notify_all();
        return 0;
    }

    // Capture each host segment from the owner-provided publication, with its
    // mapping identity. Never reinterpret a numeric guest VA as a host pointer.
    static std::vector<Segment> capture(std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
                                         std::uint64_t address, std::size_t bytes) {
        std::vector<Segment> result;
        require(bytes && bytes <= std::numeric_limits<std::uint64_t>::max() - address,
                "SCE VideoOut invalid backing extent");
        auto cursor = address;
        const auto end = address + bytes;
        while (cursor < end) {
            const auto found = std::find_if(ranges.begin(), ranges.end(), [cursor](const auto& range) {
                return cursor >= range.guestAddress && cursor - range.guestAddress < range.host.size();
            });
            require(found != ranges.end(), "SCE VideoOut registered extent has no owned backing");
            const auto offset = static_cast<std::size_t>(cursor - found->guestAddress);
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(end - cursor, found->host.size() - offset));
            result.push_back({cursor, found->host.subspan(offset, count), found->identity});
            cursor += count;
        }
        return result;
    }

    void mutateMappings(std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
                        std::uint64_t generation, const std::function<void()>& mutateCpu,
                        std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner) {
        rejectMappingReentry();
        require(mutateCpu && previousOwner && nextOwner, "SCE VideoOut mapping transaction requires genuine owners and CPU mutation");
        std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> next(ranges.begin(), ranges.end());
        { AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(next); }
        std::shared_ptr<const void> retiredOwner;
        std::unique_lock mappingLock(mappingMutex);
        require(generation > mappingGeneration, "SCE VideoOut mapping generation must increase");
        for (const auto& [key, registration] : registrations) {
            static_cast<void>(key);
            for (const auto& segment : registration.segments) {
                std::vector<Segment> replacement;
                try { replacement = capture(next, segment.address, segment.bytes.size()); }
                catch (const std::runtime_error&) {
                    throw std::runtime_error("SCE VideoOut registered VideoOut backing cannot be retired or rebound");
                }
                std::size_t offset = 0;
                for (const auto& part : replacement) {
                    require(part.bytes.data() == segment.bytes.data() + offset && part.identity == segment.identity,
                            "SCE VideoOut registered VideoOut backing cannot be retired or rebound");
                    offset += part.bytes.size();
                }
            }
        }
        // Presenter takes no mappingMutex: WaitIdle may wait for its completion
        // (including APS5_SYNC_FLIP). The registered-extent invariant above is
        // held throughout the driver's mapping admission and CPU publication.
        AgcDriver::Metal::MetalDriver::Get().MutateBorrowedRanges(next, generation, [&] {
            const MappingCallbackScope callback(this);
            mutateCpu();
        }, std::move(previousOwner), nextOwner);
        mappings.swap(next);
        mappingGeneration = generation;
        retiredOwner = std::move(mappingOwner);
        mappingOwner = std::move(nextOwner);
        mappingLock.unlock();
    }

    SceVideoOutStatusResult status(std::int32_t handle) const {
        const auto config = find(handle);
        if (!config) return {VIDEO_OUT_ERROR_INVALID_HANDLE, {}};
        std::lock_guard lock(config->mutex);
        config->Check();
        SceVideoOutStatus output;
        output.Resolution = (config->width >= 3840 || config->height >= 2160) ? 2 : 1;
        output.DynamicRange = 1;
        output.RefreshRate = config->outputMode == VIDEO_OUT_OUTPUT_MODE_119_88HZ
            ? VIDEO_OUT_REFRESH_RATE_119_88HZ : VIDEO_OUT_REFRESH_RATE_59_94HZ;
        return {0, output};
    }

    std::int32_t registerBuffers(std::int32_t handle, std::int32_t set, std::int32_t start,
                                 std::span<const SceVideoOutBuffer> rows,
                                 const SceVideoOutAttribute& attribute, std::int32_t category) {
        rejectMappingReentry();
        if (!(set >= 0 && set < VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX && start >= 0 &&
              start < VIDEO_OUT_BUFFER_NUM_MAX && !rows.empty() && rows.size() <= VIDEO_OUT_BUFFER_NUM_MAX &&
              rows.size() <= static_cast<std::size_t>(VIDEO_OUT_BUFFER_NUM_MAX - start)))
            return VIDEO_OUT_ERROR_INVALID_VALUE;
        if (category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED &&
            category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED)
            return VIDEO_OUT_ERROR_INVALID_CATEGORY;
        std::lock_guard mappingLock(mappingMutex);
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        {
            // Occupancy is an SCE result; it is rechecked under publication below.
            std::lock_guard lock(config->mutex);
            if (config->groups[set].occupied) return VIDEO_OUT_ERROR_INVALID_INDEX;
            for (std::size_t index = 0; index < rows.size(); ++index)
                if (config->buffers[start + index].Occupied()) return VIDEO_OUT_ERROR_SLOT_OCCUPIED;
        }
        BufferAttributeGroup group{nativeAttribute(attribute), category, true};
        std::array<VideoOutBuffer, VIDEO_OUT_BUFFER_NUM_MAX> validated{};
        using MetalDriver = AgcDriver::Metal::MetalDriver;
        std::array<AgcDriver::Metal::ReadableGuestRange, VIDEO_OUT_BUFFER_NUM_MAX * 2> readable{};
        std::size_t readableCount = 0;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            require(!rows[index].Reserved[0] && !rows[index].Reserved[1],
                    "SCE VideoOut reserved buffer pointers are set");
            validated[index] = {set, rows[index].DataAddress, rows[index].MetadataAddress};
            const auto display = DescribeVideoOutBuffer(validated[index], group);
            const auto dataBytes = AgcDriverDisplayBufferSize_nid_postfix(display);
            readable[readableCount++] = {display.address, dataBytes};
            if (display.dccAddress) {
                const auto metadataBytes = AgcDriver::Graphics::DccKeyBytes(dataBytes);
                require(metadataBytes != 0, "SCE VideoOut empty DCC metadata range");
                readable[readableCount++] = {display.dccAddress, metadataBytes};
            }
        }
        Registration registration{{}, mappingOwner, mappingGeneration};
        MetalDriver::Get().WithValidatedReadableRanges(std::span(readable).first(readableCount), [&] {
            require(registration.owner != nullptr, "SCE VideoOut registration requires genuine backing ownership");
            for (const auto& range : std::span(readable).first(readableCount)) {
                auto segments = capture(mappings, range.address, range.bytes);
                for (const auto& segment : segments) {
                    const auto actual = AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(segment.address, segment.bytes.size());
                    require(actual.data() == segment.bytes.data(), "SCE VideoOut owned backing differs from native mapping publication");
                }
                registration.segments.insert(registration.segments.end(), segments.begin(), segments.end());
            }
            std::lock_guard lock(config->mutex);
            config->Check();
            require(!config->groups[set].occupied, "SCE VideoOut buffer attribute set is occupied");
            for (std::size_t index = 0; index < rows.size(); ++index)
                require(!config->buffers[start + index].Occupied(), "SCE VideoOut buffer slot is occupied");
            const auto key = std::pair{handle, set};
            require(!registrations.contains(key), "SCE VideoOut duplicate backing registration");
            registrations.emplace(key, std::move(registration));
            config->groups[set] = group;
            config->width = attribute.Width;
            config->height = attribute.Height;
            for (std::size_t index = 0; index < rows.size(); ++index)
                config->buffers[start + index] = validated[index];
        });
        return 0;
    }

    std::int32_t flipRate(std::int32_t handle, std::int32_t rate) {
        if (rate < 0 || rate > 2) return VIDEO_OUT_ERROR_INVALID_VALUE;
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        std::lock_guard lock(config->mutex);
        config->Check();
        config->flipRate = rate;
        return 0;
    }

    std::int32_t unregisterBuffers(std::int32_t handle, std::int32_t set) {
        rejectMappingReentry();
        if (set < 0 || set >= VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX) return VIDEO_OUT_ERROR_INVALID_INDEX;
        Registration retired;
        std::unique_lock mappingLock(mappingMutex);
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        std::unique_lock lock(config->mutex);
        config->Check();
        if (!config->groups[set].occupied) return VIDEO_OUT_ERROR_INVALID_INDEX;
        for (std::size_t index = 0; index < config->buffers.size(); ++index)
            if (config->buffers[index].groupIndex == set)
                require(config->bufferPending[index] == 0, "SCE VideoOut cannot unregister a buffer with a pending flip");
        config->groups[set] = BufferAttributeGroup{};
        for (auto& buffer : config->buffers) if (buffer.groupIndex == set) buffer = VideoOutBuffer{};
        const auto found = registrations.find({handle, set});
        if (found != registrations.end()) { retired = std::move(found->second); registrations.erase(found); }
        lock.unlock(); mappingLock.unlock();
        return 0;
    }

    // CPU flip: no GPU work precedes it, so it is ready as soon as it is reserved.
    std::int32_t submitFlip(std::int32_t handle, std::int32_t index, std::int32_t mode, std::int64_t argument) {
        rejectMappingReentry();
        if (mode < VIDEO_OUT_FLIP_MODE_VSYNC || mode > 6) return VIDEO_OUT_ERROR_INVALID_VALUE;
        if (index < VIDEO_OUT_BUFFER_INDEX_BLACK || index >= VIDEO_OUT_BUFFER_NUM_MAX) return VIDEO_OUT_ERROR_INVALID_INDEX;
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        std::shared_ptr<AgcDriver::IVideoOutput> output;
        {
            std::lock_guard lock(stateMutex);
            const auto found = outputs.find(handle);
            if (found == outputs.end()) return VIDEO_OUT_ERROR_INVALID_HANDLE;
            output = found->second;
        }
        {
            std::lock_guard lock(config->mutex);
            config->Check();
            if (index >= 0 && !config->buffers[index].Occupied()) return VIDEO_OUT_ERROR_INVALID_INDEX;
        }
        if (queue->reservations.load() >= VIDEO_OUT_FLIP_QUEUE_CAPACITY) return VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
        auto flip = output->Reserve({static_cast<std::uint32_t>(handle), index, static_cast<std::uint32_t>(mode), argument});
        require(flip != nullptr, "SCE VideoOut output returned a null flip reservation");
        const auto serial = ++cpuFlipSerial;
        auto frame = std::make_shared<AgcDriver::FrameTiming>(serial);
        const auto now = AgcDriver::FrameTiming::Clock::now();
        frame->IncludeSubmission(serial, now, now, now, true);
        frame->SetFlip(serial, 0, now, now);
        flip->GpuReady(frame);
        return 0;
    }

    SceVideoOutFlipStatusResult flipStatus(std::int32_t handle) const {
        const auto config = find(handle);
        if (!config) return {VIDEO_OUT_ERROR_INVALID_HANDLE, {}};
        std::lock_guard lock(config->mutex);
        config->Check();
        return {0, std::bit_cast<SceVideoOutFlipStatus>(config->flipStatus)};
    }

    SceVideoOutVblankStatusResult vblankStatus(std::int32_t handle) const {
        const auto config = find(handle);
        if (!config) return {VIDEO_OUT_ERROR_INVALID_HANDLE, {}};
        std::lock_guard lock(config->mutex);
        config->Check();
        return {0, std::bit_cast<SceVideoOutVblankStatus>(config->vblankStatus)};
    }

    SceVideoOutResolutionStatusResult resolutionStatus(std::int32_t handle) const {
        const auto config = find(handle);
        if (!config) return {VIDEO_OUT_ERROR_INVALID_HANDLE, {}};
        std::lock_guard lock(config->mutex);
        config->Check();
        SceVideoOutResolutionStatus status;
        status.FullWidth = status.PaneWidth = config->width;
        status.FullHeight = status.PaneHeight = config->height;
        status.RefreshRate = config->outputMode == VIDEO_OUT_OUTPUT_MODE_119_88HZ
            ? VIDEO_OUT_REFRESH_RATE_119_88HZ : VIDEO_OUT_REFRESH_RATE_59_94HZ;
        status.ScreenSizeInInch = 50.0f;
        return {0, status};
    }

    // Returns after the next vblank of this output. With an equeue sink the
    // guest thread parks in the scheduler; otherwise this host call blocks.
    std::int32_t waitVblank(std::int32_t handle) {
        rejectMappingReentry();
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        if (events.WaitVblank) return events.WaitVblank(handle);
        std::unique_lock lock(config->mutex);
        config->Check();
        const auto count = config->vblankStatus.count;
        config->vblankCond.wait(lock, config->shutdownToken, [&] {
            return !config->opened || config->closing || config->failure || config->vblankStatus.count != count;
        });
        config->Check();
        return 0;
    }

    std::int32_t flipPending(std::int32_t handle) const {
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        std::lock_guard lock(config->mutex);
        config->Check();
        return config->flipStatus.flipPendingNum;
    }

    // Only the default mode is available on the native window.
    std::int32_t configureOutput(std::int32_t handle, std::uint64_t mode) {
        const auto config = find(handle);
        if (!config) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        if (mode != VIDEO_OUT_OUTPUT_MODE_DEFAULT) return VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE;
        std::lock_guard lock(config->mutex);
        config->Check();
        config->outputMode = mode;
        return 0;
    }

    // The native window has no TV safe area; margins are accepted for an open port.
    std::int32_t windowMargins(std::int32_t handle, std::int32_t, std::int32_t) const {
        return find(handle) ? 0 : VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    std::int32_t addEvent(std::uint64_t queue, std::int32_t handle, std::int32_t kind, std::uint64_t udata) {
        if (!find(handle)) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        if (!events.Add) return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
        return events.Add(queue, handle, kind, udata);
    }

    std::int32_t deleteEvent(std::uint64_t queue, std::int32_t handle, std::int32_t kind) {
        if (!find(handle)) return VIDEO_OUT_ERROR_INVALID_HANDLE;
        if (!events.Delete) return VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
        return events.Delete(queue, handle, kind);
    }

    std::map<std::int32_t, std::shared_ptr<VideoOutConfig>> snapshotConfigs() const {
        std::lock_guard lock(stateMutex);
        return configs;
    }

    bool cancelled(const FlipRequest& request) const {
        std::lock_guard lock(request.cfg->mutex);
        return stop.stop_requested() || request.cfg->closing || !request.cfg->opened ||
               request.cfg->generation != request.generation;
    }

    void requestStop() {
        stop.request_stop();
        queue->changed.notify_all();
    }

    void reportFailure(std::exception_ptr error) {
        {
            std::lock_guard lock(queue->mutex);
            if (!queue->failure) queue->failure = error;
        }
        const auto snapshot = snapshotConfigs();
        for (const auto& [handle, config] : snapshot) if (config) {
            std::lock_guard lock(config->mutex);
            if (!config->failure) config->failure = error;
            config->vblankCond.notify_all();
        }
        queue->changed.notify_all();
        AgcDriverReportFailure_nid_postfix(error);
        requestStop();
    }

    void presentLoop() {
        const WorkerScope worker(this);
        std::shared_ptr<FlipRequest> request;
        try {
            while (!stop.stop_requested()) {
                {
                    std::unique_lock lock(queue->mutex);
                    queue->changed.wait(lock, [&] { return stop.stop_requested() || queue->failure || !queue->requests.empty(); });
                    if (queue->failure) std::rethrow_exception(queue->failure);
                    if (stop.stop_requested()) break;
                    request = std::move(queue->requests.front());
                    queue->requests.pop_front();
                }
                try {
                    WaitForFlipVblank(*request);
                    auto target = window;
                    target.width = request->width;
                    target.height = request->height;
                    target.timing = request->timing;
                    const auto gpuReady = +[](void* context) { MarkFlipGpuComplete(*static_cast<FlipRequest*>(context)); };
                    if (request->index >= 0) {
                        const auto display = DescribeVideoOutBuffer(request->buffer, request->group);
                        AgcDriverPresentBuffer_nid_postfix(target, display, gpuReady, request.get());
                    } else {
                        AgcDriverPresentClear_nid_postfix(target, request->index == VIDEO_OUT_BUFFER_INDEX_BLACK,
                                                        gpuReady, request.get());
                    }
                    CompleteFlip(*request, presented);
                } catch (...) {
                    if (!cancelled(*request)) {
                        request->Fail(std::current_exception());
                        throw;
                    }
                }
                request.reset();
                queue->changed.notify_all();
            }
        } catch (const ProcessShutdown&) {
            requestStop();
        } catch (...) {
            reportFailure(std::current_exception());
        }
        request.reset();
        std::list<std::shared_ptr<FlipRequest>> cancelledRequests;
        {
            std::lock_guard lock(queue->mutex);
            cancelledRequests.swap(queue->requests);
        }
        cancelledRequests.clear();
        queue->changed.notify_all();
    }

    void vblankLoop() {
        const WorkerScope worker(this);
        using Frame = std::chrono::duration<std::int64_t, std::ratio<1001, 60000>>;
        const auto start = std::chrono::steady_clock::now();
        try {
            for (std::int64_t frame = 1; !stop.stop_requested(); ++frame) {
                require(frame != std::numeric_limits<std::int64_t>::max(), "SCE VideoOut vblank schedule overflow");
                const auto next = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(Frame(frame));
                {
                    std::unique_lock lock(queue->mutex);
                    queue->changed.wait_until(lock, next, [&] { return stop.stop_requested() || queue->failure; });
                    if (stop.stop_requested()) break;
                    if (queue->failure) std::rethrow_exception(queue->failure);
                }
                const auto snapshot = snapshotConfigs();
                for (const auto& [handle, config] : snapshot) if (config) {
                    std::lock_guard lock(config->mutex);
                    if (!config->opened || config->closing || config->failure) continue;
                    require(config->vblankStatus.count != std::numeric_limits<std::uint64_t>::max(),
                            "SCE VideoOut vblank counter overflow");
                    ++config->vblankStatus.count;
                    config->vblankStatus.processTime = completion.processTime(completion.context);
                    config->vblankStatus.processTimeCounter = completion.processTimeCounter(completion.context);
                    if (events.Publish)
                        events.Publish(handle, VIDEO_OUT_EVENT_VBLANK, static_cast<std::int64_t>(config->vblankStatus.count));
                    config->vblankCond.notify_all();
                }
            }
        } catch (const ProcessShutdown&) {
            requestStop();
        } catch (...) {
            reportFailure(std::current_exception());
        }
    }

    void shutdown() {
        rejectMappingReentry();
        require(workerContext != this, "SCE VideoOut worker cannot shut itself down");
        std::lock_guard shutdownLock(shutdownMutex);
        if (stopped) {
            if (shutdownFailure) std::rethrow_exception(shutdownFailure);
            return;
        }
        requestStop();
        {
            std::lock_guard lock(queue->mutex);
            queue->stopping = true;
        }
        if (presenter.joinable()) presenter.join();
        if (vblank.joinable()) vblank.join();
        std::map<std::int32_t, std::shared_ptr<AgcDriver::IVideoOutput>> retained;
        {
            std::lock_guard lock(stateMutex);
            retained.swap(outputs);
            for (const auto& [handle, config] : configs) if (config) {
                std::lock_guard configLock(config->mutex);
                config->opened = false;
                config->closing = true;
                config->vblankCond.notify_all();
            }
        }
        const auto attempt = [&](auto&& operation) {
            try { operation(); }
            catch (const ProcessShutdown&) {}
            catch (...) { if (!shutdownFailure) shutdownFailure = std::current_exception(); }
        };
        for (const auto& [handle, output] : retained)
            attempt([&] { AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), output); });
        {
            std::lock_guard lock(queue->mutex);
            if (queue->failure) attempt([&] { std::rethrow_exception(queue->failure); });
        }
        attempt([] { AgcDriverShutdown_nid_postfix(); });
        attempt([&] { AgcDriverReleaseWindow_nid_postfix(window.context); });
        std::map<std::pair<std::int32_t, std::int32_t>, Registration> retiredRegistrations;
        std::shared_ptr<const void> retiredOwner;
        {
            std::lock_guard mappingLock(mappingMutex);
            retiredRegistrations.swap(registrations);
            mappings.clear();
            retiredOwner = std::move(mappingOwner);
        }
        {
            std::lock_guard lock(stateMutex);
            for (const auto& [handle, config] : configs) {
                std::lock_guard configLock(config->mutex);
                config->groups = {}; config->buffers = {};
            }
            configs.clear(); ports = {};
        }
        stopped = true;
        if (shutdownFailure) std::rethrow_exception(shutdownFailure);
    }
};

SceNativeVideoOutBackend::SceNativeVideoOutBackend(Machine& machine, const AgcDriver::PresentationWindow& window,
                                                 const VideoOutCompletionCallbacks& completion,
                                                 std::stop_token processStop, const SceVideoOutMemoryConfiguration& memory,
                                                 SceVideoOutEventSink events) :
    impl(std::make_shared<Impl>(machine, window, completion, memory, std::move(events))) {
    impl->start(processStop);
}

SceNativeVideoOutBackend::~SceNativeVideoOutBackend() {
    try { impl->shutdown(); } catch (...) {}
}

SceVideoOutBackend SceNativeVideoOutBackend::GetCallbacks() const {
    const auto weak = std::weak_ptr<Impl>(impl);
    const auto context = [weak] {
        const auto state = weak.lock();
        require(state != nullptr, "Unsupported SCE VideoOut native backend: lifetime has expired");
        return state;
    };
    SceVideoOutBackend callbacks;
    callbacks.Open = [context](auto user, auto bus, auto index, const auto& param) { return context()->open(user, bus, index, param); };
    callbacks.Close = [context](auto handle) { return context()->close(handle); };
    callbacks.GetOutputStatus = [context](auto handle) { return context()->status(handle); };
    callbacks.RegisterBuffers = [context](auto handle, auto set, auto start, auto rows, const auto& attribute, auto category) {
        return context()->registerBuffers(handle, set, start, rows, attribute, category);
    };
    callbacks.SetFlipRate = [context](auto handle, auto rate) { return context()->flipRate(handle, rate); };
    callbacks.UnregisterBuffers = [context](auto handle, auto set) { return context()->unregisterBuffers(handle, set); };
    callbacks.SubmitFlip = [context](auto handle, auto index, auto mode, auto argument) {
        return context()->submitFlip(handle, index, mode, argument);
    };
    callbacks.GetFlipStatus = [context](auto handle) { return context()->flipStatus(handle); };
    callbacks.GetVblankStatus = [context](auto handle) { return context()->vblankStatus(handle); };
    callbacks.GetResolutionStatus = [context](auto handle) { return context()->resolutionStatus(handle); };
    callbacks.WaitVblank = [context](auto handle) { return context()->waitVblank(handle); };
    callbacks.IsFlipPending = [context](auto handle) { return context()->flipPending(handle); };
    callbacks.ConfigureOutput = [context](auto handle, auto mode) { return context()->configureOutput(handle, mode); };
    callbacks.SetWindowModeMargins = [context](auto handle, auto top, auto bottom) {
        return context()->windowMargins(handle, top, bottom);
    };
    callbacks.AddEvent = [context](auto queue, auto handle, auto kind, auto udata) {
        return context()->addEvent(queue, handle, kind, udata);
    };
    callbacks.DeleteEvent = [context](auto queue, auto handle, auto kind) {
        return context()->deleteEvent(queue, handle, kind);
    };
    return callbacks;
}

std::shared_ptr<VideoOutConfig> SceNativeVideoOutBackend::GetConfig(std::int32_t handle) const { return impl->getConfig(handle); }
void SceNativeVideoOutBackend::MutateBorrowedRanges(
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation,
    const std::function<void()>& mutateCpu, std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner) {
    impl->mutateMappings(ranges, generation, mutateCpu, std::move(previousOwner), std::move(nextOwner));
}
void SceNativeVideoOutBackend::RequestStop() { impl->requestStop(); }
void SceNativeVideoOutBackend::Shutdown() { impl->shutdown(); }

struct SceNativeGraphicsSession::Impl {
    std::thread::id mainThread = std::this_thread::get_id();
    std::unique_ptr<AnyPS5::Host::NativeMetalSession> session;
    std::unique_ptr<SceNativeVideoOutBackend> videoOut;
    std::unique_ptr<SceVideoOutImports> imports;
    bool closed = false;
    std::exception_ptr shutdownFailure;

    void shutdown() {
        rejectVideoOutMappingReentry();
        // Checks main-thread ownership before any gates or worker state change.
        // Request-close keeps the real layer alive until presentation/GPU drain.
        require(std::this_thread::get_id() == mainThread, "SCE native graphics session requires the AppKit main thread");
        if (closed) {
            if (shutdownFailure) std::rethrow_exception(shutdownFailure);
            return;
        }
        const auto attempt = [&](auto&& operation) {
            try { operation(); }
            catch (const ProcessShutdown&) {}
            catch (...) { if (!shutdownFailure) shutdownFailure = std::current_exception(); }
        };
        if (session) attempt([&] { session->Window().RequestCloseMainThread(); });
        imports.reset();
        if (videoOut) attempt([&] { videoOut->Shutdown(); });
        videoOut.reset();
        if (session) attempt([&] { session->ShutdownAfterCpuStoppedMainThread(); });
        closed = true;
        if (shutdownFailure) std::rethrow_exception(shutdownFailure);
    }
};

SceNativeGraphicsSession::SceNativeGraphicsSession(std::unique_ptr<Impl> state) : impl(std::move(state)) {}
SceNativeGraphicsSession::~SceNativeGraphicsSession() {
    try { impl->shutdown(); } catch (...) {}
}

std::unique_ptr<SceNativeGraphicsSession> SceNativeGraphicsSession::CreateMainThread(
    Machine& machine, const AnyPS5::Host::NativeMetalSessionConfiguration& configuration,
    const VideoOutCompletionCallbacks& completion, std::stop_token processStop,
    std::uint64_t videoOutGateBase, std::span<const VideoOutAbiAdmission> admissions, SceVideoOutEventSink events) {
    auto state = std::make_unique<Impl>();
    state->session = AnyPS5::Host::NativeMetalSession::CreateMainThread(configuration);
    state->videoOut = std::make_unique<SceNativeVideoOutBackend>(machine,
        state->session->Window().Presentation(configuration.window.contentWidth, configuration.window.contentHeight),
        completion, processStop, SceVideoOutMemoryConfiguration{
            configuration.initialRanges, configuration.initialRangeOwner, configuration.initialGeneration},
        std::move(events));
    state->imports = std::make_unique<SceVideoOutImports>(machine, state->videoOut->GetCallbacks(), videoOutGateBase, admissions);
    return std::unique_ptr<SceNativeGraphicsSession>(new SceNativeGraphicsSession(std::move(state)));
}

AnyPS5::Host::NativeHostWindow& SceNativeGraphicsSession::Window() {
    require(!impl->closed, "SCE native graphics session is closed");
    return impl->session->Window();
}
AgcDriver::Metal::MetalDriver& SceNativeGraphicsSession::Driver() {
    require(!impl->closed, "SCE native graphics session is closed");
    return impl->session->Driver();
}
std::uint64_t SceNativeGraphicsSession::ResolveVideoOut(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize) {
    require(!impl->closed && impl->imports != nullptr, "SCE native graphics session is closed");
    return impl->imports->Resolve(import, symbolType, symbolSize);
}
std::uint64_t SceNativeGraphicsSession::ResolveVideoOutPublicFixture(const SceImport& import) {
    require(!impl->closed && impl->imports != nullptr, "SCE native graphics session is closed");
    return impl->imports->ResolvePublicFixture(import);
}
void SceNativeGraphicsSession::MutateBorrowedRanges(
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation,
    const std::function<void()>& mutateCpu, std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner) {
    require(!impl->closed && impl->videoOut != nullptr, "SCE native graphics session is closed");
    impl->videoOut->MutateBorrowedRanges(ranges, generation, mutateCpu, std::move(previousOwner), std::move(nextOwner));
}
void SceNativeGraphicsSession::RequestStop() {
    if (impl->videoOut) impl->videoOut->RequestStop();
}
void SceNativeGraphicsSession::ShutdownAfterCpuStoppedMainThread() { impl->shutdown(); }

}
