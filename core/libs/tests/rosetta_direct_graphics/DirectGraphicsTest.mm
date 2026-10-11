// Rosetta direct path, graphics (fork-only, macOS x86_64, run under arch -x86_64).
//
// Drives the x86_64 libkernel, libSceAgcDriver and libSceVideoOut libraries the way a natively
// running title does, with guest pointers that are host pointers:
//   1. direct memory is allocated and mapped through libkernel, and the Metal driver sees it as
//      an identity range (no TCG translation);
//   2. a DCB holding one DMA_DATA fill (a clear of the display buffer) is submitted through
//      sceAgcDriverSubmitMultiDcbs and executed by the Metal driver;
//   3. the buffer is registered and flipped through libSceVideoOut, which presents it with the
//      Metal presenter into the native window (AppKit window + CAMetalLayer);
//   4. the ANYPS5_FRAME_CAPTURE_DIR capture of that frame is a PNG of the clear colour.
// The main thread services the main dispatch queue, as the direct-path launcher must, while the
// "guest" runs on another thread.
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#include "prx/libSceAgcDriver/Execution/Metal/DirectDriver.hpp"
#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C" {
std::size_t APS5_VABI sceKernelGetDirectMemorySize();
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceAgcDriverSubmitMultiDcbs(std::uint32_t* const*, const std::uint32_t*, std::uint32_t);
void AgcDriverWaitIdle_nid_postfix();
void AgcDriverCheckGuestMemory_nid_postfix(const void*, std::size_t, std::size_t, bool);
void APS5_VABI sceVideoOutSetBufferAttribute2(VideoOutBufferAttribute2*, std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t);
int APS5_VABI sceVideoOutOpen(int, int, int, const void*);
int APS5_VABI sceVideoOutRegisterBuffers2(int, int, int, const VideoOutBuffers*, int, const VideoOutBufferAttribute2*, int, void*);
int APS5_VABI sceVideoOutSubmitFlip(int, int, int, std::int64_t);
}

namespace {

constexpr std::uint32_t Width = 1280;
constexpr std::uint32_t Height = 720;
constexpr std::size_t DisplayBytes = std::size_t{Width} * Height * 4;
constexpr std::size_t ControlBytes = 64 * 1024;
constexpr std::size_t MappedBytes = 4 * 1024 * 1024 + ControlBytes;
constexpr int ProtCpuGpuReadWrite = 0x33;
constexpr std::uint64_t PixelFormatR8G8B8A8Srgb = 0x8000000022000000ull;
constexpr std::uint32_t TilingLinear = 1;
// Little-endian R, G, B, A bytes of the clear: an orange no capture of an empty frame can match.
constexpr std::uint32_t ClearColor = 0xff2080e0u;

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
    std::fprintf(stderr, "ok: %s\n", what.c_str());
}

struct Control {
    std::uint32_t dcb[16];
    std::uint32_t* dcbAddress;
    std::uint32_t dcbWords;
    VideoOutBufferAttribute2 attribute;
    VideoOutBuffers buffers;
};

