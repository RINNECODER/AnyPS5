#include "FrameCapture.hpp"
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace AgcDriver::Metal {

struct FrameCapture::State {
    std::string directory;
    std::chrono::milliseconds interval{30000};
    std::atomic<std::uint64_t> presented{0};
    std::atomic<bool> inFlight{false};
    std::mutex mutex;
    std::chrono::steady_clock::time_point lastCapture;
    bool captured = false;
};

namespace {

std::int64_t epochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

void emit(const std::string& line) {
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

// 8-bit RGBA from a BGRA8 or BGR10A2 row-padded readback. Alpha is forced opaque.
std::vector<std::uint8_t> toRgba(const std::uint8_t* bytes, std::size_t row, std::uint32_t width, std::uint32_t height,
                                 MTLPixelFormat format) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* in = bytes + y * row;
        auto* out = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (std::uint32_t x = 0; x < width; ++x, in += 4, out += 4) {
            if (format == MTLPixelFormatBGR10A2Unorm) {
                std::uint32_t word;
                std::memcpy(&word, in, 4);
                out[0] = static_cast<std::uint8_t>(((word >> 20) & 0x3ff) >> 2);
                out[1] = static_cast<std::uint8_t>(((word >> 10) & 0x3ff) >> 2);
                out[2] = static_cast<std::uint8_t>((word & 0x3ff) >> 2);
            } else {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
            }
            out[3] = 255;
        }
    }
    return rgba;
}

bool writePng(const std::string& path, const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CFDataRef data = CFDataCreate(nullptr, rgba.data(), static_cast<CFIndex>(rgba.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGImageRef image = CGImageCreate(width, height, 8, 32, static_cast<std::size_t>(width) * 4, space,
                                     kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    bool ok = false;
    if (image != nullptr) {
        const std::string temporary = path + ".part";
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(temporary.c_str()),
                                                               static_cast<CFIndex>(temporary.size()), false);
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
        if (destination != nullptr) {
            CGImageDestinationAddImage(destination, image, nullptr);
            ok = CGImageDestinationFinalize(destination) && std::rename(temporary.c_str(), path.c_str()) == 0;
            CFRelease(destination);
        }
        CFRelease(url);
        CGImageRelease(image);
    }
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
    return ok;
}

}

FrameCapture& FrameCapture::Shared() {
    static FrameCapture* capture = new FrameCapture();  // never destroyed: the reporter thread outlives statics
    return *capture;
}

FrameCapture::FrameCapture() {
    const char* directory = std::getenv("ANYPS5_FRAME_CAPTURE_DIR");
    if (directory == nullptr || *directory == 0) return;
    enabled = true;
    state = new State();
    state->directory = directory;
    if (const char* interval = std::getenv("ANYPS5_FRAME_CAPTURE_INTERVAL_MS")) {
        char* end = nullptr;
        const auto value = std::strtoll(interval, &end, 10);
        if (end != interval && value > 0) state->interval = std::chrono::milliseconds(value);
    }
    @autoreleasepool {
        [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:directory]
                                  withIntermediateDirectories:YES attributes:nil error:nil];
    }
    auto* shared = state;
    std::thread([shared] {
        auto last = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(shared->interval);
            const auto now = std::chrono::steady_clock::now();
            const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            last = now;
            emit("{\"schema_version\":1,\"event\":\"frames_presented\",\"count\":" + std::to_string(shared->presented.load()) +
                 ",\"since_ms\":" + std::to_string(since) + "}");
        }
    }).detach();
}

std::optional<FrameCapture::Ticket> FrameCapture::Begin(std::uint32_t guestWidth, std::uint32_t guestHeight) {
    if (!enabled) return std::nullopt;
    const auto index = state->presented.fetch_add(1);
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(state->mutex);
    if (state->captured && now - state->lastCapture < state->interval) return std::nullopt;
    bool expected = false;
    if (!state->inFlight.compare_exchange_strong(expected, true)) return std::nullopt;
    state->captured = true;
    state->lastCapture = now;
    return Ticket{index, guestWidth, guestHeight};
}

void FrameCapture::Abandon() {
    if (enabled) state->inFlight.store(false);
}

void FrameCapture::Encode(id<MTLCommandBuffer> commands, id<MTLTexture> texture, const Ticket& ticket) {
    const NSUInteger width = texture.width, height = texture.height;
    const NSUInteger row = (width * 4 + 255) & ~NSUInteger{255};
    id<MTLBuffer> staging = [texture.device newBufferWithLength:row * height options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = staging != nil ? [commands blitCommandEncoder] : nil;
    if (blit == nil) {
        Abandon();
        return;
    }
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1) toBuffer:staging destinationOffset:0
          destinationBytesPerRow:row destinationBytesPerImage:row * height];
    [blit endEncoding];
    const auto format = texture.pixelFormat;
    const Ticket flip = ticket;  // the handler outlives the caller's reference
    auto* shared = state;
    [commands addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        if (finished.status != MTLCommandBufferStatusCompleted) {
            shared->inFlight.store(false);
            return;
        }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            @autoreleasepool {
                const auto rgba = toRgba(static_cast<const std::uint8_t*>(staging.contents), row,
                                         static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), format);
                // Coarse 64x36 grid so blank or solid frames are obvious without a full scan.
                std::set<std::uint32_t> colors;
                unsigned nonblack = 0;
                constexpr unsigned columns = 64, rows = 36;
                for (unsigned gy = 0; gy < rows; ++gy) {
                    for (unsigned gx = 0; gx < columns; ++gx) {
                        const auto x = std::min<std::uint64_t>(width - 1, (gx * 2 + 1) * width / (columns * 2));
                        const auto y = std::min<std::uint64_t>(height - 1, (gy * 2 + 1) * height / (rows * 2));
                        const auto* p = rgba.data() + (y * width + x) * 4;
                        colors.insert(std::uint32_t{p[0]} << 16 | std::uint32_t{p[1]} << 8 | p[2]);
                        if (std::max({p[0], p[1], p[2]}) > 8) ++nonblack;
                    }
                }
                const auto ms = epochMs();
                const auto path = shared->directory + "/frame-" + std::to_string(flip.flipIndex) + "-" + std::to_string(ms) + ".png";
                if (writePng(path, rgba, static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height))) {
                    char ratio[32];
                    std::snprintf(ratio, sizeof ratio, "%.4f", static_cast<double>(nonblack) / (columns * rows));
                    emit("{\"schema_version\":1,\"event\":\"frame_captured\",\"path\":\"" + escape(path) +
                         "\",\"flip_index\":" + std::to_string(flip.flipIndex) + ",\"width\":" + std::to_string(width) +
                         ",\"height\":" + std::to_string(height) + ",\"nonblack_ratio\":" + ratio +
                         ",\"distinct_colors\":" + std::to_string(colors.size()) +
                         ",\"guest_width\":" + std::to_string(flip.guestWidth) +
                         ",\"guest_height\":" + std::to_string(flip.guestHeight) + ",\"ms\":" + std::to_string(ms) + "}");
                } else {
                    emit("{\"schema_version\":1,\"event\":\"frame_capture_failed\",\"flip_index\":" + std::to_string(flip.flipIndex) + "}");
                }
                shared->inFlight.store(false);
            }
        });
    }];
}

}
