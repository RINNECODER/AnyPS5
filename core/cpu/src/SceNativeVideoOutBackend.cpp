#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include <array>
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

void noEventRegistrations(const VideoOutConfig& config) {
    require(config.flipEvents.empty() && config.vblankEvents.empty() && config.preVblankEvents.empty() &&
            config.outputModeEvents.empty(), "Unsupported SCE VideoOut event registration in native adapter");
}

}

struct SceNativeVideoOutBackend::Impl {
    static inline thread_local Impl* workerContext = nullptr;
    struct WorkerScope {
        Impl* previous;
        explicit WorkerScope(Impl* context) : previous(workerContext) { workerContext = context; }
        ~WorkerScope() { workerContext = previous; }
    };
    Machine& machine;
    AgcDriver::PresentationWindow window;
    VideoOutCompletionCallbacks completion;
    std::stop_source stop;
    std::shared_ptr<FlipQueue> queue = std::make_shared<FlipQueue>();
    mutable std::mutex stateMutex;
    std::mutex shutdownMutex;
    std::array<std::shared_ptr<VideoOutConfig>, VIDEO_OUT_NUM_MAX> configs{};
    std::array<std::shared_ptr<AgcDriver::IVideoOutput>, VIDEO_OUT_NUM_MAX> outputs{};
    std::jthread presenter;
    std::jthread vblank;
    std::unique_ptr<std::stop_callback<std::function<void()>>> externalStop;
    bool stopped = false;
    std::exception_ptr shutdownFailure;

    Impl(Machine& guest, const AgcDriver::PresentationWindow& target,
         const VideoOutCompletionCallbacks& callbacks) : machine(guest), window(target), completion(callbacks) {
        require(window.context && window.getDrawableSize && window.metalLayer && window.width && window.height,
                "Unsupported SCE VideoOut native window: missing stable Metal presentation context");
        require(window.metalLayer(window.context) != nullptr,
                "Unsupported SCE VideoOut native window: missing Metal layer");
        require(completion.processTime && completion.processTimeCounter && completion.flipEvent,
                "Unsupported SCE VideoOut native completion: missing clock or event dispatch");
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
        checkQueue();
        std::shared_ptr<VideoOutConfig> config;
        {
            std::lock_guard lock(stateMutex);
            require(handle > 0 && handle < VIDEO_OUT_NUM_MAX && configs[handle],
                    "SCE VideoOut invalid output handle");
            config = configs[handle];
        }
        std::lock_guard lock(config->mutex);
        config->Check();
        return config;
    }

