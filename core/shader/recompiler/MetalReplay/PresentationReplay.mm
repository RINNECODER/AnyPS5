#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceVideoOut/include/BufferMetadata.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
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
