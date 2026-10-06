#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceVideoOut/include/BufferMetadata.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceVideoOut/include/NativeHostWindow.hpp"
#include "prx/libSceVideoOut/include/ControllerSource.hpp"
#import <GameController/GameController.h>
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include <algorithm>
#include <future>
#include <chrono>
#include <cstdlib>
#include <stop_token>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

@interface ReplayMetalLayer : CAMetalLayer
@property(nonatomic, strong) id<CAMetalDrawable> recordedDrawable;
@property(nonatomic) NSUInteger acquisitionCount;
@end
@implementation ReplayMetalLayer
- (id<CAMetalDrawable>)nextDrawable {
    self.framebufferOnly = NO;
    id<CAMetalDrawable> drawable = [super nextDrawable];
    self.recordedDrawable = drawable;
    self.acquisitionCount += 1;
    return drawable;
}
@end

namespace {
using Pixel = std::array<std::uint8_t, 4>;
constexpr std::uint32_t SourceWidth = 8, SourceHeight = 4;
constexpr std::uint64_t BGRA = 0x8000000000000000ull;
constexpr std::uint64_t RGBA = 0x8000000022000000ull;
constexpr std::uint64_t TenBit = 0x0100000000000000ull;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Pixel Pattern(std::uint32_t x, std::uint32_t y) {
    return {static_cast<std::uint8_t>(23 + x * 17), static_cast<std::uint8_t>(67 + y * 31),
            static_cast<std::uint8_t>(149 - x * 11 + y * 7), 255};
}

struct WindowContext {
    ReplayMetalLayer* layer;
    std::uint32_t width = 8;
    std::uint32_t height = 4;
};

void Size(void* context, std::uint32_t* width, std::uint32_t* height) {
    auto& window = *static_cast<WindowContext*>(context);
    *width = window.width;
    *height = window.height;
}

void* Layer(void* context) {
    return (__bridge void*)static_cast<WindowContext*>(context)->layer;
}

struct Completion {
    WindowContext* window;
    id<MTLCommandQueue> queue;
    std::vector<Pixel> expected;
    std::uint32_t calls = 0;
    bool expectDrawable = true;
};

void Ready(void* context) {
    auto& completion = *static_cast<Completion*>(context);
    ++completion.calls;
    Require(completion.calls == 1, "Original presentation entry repeated the completion callback");
    id<CAMetalDrawable> drawable = completion.window->layer.recordedDrawable;
    if (!completion.expectDrawable) {
        Require(drawable == nil, "Minimized presentation unexpectedly acquired a drawable");
        return;
    }
    Require(drawable != nil, "Original presentation entry did not obtain an actual CAMetalLayer drawable");
    id<MTLTexture> texture = drawable.texture;
    Require(texture.width == completion.window->width && texture.height == completion.window->height,
            "Presentation ignored the current drawable-size callback");
    const auto rowBytes = (texture.width * 4u + 255u) & ~NSUInteger{255u};
    auto bytes = [texture.device newBufferWithLength:rowBytes * texture.height options:MTLResourceStorageModeShared];
    Require(bytes != nil, "Drawable independent readback buffer allocation failed");
    std::memset(bytes.contents, 0x35, bytes.length);
    auto commands = [completion.queue commandBuffer];
    auto blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(texture.width, texture.height, 1) toBuffer:bytes destinationOffset:0
       destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * texture.height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted, "Actual drawable independent GPU readback failed");
    Require(completion.expected.size() == texture.width * texture.height, "Independent presentation golden has the wrong extent");
    for (NSUInteger y = 0; y < texture.height; ++y) {
        for (NSUInteger x = 0; x < texture.width; ++x) {
            Pixel actual{};
            std::memcpy(actual.data(), static_cast<const std::byte*>(bytes.contents) + y * rowBytes + x * 4, 4);
            Require(actual == completion.expected[y * texture.width + x],
                    "Completion callback observed unfinished or incorrect drawable pixels at " + std::to_string(x) + "," + std::to_string(y));
        }
    }
    completion.window->layer.recordedDrawable = nil;
}

std::vector<Pixel> Golden(std::uint32_t targetWidth, std::uint32_t targetHeight, bool uniform = false, Pixel value = {}) {
    std::vector<Pixel> result(targetWidth * targetHeight, Pixel{0, 0, 0, 255});
    Require(targetWidth == 8 && (targetHeight == 4 || targetHeight == 8), "Fixture golden only covers independently specified extents");
    const auto top = targetHeight == 8 ? 2u : 0u;
    for (std::uint32_t y = 0; y < SourceHeight; ++y) {
        for (std::uint32_t x = 0; x < SourceWidth; ++x) result[(y + top) * targetWidth + x] = uniform ? value : Pattern(x, y);
    }
    return result;
}

void EncodePixel(std::byte* target, Pixel pixel, std::uint64_t format) {
    const bool rgba = (format & ~TenBit) == RGBA;
    const auto first = rgba ? pixel[2] : pixel[0];
    const auto last = rgba ? pixel[0] : pixel[2];
    if ((format & TenBit) != 0) {
        const std::uint32_t word = (std::uint32_t{first} * 4u + 3u) |
            ((std::uint32_t{pixel[1]} * 4u + 2u) << 10u) |
            ((std::uint32_t{last} * 4u + 1u) << 20u) | (3u << 30u);
        std::memcpy(target, &word, 4);
    } else {
        const std::array<std::uint8_t, 4> texel{first, pixel[1], last, pixel[3]};
        std::memcpy(target, texel.data(), 4);
    }
}