    std::int32_t open(std::int32_t user, std::int32_t bus, std::int32_t index,
                      const std::optional<SceVideoOutOpenParam>& param) {
        require((user == 0 || user == 255) && bus >= 0 && bus <= 2 && index == 0,
                "SCE VideoOut invalid open user, bus or index");
        if (param) {
            require(param->FirstWord == 16 && param->SetPriority <= 1 && param->SetAffinity <= 1,
                    "SCE VideoOut invalid open parameters");
            require(!param->SetPriority && !param->SetAffinity,
                    "Unsupported SCE VideoOut native service thread priority or affinity");
        }
        std::lock_guard lock(stateMutex);
        checkQueue();
        const auto handle = bus + 1;
        std::uint64_t generation = 1;
        if (configs[handle]) {
            std::lock_guard configLock(configs[handle]->mutex);
            require(!configs[handle]->opened, "SCE VideoOut output port is already open");
            require(configs[handle]->generation != std::numeric_limits<std::uint64_t>::max(),
                    "SCE VideoOut generation overflow");
            generation = configs[handle]->generation + 1;
        }
        auto config = std::make_shared<VideoOutConfig>(stop.get_token());
        config->generation = generation;
        config->width = window.width;
        config->height = window.height;
        config->opened = true;
        config->flipStatus.flipArg = -1;
        config->flipStatus.currentBuffer = -1;
        auto output = CreateVideoOutput(config, queue);
        AgcDriverRegisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), output);
        if (stop.stop_requested()) {
            AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), output);
            throw ProcessShutdown{};
        }
        configs[handle] = std::move(config);
        outputs[handle] = std::move(output);
        return handle;
    }

    std::int32_t close(std::int32_t handle) {
        std::lock_guard lock(stateMutex);
        checkQueue();
        require(handle > 0 && handle < VIDEO_OUT_NUM_MAX && outputs[handle],
                "SCE VideoOut invalid close handle");
        const auto config = configs[handle];
        {
            std::lock_guard configLock(config->mutex);
            config->Check();
            noEventRegistrations(*config);
        }
        AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), outputs[handle]);
        outputs[handle].reset();
        {
            std::lock_guard configLock(config->mutex);
            config->opened = false;
            config->closing = true;
            config->vblankCond.notify_all();
        }
        queue->changed.notify_all();
        return 0;
    }

    SceVideoOutStatusResult status(std::int32_t handle) const {
        const auto config = getConfig(handle);
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
        require(set >= 0 && set < VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX && start >= 0 &&
                start < VIDEO_OUT_BUFFER_NUM_MAX && !rows.empty() && rows.size() <= VIDEO_OUT_BUFFER_NUM_MAX &&
                rows.size() <= static_cast<std::size_t>(VIDEO_OUT_BUFFER_NUM_MAX - start),
                "SCE VideoOut invalid buffer set, slot or count");
        require(category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED ||
                category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED,
                "SCE VideoOut invalid buffer category");
        const auto config = getConfig(handle);
        BufferAttributeGroup group{nativeAttribute(attribute), category, true};
        std::array<VideoOutBuffer, VIDEO_OUT_BUFFER_NUM_MAX> validated{};
        for (std::size_t index = 0; index < rows.size(); ++index) {
            require(!rows[index].Reserved[0] && !rows[index].Reserved[1],
                    "SCE VideoOut reserved buffer pointers are set");
            validated[index] = {set, rows[index].DataAddress, rows[index].MetadataAddress};
            const auto display = DescribeVideoOutBuffer(validated[index], group);
            const auto dataBytes = AgcDriverDisplayBufferSize_nid_postfix(display);
            machine.CheckAccess(display.address, dataBytes, Permission::Read);
            if (display.dccAddress) {
                const auto metadataBytes = AgcDriver::Graphics::DccKeyBytes(dataBytes);
                require(metadataBytes != 0, "SCE VideoOut empty DCC metadata range");
                machine.CheckAccess(display.dccAddress, metadataBytes, Permission::Read);
            }
        }
        std::lock_guard lock(config->mutex);
        config->Check();
        require(!config->groups[set].occupied, "SCE VideoOut buffer attribute set is occupied");
        for (std::size_t index = 0; index < rows.size(); ++index)
            require(!config->buffers[start + index].Occupied(), "SCE VideoOut buffer slot is occupied");
        config->groups[set] = group;
        config->width = attribute.Width;
        config->height = attribute.Height;
        for (std::size_t index = 0; index < rows.size(); ++index)
            config->buffers[start + index] = validated[index];
        return 0;
    }

    std::int32_t flipRate(std::int32_t handle, std::int32_t rate) {
        require(rate >= 0 && rate <= 2, "SCE VideoOut invalid flip rate");
        const auto config = getConfig(handle);
        std::lock_guard lock(config->mutex);
        config->Check();
        config->flipRate = rate;
        return 0;
    }

    std::int32_t unregisterBuffers(std::int32_t handle, std::int32_t set) {
        require(set >= 0 && set < VIDEO_OUT_BUFFER_ATTRIBUTE_NUM_MAX, "SCE VideoOut invalid buffer set index");
        const auto config = getConfig(handle);
        std::lock_guard lock(config->mutex);
        config->Check();
        require(config->groups[set].occupied, "SCE VideoOut buffer attribute set is not registered");
        for (std::size_t index = 0; index < config->buffers.size(); ++index)
            if (config->buffers[index].groupIndex == set)
                require(config->bufferPending[index] == 0, "SCE VideoOut cannot unregister a buffer with a pending flip");
        config->groups[set] = BufferAttributeGroup{};
        for (auto& buffer : config->buffers) if (buffer.groupIndex == set) buffer = VideoOutBuffer{};
        return 0;
    }

    std::array<std::shared_ptr<VideoOutConfig>, VIDEO_OUT_NUM_MAX> snapshotConfigs() const {
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
        for (const auto& config : snapshot) if (config) {
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
                    CompleteFlip(*request, completion);
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
                for (const auto& config : snapshot) if (config) {
                    std::lock_guard lock(config->mutex);
                    if (!config->opened || config->closing || config->failure) continue;
                    noEventRegistrations(*config);
                    require(config->vblankStatus.count != std::numeric_limits<std::uint64_t>::max(),
                            "SCE VideoOut vblank counter overflow");
                    ++config->vblankStatus.count;
                    config->vblankStatus.processTime = completion.processTime(completion.context);
                    config->vblankStatus.processTimeCounter = completion.processTimeCounter(completion.context);
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
        std::array<std::shared_ptr<AgcDriver::IVideoOutput>, VIDEO_OUT_NUM_MAX> retained;
        {
            std::lock_guard lock(stateMutex);
            retained.swap(outputs);
            for (const auto& config : configs) if (config) {
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
        for (std::size_t handle = 1; handle < retained.size(); ++handle) if (retained[handle])
            attempt([&] { AgcDriverUnregisterVideoOutput_nid_postfix(static_cast<std::uint32_t>(handle), retained[handle]); });
        {
            std::lock_guard lock(queue->mutex);
            if (queue->failure) attempt([&] { std::rethrow_exception(queue->failure); });
        }
        attempt([] { AgcDriverShutdown_nid_postfix(); });
        attempt([&] { AgcDriverReleaseWindow_nid_postfix(window.context); });
        stopped = true;
        if (shutdownFailure) std::rethrow_exception(shutdownFailure);
    }
};

SceNativeVideoOutBackend::SceNativeVideoOutBackend(Machine& machine, const AgcDriver::PresentationWindow& window,
                                                 const VideoOutCompletionCallbacks& completion,
                                                 std::stop_token processStop) :
    impl(std::make_shared<Impl>(machine, window, completion)) {
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
    return callbacks;
}

std::shared_ptr<VideoOutConfig> SceNativeVideoOutBackend::GetConfig(std::int32_t handle) const { return impl->getConfig(handle); }
void SceNativeVideoOutBackend::RequestStop() { impl->requestStop(); }
void SceNativeVideoOutBackend::Shutdown() { impl->shutdown(); }

}