void Guest(const std::filesystem::path& captureDirectory) {
    std::int64_t physical = -1;
    Require(sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(sceKernelGetDirectMemorySize()), MappedBytes, 64 * 1024, 0, &physical) == 0, "allocate direct memory");
    void* mapped = nullptr;
    Require(sceKernelMapDirectMemory(&mapped, MappedBytes, ProtCpuGpuReadWrite, 0, physical, 64 * 1024) == 0 && mapped != nullptr, "map direct memory");
    auto* display = static_cast<std::byte*>(mapped);
    auto* control = reinterpret_cast<Control*>(display + MappedBytes - ControlBytes);
    std::memset(display, 0, DisplayBytes);

    // Guest pointers are host pointers: the mapping is borrowed as one identity range.
    const auto address = reinterpret_cast<std::uint64_t>(mapped);
    bool borrowed = false;
    for (const auto& range : *AgcDriver::Metal::DirectGuestRanges()) {
        borrowed |= range.guestAddress <= address && address + MappedBytes <= range.guestAddress + range.host.size() &&
            reinterpret_cast<std::uint64_t>(range.host.data()) == range.guestAddress && range.writable;
    }
    Require(borrowed, "mapped direct memory is a writable identity range of the Metal driver");

    AgcDriverCheckGuestMemory_nid_postfix(control, sizeof(*control), alignof(Control), true);
    Require(true, "a guest thread outside the driver validates guest memory against the identity ranges");
    // Titles pass structures on their (host) thread stacks, outside every guest allocation.
    std::uint64_t onStack[4]{};
    AgcDriverCheckGuestMemory_nid_postfix(onStack, sizeof(onStack), alignof(std::uint64_t), true);
    Require(true, "a structure on the thread stack passes the guest memory check");

    // A borrowed range longer than the device's buffer limit is split at multiples of the limit.
    const auto identity = [](std::uint64_t address, std::size_t bytes) {
        return AgcDriver::NativeGuestMemory::BorrowedRange{address, {reinterpret_cast<std::byte*>(address), bytes}, true};
    };
    const auto split = AgcDriver::Metal::SplitBorrowedRanges({identity(0x18000, 0x28000), identity(0x60000, 0x8000)}, 0x10000);
    const std::vector<std::pair<std::uint64_t, std::size_t>> expected{{0x18000, 0x8000}, {0x20000, 0x10000}, {0x30000, 0x10000}, {0x60000, 0x8000}};
    bool splitOk = split.size() == expected.size();
    for (std::size_t i = 0; splitOk && i < split.size(); ++i)
        splitOk = split[i].guestAddress == expected[i].first && split[i].host.size() == expected[i].second &&
            reinterpret_cast<std::uint64_t>(split[i].host.data()) == expected[i].first && split[i].writable;
    Require(splitOk, "borrowed ranges split at multiples of the limit");

    // DMA_DATA: immediate source (src_sel 2), memory destination (dst_sel 0), byte count.
    const std::uint32_t dma[] = {0xc0055000u, 2u << 29u, ClearColor, 0, static_cast<std::uint32_t>(address),
        static_cast<std::uint32_t>(address >> 32u), static_cast<std::uint32_t>(DisplayBytes)};
    std::memcpy(control->dcb, dma, sizeof(dma));
    control->dcbAddress = control->dcb;
    control->dcbWords = sizeof(dma) / sizeof(dma[0]);
    Require(sceAgcDriverSubmitMultiDcbs(&control->dcbAddress, &control->dcbWords, 1) == 0, "submit the clear");
    AgcDriverWaitIdle_nid_postfix();
    std::uint32_t first = 0;
    std::uint32_t last = 0;
    std::memcpy(&first, display, 4);
    std::memcpy(&last, display + DisplayBytes - 4, 4);
    Require(first == ClearColor && last == ClearColor, "the Metal driver cleared the whole display buffer");

    const int handle = sceVideoOutOpen(255, 0, 0, nullptr);
    Require(handle > 0, "open the video output");
    // The window is created on the main queue; flip into it rather than into the offscreen layer.
    const auto windowDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    __block bool windowOpen = false;
    while (!windowOpen && std::chrono::steady_clock::now() < windowDeadline) {
        dispatch_sync(dispatch_get_main_queue(), ^{ windowOpen = NSApp != nil && NSApp.windows.count > 0; });
        if (!windowOpen) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Require(windowOpen, "the native window opens on the main thread");
    sceVideoOutSetBufferAttribute2(&control->attribute, PixelFormatR8G8B8A8Srgb, TilingLinear, Width, Height, 0, 0, 0);
    control->buffers = {display, nullptr, {nullptr, nullptr}};
    Require(sceVideoOutRegisterBuffers2(handle, 0, 0, &control->buffers, 1, &control->attribute, 0, nullptr) == 0, "register the display buffer");
    Require(sceVideoOutSubmitFlip(handle, 0, 1, 0) == 0, "flip the cleared buffer");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::filesystem::path png;
    while (png.empty() && std::chrono::steady_clock::now() < deadline) {
        for (const auto& entry : std::filesystem::directory_iterator(captureDirectory))
            if (entry.path().extension() == ".png") png = entry.path();
        if (png.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Require(!png.empty(), "a frame capture PNG appears for the flip");

    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:png.c_str()]];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    Require(source != nullptr, "open the capture");
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    Require(image != nullptr, "decode the capture");
    const auto width = CGImageGetWidth(image);
    const auto height = CGImageGetHeight(image);
    std::vector<std::uint8_t> rgba(width * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(rgba.data(), width, height, 8, width * 4, space, kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    const auto* center = rgba.data() + ((height / 2) * width + width / 2) * 4;
    const auto near = [](int value, int expected) { return std::abs(value - expected) <= 12; };
    std::printf("capture %s: %zux%zu, center rgb %d %d %d\n", png.filename().c_str(), width, height, center[0], center[1], center[2]);
    Require(near(center[0], 0xe0) && near(center[1], 0x80) && near(center[2], 0x20), "the captured frame shows the clear colour");
}

}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <AnyPS5Utilities.metallib>\n", argv[0]);
        return 2;
    }
    char pattern[] = "/tmp/anyps5-direct-graphics-XXXXXX";
    const char* directory = mkdtemp(pattern);
    if (directory == nullptr) return 2;
    setenv("ANYPS5_METAL_UTILITY_LIBRARY", argv[1], 1);
    setenv("ANYPS5_FRAME_CAPTURE_DIR", directory, 1);
    std::atomic<int> result{-1};
    std::thread guest([&] {
        try {
            Guest(directory);
            result = 0;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what());
            result = 1;
        }
    });
    while (result.load() < 0) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    guest.join();
    std::filesystem::remove_all(directory);
    if (result == 0) std::printf("rosetta direct graphics: clear + flip presented and captured\n");
    std::fflush(stdout);
    std::fflush(stderr);
    // The video output and Metal driver threads keep running; skip their teardown.
    _exit(result.load());
}
