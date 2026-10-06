#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
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
        const AgcDriver::DisplayBuffer buffer{0x10000u + i * 0x10000u, formats[i], SourceWidth, SourceHeight, 1, 10};
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
            Run(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