void FillLinear(std::vector<std::byte>& pixels, std::uint64_t format) {
    std::fill(pixels.begin(), pixels.end(), std::byte{0x7b});
    for (std::uint32_t y = 0; y < SourceHeight; ++y) {
        for (std::uint32_t x = 0; x < SourceWidth; ++x) EncodePixel(pixels.data() + (y * 10u + x) * 4u, Pattern(x, y), format);
    }
}

void FillTiled(std::vector<std::byte>& pixels) {
    constexpr std::array<std::uint32_t, 8> columns{0, 4, 8, 12, 128, 132, 136, 140};
    constexpr std::array<std::uint32_t, 4> rows{0, 16, 32, 48};
    std::fill(pixels.begin(), pixels.end(), std::byte{0x69});
    for (std::uint32_t y = 0; y < SourceHeight; ++y) {
        for (std::uint32_t x = 0; x < SourceWidth; ++x) EncodePixel(pixels.data() + rows[y] + columns[x], Pattern(x, y), BGRA);
    }
}

void Present(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer,
             WindowContext& context, id<MTLCommandQueue> queue, std::vector<Pixel> expected) {
    @autoreleasepool {
        context.layer.recordedDrawable = nil;
        Completion completion{&context, queue, std::move(expected)};
        AgcDriverPresentBuffer_nid_postfix(window, buffer, Ready, &completion);
        Require(completion.calls == 1, "Original buffer presentation entry omitted completion");
    }
}


void ReplayOriginalFlipState(AgcDriver::Metal::MetalDriver& driver, WindowContext& window,
                             const AgcDriver::PresentationWindow& target, id<MTLCommandQueue> readbackQueue,
                             std::vector<std::byte>& source) {
    using namespace std::chrono_literals;
    constexpr std::uint32_t Handle = 3;
    std::array<std::array<std::uint32_t, 6>, 2> words{{
        {AgcDriver::FlipPacketHeader, Handle, 0, 1, 0x12345678, 0x11223344},
        {AgcDriver::FlipPacketHeader, Handle, 0, 1, 0x87654321, 0x55667788}}};
    std::array<Packet, 2> packets{{
        {reinterpret_cast<std::uint32_t*>(0xa0000), 6, 0, {}},
        {reinterpret_cast<std::uint32_t*>(0xb0000), 6, 0, {}}}};
    const auto originalWords = words;
    const auto originalPackets = std::as_bytes(std::span(packets));
    const std::vector<std::byte> packetGolden(originalPackets.begin(), originalPackets.end());
    const auto sourceGolden = source;
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 4> ranges{{
        {0x10000, source, false},
        {0x90000, std::as_writable_bytes(std::span(packets)), false},
        {0xa0000, std::as_writable_bytes(std::span(words[0])), false},
        {0xb0000, std::as_writable_bytes(std::span(words[1])), false}}};
    driver.ReplaceBorrowedRanges(ranges, 1);
    std::stop_source stop;
    auto cfg = std::make_shared<VideoOutConfig>(stop.get_token());
    auto queue = std::make_shared<FlipQueue>();
    cfg->opened = true;
    cfg->generation = 7;
    cfg->width = 8;
    cfg->height = 4;
    cfg->flipRate = 1;
    cfg->buffers[0] = {0, 0x10000, 0};
    cfg->groups[0].occupied = true;
    auto& attribute = cfg->groups[0].attribute;
    attribute.pixel_format = BGRA;
    attribute.width = 8;
    attribute.height = 4;
    attribute.pitch_in_pixel = 10;
    attribute.tiling_mode = 1;
    attribute.option = VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY;
    const auto output = CreateVideoOutput(cfg, queue);
    AgcDriverRegisterVideoOutput_nid_postfix(Handle, output);
    auto otherConfig = std::make_shared<VideoOutConfig>(stop.get_token());
    otherConfig->opened = true;
    otherConfig->generation = 8;
    const auto otherOutput = CreateVideoOutput(otherConfig, queue);
    std::future<void> firstWait, secondWait, idle, room;
    std::vector<std::shared_ptr<AgcDriver::IFlipRequest>> otherReservations;
    struct Cleanup {
        std::shared_ptr<AgcDriver::IVideoOutput> output;
        std::stop_source* stop;
        bool done = false;
        ~Cleanup() {
            if (!done) {
                output->Fail(std::make_exception_ptr(std::runtime_error("Original VideoOut replay cancelled")));
                stop->request_stop();
            }
        }
    } cleanup{output, &stop};
    AgcDriver::Submit(reinterpret_cast<const Packet*>(0x90000), 0);
    const auto firstFence = output->CaptureRenderingWait(0);
    AgcDriver::Submit(reinterpret_cast<const Packet*>(0x90000 + sizeof(Packet)), 0);
    const auto secondFence = output->CaptureRenderingWait(0);
    firstWait = std::async(std::launch::async, [firstFence] { firstFence->Wait(); });
    secondWait = std::async(std::launch::async, [secondFence] { secondFence->Wait(); });
    idle = std::async(std::launch::async, [&driver] { driver.WaitIdle(); });
    {
        std::lock_guard lock(cfg->mutex);
        Require(cfg->flipStatus.flipPendingNum == 2 && cfg->bufferPending[0] == 2 && queue->reservations == 2,
                "Original public flip submission did not reserve both pending tickets");
        cfg->buffers[0].dataAddress = 0xc0000;
        cfg->groups[0].attribute.width = 2;
    }
    struct EventState { std::uint32_t calls = 0; std::int64_t expected; } events;
    const VideoOutCompletionCallbacks callbacks{&events,
        +[](void*) -> std::uint64_t { return 101; },
        +[](void*) -> std::uint64_t { return 202; },
        +[](void* context, VideoOutConfig& state, std::int64_t argument) {
            auto& event = *static_cast<EventState*>(context);
            Require(argument == event.expected && state.flipStatus.count == event.calls,
                    "Original flip event did not precede its completed status update");
            Require(state.flipStatus.flipPendingNum == 2 - static_cast<int>(event.calls),
                    "Original flip event observed premature pending retirement");
            ++event.calls;
        }};
    window.width = 8;
    window.height = 4;
    for (std::uint32_t i = 0; i < 2; ++i) {
        std::shared_ptr<FlipRequest> request;
        {
            std::unique_lock lock(queue->mutex);
            Require(queue->changed.wait_for(lock, 2s, [&] { return !queue->requests.empty(); }),
                    "Original public PM4 flip never entered the ready queue");
            request = queue->requests.front();
            queue->requests.pop_front();
        }
        Require(request->ready && !request->terminal && request->reuseTicket == i + 1 &&
                request->buffer.dataAddress == 0x10000 && request->group.attribute.width == 8,
                "Original reserved flip lost its buffer snapshot or reuse ticket");
        auto pacing = std::async(std::launch::async, [request] { WaitForFlipVblank(*request); });
        Cleanup pacingCleanup{output, &stop};
        Require(pacing.wait_for(75ms) == std::future_status::timeout,
                "Original flip pacing ignored its reserved two-vblank interval");
        {
            std::lock_guard lock(cfg->mutex);
            cfg->vblankStatus.count = 2 * i + 1;
            cfg->vblankCond.notify_all();
        }
        Require(pacing.wait_for(75ms) == std::future_status::timeout,
                "Original flip pacing ignored the reserved flip-rate interval");
        {
            std::lock_guard lock(cfg->mutex);
            cfg->vblankStatus.count = 2 * (i + 1);
            cfg->vblankCond.notify_all();
        }
        Require(pacing.wait_for(2s) == std::future_status::ready, "Original vblank pacing did not wake");
        pacing.get();
        pacingCleanup.done = true;
        Completion pixels{&window, readbackQueue, Golden(8, 4)};
        struct ActualCompletion { Completion* pixels; FlipRequest* request; } completion{&pixels, request.get()};
        const auto display = DescribeVideoOutBuffer(request->buffer, request->group);
        auto presentation = std::async(std::launch::async, [&] {
            @autoreleasepool {
                AgcDriverPresentBuffer_nid_postfix(target, display, +[](void* context) {
                    auto& completion = *static_cast<ActualCompletion*>(context);
                    Ready(completion.pixels);
                    MarkFlipGpuComplete(*completion.request);
                }, &completion);
            }
        });
        if (presentation.wait_for(2s) != std::future_status::ready) {
            output->Fail(std::make_exception_ptr(std::runtime_error("Sync flip held the GPU mutex during actual presentation")));
            presentation.get();
            throw std::runtime_error("Sync flip held the GPU mutex during actual presentation");
        }
        presentation.get();
        {
            std::lock_guard lock(cfg->mutex);
            Require(request->gpuComplete && !request->terminal && cfg->flipStatus.count == i &&
                    cfg->flipStatus.flipPendingNum == 2 - static_cast<int>(i),
                    "Actual drawable completion prematurely retired original flip state");
        }
        Require((i == 0 ? firstWait : secondWait).wait_for(50ms) == std::future_status::timeout,
                "Original rendering wait retired at GPU readiness instead of flip completion");
        if (i == 0) {
            for (std::uint32_t extra = 0; extra < 14; ++extra)
                otherReservations.push_back(otherOutput->Reserve({4, VIDEO_OUT_BUFFER_INDEX_BLACK, 1, 0}));
            {
                std::scoped_lock lock(cfg->mutex, otherConfig->mutex);
                Require(queue->reservations == 16 && cfg->flipStatus.flipPendingNum == 2 &&
                        otherConfig->flipStatus.flipPendingNum == 14,
                        "Original shared VideoOut queue did not retain both owners' reservations");
            }
            room = std::async(std::launch::async, [output] { output->WaitForFlipRoom(); });
            Require(room.wait_for(50ms) == std::future_status::timeout,
                    "Original shared VideoOut queue admitted a producer while all sixteen flips were reserved");
        }
        events.expected = i == 0 ? 0x1122334412345678ll : 0x5566778887654321ll;
        CompleteFlip(*request, callbacks);
        if (i == 0) {
            Require(room.wait_for(2s) == std::future_status::ready,
                    "Actual drawable flip completion did not wake the blocked original VideoOut producer");
            room.get();
            {
                std::scoped_lock lock(cfg->mutex, otherConfig->mutex);
                Require(queue->reservations == 15 && cfg->flipStatus.flipPendingNum == 1 &&
                        otherConfig->flipStatus.flipPendingNum == 14 && request->terminal,
                        "Original queue room woke before the presented flip retired its own reservation");
            }
            otherReservations.clear();
            {
                std::lock_guard lock(otherConfig->mutex);
                Require(otherConfig->flipStatus.flipPendingNum == 0,
                        "Original unready VideoOut requests did not roll back the other owner's reservations");
            }
        }
        {
            std::lock_guard lock(cfg->mutex);
            Require(request->terminal && cfg->flipStatus.count == i + 1 && cfg->flipStatus.processTime == 101 &&
                    cfg->flipStatus.processTimeCounter == 202 && cfg->flipStatus.flipArg == events.expected &&
                    cfg->flipStatus.currentBuffer == 0 && cfg->width == 8 && cfg->height == 4 &&
                    cfg->lastFlipVblank == 2 * (i + 1) && cfg->bufferPending[0] == 1 - i && queue->reservations == 1 - i,
                    "Original flip completion lost counters, dimensions, interval, or pending retirement");
        }
        auto& wait = i == 0 ? firstWait : secondWait;
        Require(wait.wait_for(2s) == std::future_status::ready,
                "Original captured rendering wait did not retire its completed ticket");
        wait.get();
        if (i == 0) Require(secondWait.wait_for(50ms) == std::future_status::timeout,
                            "First flip completion incorrectly released the later captured buffer ticket");
    }
    Require(idle.wait_for(2s) == std::future_status::ready, "Original sync flips did not release public WaitIdle");
    idle.get();
    Require(events.calls == 2 && source == sourceGolden && words == originalWords &&
            std::equal(packetGolden.begin(), packetGolden.end(), std::as_bytes(std::span(packets)).begin()),
            "Original VideoOut integration changed borrowed pixels, packet bytes, or callback count");
    AgcDriverUnregisterVideoOutput_nid_postfix(Handle, output);
    {
        std::lock_guard lock(cfg->mutex);
        cfg->closing = true;
        cfg->opened = false;
        cfg->vblankCond.notify_all();
    }
    bool closed = false;
    try { firstFence->Wait(); } catch (const std::exception& error) { closed = std::string(error.what()).find("closed") != std::string::npos; }
    Require(closed, "Original captured wait ignored its closed VideoOut owner lifecycle");
    cleanup.done = true;
    std::cout << "PASS: public PM4 original VideoOut reservation, immutable metadata, sync readiness, paced actual drawable pixels, completion callbacks, global queue room and captured ticket retirement\n";
}

void ReplayNativeHost(id<MTLDevice> device, AgcDriver::Metal::MetalDriver& driver) {
    using namespace AnyPS5::Host;
    using namespace std::chrono_literals;
    auto host = NativeHostWindow::CreateMainThread({"AnyPS5 native host event replay", 160, 100});
    id<MTLCommandBuffer> gpuWork = nil;
    std::unique_ptr<NativeControllerSource> controller;
    const bool previousControllerMonitoring = GCController.shouldMonitorBackgroundEvents;
    const auto controllerStartedAt = std::chrono::steady_clock::now();
    try {
        controller = NativeControllerSource::CreateMainThread();
        std::uint32_t actualSupportedControllers = 0, actualUnsupportedControllers = 0;
        for (GCController* actual in GCController.controllers) {
            if (actual.isSnapshot) continue;
            if (actual.extendedGamepad) ++actualSupportedControllers;
            else ++actualUnsupportedControllers;
        }
        const auto controllerInitial = controller->Snapshot();
        Require(controllerInitial.sourceOpen && controllerInitial.connected == (actualSupportedControllers != 0) &&
                controllerInitial.unsupportedControllerCount == actualUnsupportedControllers,
                "Coexisting controller availability differs from actual nonsnapshot framework devices");
        std::uint64_t controllerSequence = 0;
        auto controllerCapturedAt = controllerStartedAt;
        auto drainController = [&] {
            auto batch = controller->DrainEvents();
            for (const auto& event : batch.events) {
                Require(event.after.lastSequence == ++controllerSequence &&
                        event.capturedAt >= controllerCapturedAt && event.capturedAt <= std::chrono::steady_clock::now(),
                        "Coexisting controller lost its independent event prefix or host capture time");
                controllerCapturedAt = event.capturedAt;
            }
            Require(batch.after.lastSequence == controllerSequence,
                    "Coexisting controller snapshot crossed its own drained cutoff");
            if (!batch.events.empty()) {
                const auto& last = batch.events.back().after;
                Require(batch.after.controller == last.controller && batch.after.state == last.state &&
                        batch.after.sourceOpen == last.sourceOpen && batch.after.connected == last.connected &&
                        batch.after.kind == last.kind && batch.after.vendorName == last.vendorName &&
                        batch.after.capabilities.leftStickButton == last.capabilities.leftStickButton &&
                        batch.after.capabilities.rightStickButton == last.capabilities.rightStickButton &&
                        batch.after.capabilities.optionsButton == last.capabilities.optionsButton &&
                        batch.after.capabilities.touchpadButton == last.capabilities.touchpadButton &&
                        batch.after.capabilities.motion == last.capabilities.motion &&
                        batch.after.capabilities.touchCoordinates == last.capabilities.touchCoordinates &&
                        batch.after.capabilities.haptics == last.capabilities.haptics &&
                        batch.after.unsupportedControllerCount == last.unsupportedControllerCount,
                        "Coexisting controller after-state differs from its final prefix event");
            }
            return batch;
        };
        const auto controllerAdmission = drainController();
        Require(controllerAdmission.after.sourceOpen,
                "Controller admission closed the independent source");
        NSWindow* window = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"AnyPS5 native host event replay"]) window = candidate;
        Require(window != nil, "Native host did not own an actual AppKit window");
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!host->Snapshot().focused && std::chrono::steady_clock::now() < deadline)
            host->PumpMainThread(10ms);
        {
            auto notifications = std::make_shared<std::array<bool, 3>>();
            struct Notifications {
                id resize = nil, minimize = nil, restore = nil;
                ~Notifications() {
                    if (resize) [NSNotificationCenter.defaultCenter removeObserver:resize];
                    if (minimize) [NSNotificationCenter.defaultCenter removeObserver:minimize];
                    if (restore) [NSNotificationCenter.defaultCenter removeObserver:restore];
                }
            } notices;
            notices.resize = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidResizeNotification
                object:window queue:nil usingBlock:^(NSNotification*) { (*notifications)[0] = true; }];
            notices.minimize = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidMiniaturizeNotification
                object:window queue:nil usingBlock:^(NSNotification*) { (*notifications)[1] = true; }];
            notices.restore = [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidDeminiaturizeNotification
                object:window queue:nil usingBlock:^(NSNotification*) { (*notifications)[2] = true; }];
            auto pumpUntil = [&](auto completed, const std::string& operation) {
                const auto end = std::chrono::steady_clock::now() + 2s;
                while (!completed() && std::chrono::steady_clock::now() < end) host->PumpMainThread(10ms);
                Require(completed(), "Actual AppKit " + operation + " did not complete before its deadline");
            };
            auto extent = [&](NSSize points, bool zero, const std::string& operation) {
                const auto backing = [window.contentView convertRectToBacking:NSMakeRect(0, 0, points.width, points.height)];
                Require(NSEqualSizes(window.contentView.bounds.size, points) &&
                        backing.size.width == points.width * window.backingScaleFactor &&
                        backing.size.height == points.height * window.backingScaleFactor,
                        "Actual AppKit " + operation + " content/backing geometry differs");
                const auto width = zero ? 0u : static_cast<std::uint32_t>(backing.size.width);
                const auto height = zero ? 0u : static_cast<std::uint32_t>(backing.size.height);
                const auto observed = std::async(std::launch::async, [&] {
                    const auto snapshot = host->Snapshot();
                    const auto presentation = host->Presentation(SourceWidth, SourceHeight);
                    std::uint32_t callbackWidth = 0, callbackHeight = 0;
                    presentation.getDrawableSize(presentation.context, &callbackWidth, &callbackHeight);
                    return std::array<std::uint32_t, 4>{snapshot.drawableWidth, snapshot.drawableHeight,
                        callbackWidth, callbackHeight};
                }).get();
                Require(observed == std::array<std::uint32_t, 4>{width, height, width, height},
                        "Native host worker Snapshot/presentation stale after actual AppKit " + operation);
            };
            const auto originalSize = window.contentView.bounds.size;
            const auto changedSize = NSMakeSize(213, 117);
            [window setContentSize:changedSize];
            pumpUntil([&] { return (*notifications)[0]; }, "resize");
            extent(changedSize, false, "resize");
            [window miniaturize:nil];
            pumpUntil([&] { return (*notifications)[1] && window.miniaturized; }, "minimize");
            extent(changedSize, true, "minimize");
            [window deminiaturize:nil];
            pumpUntil([&] { return (*notifications)[2] && !window.miniaturized; }, "restore");
            extent(changedSize, false, "restore");
            (*notifications)[0] = false;
            [window setContentSize:originalSize];
            pumpUntil([&] { return (*notifications)[0]; }, "original extent restore");
            extent(originalSize, false, "original extent restore");
            [window makeKeyAndOrderFront:nil];
            pumpUntil([&] { return window.keyWindow && NSApp.keyWindow == window; }, "key-window restore");
            std::cout << "PASS: actual native resize, minimize and restore publish independent backing extents to worker snapshots and presentation callbacks\n";
        }
        host->PumpMainThread(0ms);
        auto initial = host->DrainEvents().after;
        Require(initial.open && initial.focused && initial.window.id != 0 && initial.window.generation != 0 &&
                initial.sources.keyboard == SourceAvailability::WindowEvents &&
                initial.sources.mouse == SourceAvailability::WindowEvents &&
                initial.sources.controller == SourceAvailability::Unsupported,
                "Native host activation or input-source availability differs");
        bool secondRejected = false;
        try { auto second = NativeHostWindow::CreateMainThread({"Unexpected second native host", 80, 60}); }
        catch (const std::logic_error&) { secondRejected = true; }
        Require(secondRejected, "Native host admitted a second live window");
        std::uint64_t sequence = initial.lastSequence;
        auto capturedAt = std::chrono::steady_clock::time_point{};
        auto drain = [&](std::size_t count) {
            auto batch = host->DrainEvents();
            Require(batch.events.size() == count && batch.after.window == initial.window,
                    "Native host drain lost or duplicated its event prefix");
            for (const auto& event : batch.events) {
                Require(event.window == initial.window && event.sequence == ++sequence &&
                        event.capturedAt >= capturedAt && event.capturedAt <= std::chrono::steady_clock::now(),
                        "Native host event identity, ordering or capture time differs");
                capturedAt = event.capturedAt;
            }
            Require(batch.after.lastSequence == sequence, "Native host snapshot crossed its drained event cutoff");
            return batch;
        };
        auto key = [&](bool pressed, bool repeat = false) {
            return [NSEvent keyEventWithType:pressed ? NSEventTypeKeyDown : NSEventTypeKeyUp
                location:NSMakePoint(8, 8) modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                windowNumber:window.windowNumber context:nil characters:@"a" charactersIgnoringModifiers:@"a"
                isARepeat:repeat keyCode:0];
        };
        auto mouse = [&](bool pressed) {
            const auto location = [window.contentView convertPoint:NSMakePoint(NSMidX(window.contentView.bounds), NSMidY(window.contentView.bounds)) toView:nil];
            return [NSEvent mouseEventWithType:pressed ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp
                location:location modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime
                windowNumber:window.windowNumber context:nil eventNumber:1 clickCount:1 pressure:pressed ? 1 : 0];
        };
        auto post = [&](NSArray<NSEvent*>* events) {
            for (NSEvent* event in events.reverseObjectEnumerator) [NSApp postEvent:event atStart:YES];
            host->PumpMainThread(0ms, events.count);
        };
        post(@[key(true), key(true, true), mouse(true), key(false)]);
        auto first = drain(3);
        const auto& down = std::get<KeyboardInputEvent>(first.events[0].payload);
        const auto& button = std::get<MouseInputEvent>(first.events[1].payload);
        const auto& up = std::get<KeyboardInputEvent>(first.events[2].payload);
        Require(down.keyCode == 4 && down.pressed && button.button == 1 && button.pressed &&
                up.keyCode == 4 && !up.pressed && !first.after.heldKeys[4] && first.after.heldMouseButtons == 1,
                "Actual AppKit key/repeat/mouse routing or first snapshot differs");
        Require(drain(0).after.heldMouseButtons == 1, "Empty drain changed the previous event cutoff state");
        const auto controllerAfterWindowKeys = drainController();
        Require(controllerAfterWindowKeys.after.sourceOpen && host->Snapshot().heldMouseButtons == 1 &&
                host->Snapshot().window == initial.window,
                "Independent controller drain changed the window input cutoff or owner");
        if (actualSupportedControllers == 0 && actualUnsupportedControllers == 0) {
            Require(controllerAfterWindowKeys.events.empty() && !controllerAfterWindowKeys.after.connected &&
                    controllerAfterWindowKeys.after.controller == ControllerIdentity{} &&
                    controllerAfterWindowKeys.after.state.buttons == 0 &&
                    controllerAfterWindowKeys.after.state.sticks == std::array<std::uint8_t, 4>{128, 128, 128, 128} &&
                    controllerAfterWindowKeys.after.state.leftTrigger == 0 && controllerAfterWindowKeys.after.state.rightTrigger == 0,
                    "Window key/mouse events invented a physical controller or pressed state");
        }
        post(@[mouse(false), mouse(true), key(true)]);
        auto otherConsumer = std::async(std::launch::async, [&] {
            try { static_cast<void>(host->DrainEvents()); }
            catch (const std::logic_error&) { return true; }
            return false;
        });
        Require(otherConsumer.get(), "Native host allowed a second event consumer");
        auto next = drain(3);
        Require(!std::get<MouseInputEvent>(next.events[0].payload).pressed &&
                std::get<MouseInputEvent>(next.events[1].payload).pressed &&
                std::get<KeyboardInputEvent>(next.events[2].payload).pressed &&
                next.after.heldKeys[4] && next.after.heldMouseButtons == 1 && !first.after.heldKeys[4],
                "Native host later prefix changed an earlier cutoff snapshot");
        NSWindow* focusTarget = nil;
        for (NSWindow* candidate in NSApp.windows)
            if ([candidate.title isEqualToString:@"AnyPS5 Metal presentation replay"]) focusTarget = candidate;
        Require(focusTarget != nil && focusTarget != window, "Native host focus replay lacks its existing presentation window");
        [focusTarget makeKeyAndOrderFront:nil];
        auto lost = drain(3);
        Require(std::get<KeyboardInputEvent>(lost.events[0].payload).resetKeys &&
                std::get<MouseInputEvent>(lost.events[1].payload).resetButtons &&
                !std::get<FocusChanged>(lost.events[2].payload).focused && !lost.after.focused &&
                std::none_of(lost.after.heldKeys.begin(), lost.after.heldKeys.end(), [](bool held) { return held; }) &&
                lost.after.heldMouseButtons == 0,
                "Actual AppKit focus loss did not reset held input before its focus event");
        [window makeKeyAndOrderFront:nil];
        auto gained = drain(3);
        Require(std::get<KeyboardInputEvent>(gained.events[0].payload).connectionChange &&
                std::get<KeyboardInputEvent>(gained.events[0].payload).connected &&
                std::get<MouseInputEvent>(gained.events[1].payload).connectionChange &&
                std::get<MouseInputEvent>(gained.events[1].payload).connected &&
                std::get<FocusChanged>(gained.events[2].payload).focused && gained.after.focused,
                "Actual AppKit focus gain did not reconnect window input in order");
        post(@[key(true), mouse(true)]);
        auto held = drain(2);
        Require(held.after.heldKeys[4] && held.after.heldMouseButtons == 1, "Native host did not retain input before close");
        const auto presentation = host->Presentation(SourceWidth, SourceHeight);
        unsigned ready = 0;
        AgcDriverPresentClear_nid_postfix(presentation, true, +[](void* context) {
            ++*static_cast<unsigned*>(context);
        }, &ready);
        Require(ready == 1, "Native host presentation did not complete its real window clear");
        auto layer = (__bridge CAMetalLayer*)presentation.metalLayer(presentation.context);
        layer.framebufferOnly = NO;
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        Require(drawable != nil && drawable.texture.device == device &&
                drawable.texture.width == gained.after.drawableWidth && drawable.texture.height == gained.after.drawableHeight,
                "Native host presentation callbacks did not provide an actual drawable with the backing extent");
        auto queue = [device newCommandQueue];
        auto bytes = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        Require(queue != nil && bytes != nil, "Native host drawable readback allocation failed");
        std::memset(bytes.contents, 0x35, bytes.length);
        gpuWork = [queue commandBuffer];
        auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
        auto encoder = [gpuWork renderCommandEncoderWithDescriptor:pass];
        Require(encoder != nil, "Native host drawable render encoding failed");
        [encoder endEncoding];
        auto blit = [gpuWork blitCommandEncoder];
        [blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(1, 1, 1) toBuffer:bytes destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
        [blit endEncoding];
        [window performClose:nil];
        auto closing = drain(5);
        Require(std::get<KeyboardInputEvent>(closing.events[0].payload).resetKeys &&
                std::get<MouseInputEvent>(closing.events[1].payload).resetButtons &&
                std::get<KeyboardInputEvent>(closing.events[2].payload).connectionChange &&
                !std::get<KeyboardInputEvent>(closing.events[2].payload).connected &&
                std::get<MouseInputEvent>(closing.events[3].payload).connectionChange &&
                !std::get<MouseInputEvent>(closing.events[3].payload).connected &&
                std::holds_alternative<CloseRequested>(closing.events[4].payload) &&
                closing.after.open && closing.after.closeRequested && !closing.after.focused && window.visible &&
                closing.after.heldMouseButtons == 0 &&
                std::none_of(closing.after.heldKeys.begin(), closing.after.heldKeys.end(), [](bool held) { return held; }) &&
                presentation.metalLayer(presentation.context) == (__bridge void*)layer,
                "Actual AppKit close destroyed presentation before CPU/GPU drain or lost its ordered input reset");
        controller->CloseMainThread();
        const auto controllerClosed = drainController();
        Require(!controllerClosed.after.sourceOpen && !controllerClosed.after.connected &&
                controllerClosed.after.state.buttons == 0 &&
                controllerClosed.after.state.sticks == std::array<std::uint8_t, 4>{128, 128, 128, 128} &&
                controllerClosed.after.state.leftTrigger == 0 && controllerClosed.after.state.rightTrigger == 0 &&
                std::count_if(controllerClosed.events.begin(), controllerClosed.events.end(), [](const auto& event) {
                    return event.kind == ControllerEventKind::SourceClosed;
                }) == 1 && !controllerClosed.events.empty() &&
                controllerClosed.events.back().kind == ControllerEventKind::SourceClosed,
                "Coexisting controller did not deliver its final neutral close prefix before GPU/window retirement");
        Require(GCController.shouldMonitorBackgroundEvents == previousControllerMonitoring &&
                host->Snapshot().open && host->Snapshot().closeRequested && window.visible &&
                presentation.metalLayer(presentation.context) == (__bridge void*)layer,
                "Controller close changed process policy or destroyed the window before GPU drain");
        const auto controllerAfterClose = drainController();
        Require(controllerAfterClose.events.empty() && !controllerAfterClose.after.sourceOpen &&
                controllerAfterClose.after.lastSequence == controllerClosed.after.lastSequence,
                "Closed coexisting controller repeated its terminal prefix");
        [gpuWork commit];
        [gpuWork waitUntilCompleted];
        Require(gpuWork.status == MTLCommandBufferStatusCompleted, "Deferred-close native drawable did not complete on the GPU");
        const Pixel green{0, 255, 0, 255};
        Require(std::memcmp(bytes.contents, green.data(), green.size()) == 0 &&
                std::all_of(static_cast<const std::byte*>(bytes.contents) + 4,
                    static_cast<const std::byte*>(bytes.contents) + bytes.length, [](auto byte) { return byte == std::byte{0x35}; }),
                "Deferred-close actual drawable pixels or readback guards differ");
        drawable = nil;
        driver.WaitIdle();
        host->CloseAfterGpuDrainMainThread();
        auto closed = drain(1);
        Require(std::holds_alternative<WindowClosed>(closed.events[0].payload) && !closed.after.open &&
                closed.after.sources.keyboard == SourceAvailability::Unavailable &&
                closed.after.sources.mouse == SourceAvailability::Unavailable &&
                closed.after.drawableWidth == 0 && closed.after.drawableHeight == 0 && !window.visible,
                "Ordered native host close did not retire its window, source availability and drawable extent");
        host = NativeHostWindow::CreateMainThread({"AnyPS5 replacement native host", 80, 60});
        Require(host->Snapshot().window != initial.window, "Native host reused a closed window identity");
        host->RequestCloseMainThread();
        host->CloseAfterGpuDrainMainThread();
    } catch (...) {
        if (controller) { try { controller->CloseMainThread(); } catch (...) {} }
        if (gpuWork.status >= MTLCommandBufferStatusCommitted) [gpuWork waitUntilCompleted];
        try { host->RequestCloseMainThread(); } catch (...) {}
        host->CloseAfterGpuDrainMainThread();
        throw;
    }
    std::cout << "PASS: real AppKit pump, ordered single-consumer event cutoffs, focus resets and deferred native drawable close\n";
    std::cout << "PASS: production-linked real window/controller coexistence, separate event cutoffs and neutral controller close before GPU/window retirement; PHYSICAL_POSITIVE_UNVERIFIED\n";
}

void Run(id<MTLDevice> device, id<MTLLibrary> library) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    auto window = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 40, 160, 100)
                                            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.title = @"AnyPS5 Metal presentation replay";
    auto layer = [ReplayMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.frame = window.contentView.bounds;
    window.contentView.wantsLayer = YES;
    window.contentView.layer = layer;
    [window orderFront:nil];
    [CATransaction flush];
    WindowContext context{layer};
    AgcDriver::PresentationWindow presentation{&context, {}, nullptr, Size, SourceWidth, SourceHeight, {}, Layer};
    std::array<std::vector<std::byte>, 4> linear;
    for (auto& bytes : linear) bytes.resize(10u * SourceHeight * 4u);
    std::vector<std::byte> tiled(65536);
    FillTiled(tiled);
    std::vector<std::byte> keys(256, std::byte{0xff});
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges;
    for (std::size_t i = 0; i < linear.size(); ++i) ranges.push_back({0x10000u + i * 0x10000u, linear[i], false});
    ranges.push_back({0x50000, tiled, false});
    ranges.push_back({0x60000, keys, false});
    auto& driver = AgcDriver::Metal::MetalDriver::Get();
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges);
    const auto queue = [device newCommandQueue];
    Require(queue != nil, "Independent drawable readback queue creation failed");
    const std::array<std::uint64_t, 4> formats{BGRA, RGBA, BGRA | TenBit, RGBA | TenBit};
    for (std::size_t i = 0; i < formats.size(); ++i) {
        FillLinear(linear[i], formats[i]);
        BufferAttributeGroup group{};
        group.attribute.pixel_format = formats[i];
        group.attribute.width = SourceWidth;
        group.attribute.height = SourceHeight;
        group.attribute.tiling_mode = 1;
        group.attribute.pitch_in_pixel = 10;
        group.attribute.option = VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY;
        group.occupied = true;
        const VideoOutBuffer registered{0, 0x10000u + i * 0x10000u, 0};
        const auto buffer = DescribeVideoOutBuffer(registered, group);
        Present(presentation, buffer, context, queue, Golden(8, 4));
    }
    const AgcDriver::DisplayBuffer tiledBuffer{0x50000, BGRA, SourceWidth, SourceHeight};
    Present(presentation, tiledBuffer, context, queue, Golden(8, 4));
    context.height = 8;
    Present(presentation, tiledBuffer, context, queue, Golden(8, 8));
    auto compressed = tiledBuffer;
    compressed.dccAddress = 0x60000;
    Present(presentation, compressed, context, queue, Golden(8, 8));
    compressed.address = 0x70000;
    for (const auto key : {0x00u, 0x40u, 0x80u, 0xc0u, 0x20u}) {
        std::fill(keys.begin(), keys.end(), static_cast<std::byte>(key));
        compressed.dccClearColor = key == 0x20 ? 0xff2f7badu : 0;
        const Pixel pixel = key == 0x00 ? Pixel{0, 0, 0, 0} : key == 0x40 ? Pixel{0, 0, 0, 255} :
            key == 0x80 ? Pixel{255, 255, 255, 0} : key == 0xc0 ? Pixel{255, 255, 255, 255} : Pixel{173, 123, 47, 255};
        Present(presentation, compressed, context, queue, Golden(8, 8, true, pixel));
    }
    keys[0] = std::byte{0xff};
    context.layer.recordedDrawable = nil;
    Completion mixed{&context, queue, {}};
    bool rejected = false;
    try { AgcDriverPresentBuffer_nid_postfix(presentation, compressed, Ready, &mixed); }
    catch (const std::runtime_error& error) { rejected = std::string(error.what()).find("mixed") != std::string::npos; }
    Require(rejected && mixed.calls == 0 && context.layer.recordedDrawable == nil,
            "Mixed DCC presentation did not reject before drawing or completion");
    for (const auto opaque : {false, true}) {
        context.layer.recordedDrawable = nil;
        Completion completion{&context, queue, std::vector<Pixel>(64, Pixel{0, 0, 0, static_cast<std::uint8_t>(opaque ? 255 : 0)})};
        AgcDriverPresentClear_nid_postfix(presentation, opaque, Ready, &completion);
        Require(completion.calls == 1 && context.layer.opaque == opaque, "Original clear entry lost opacity or completion");
    }
    context.width = 0;
    context.height = 0;
    context.layer.recordedDrawable = nil;
    const auto acquisitions = context.layer.acquisitionCount;
    const AgcDriver::DisplayBuffer unreadable{0x80000, BGRA, SourceWidth, SourceHeight, 1, 10};
    Completion minimized{&context, queue, {}, 0, false};
    AgcDriverPresentBuffer_nid_postfix(presentation, unreadable, Ready, &minimized);
    Require(minimized.calls == 1 && context.layer.acquisitionCount == acquisitions,
            "Minimized original presentation read an unborrowed display or acquired a drawable");
    ReplayOriginalFlipState(driver, context, presentation, queue, linear[0]);
    ReplayNativeHost(device, driver);
    AgcDriverReleaseWindow_nid_postfix(&context);
    driver.Shutdown();
    [window orderOut:nil];
    [window close];
    std::cout << "PASS: original Metal presentation entries, actual drawable completion pixels, pitched formats, tiled offsets, DCC clear/uncompressed/mixed, aspect bars, opacity and minimized no-read\n";
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 2, "Presentation replay requires the compiled utility metallib path");
            const auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "Presentation replay requires a native Metal device");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, error.localizedDescription.UTF8String ?: "Utility library load failed");
            setenv("APS5_SYNC_FLIP", "1", 1);
            Run(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
