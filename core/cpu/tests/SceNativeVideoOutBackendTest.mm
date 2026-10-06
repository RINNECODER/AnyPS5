#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cpu/Cpu.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class TAction> void rejects(TAction&& action, const char* expected, const char* message) {
    try { action(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(message);
}

struct Backing {
    std::unique_ptr<std::byte, decltype(&std::free)> bytes{nullptr, &std::free};
    Backing() {
        void* memory = nullptr;
        require(posix_memalign(&memory, 65536, 65536) == 0, "Cannot allocate native display backing");
        bytes.reset(static_cast<std::byte*>(memory));
    }
    std::span<std::byte> Bytes() { return {bytes.get(), 65536}; }
};

struct WindowContext { CAMetalLayer* layer; };

void drawableSize(void* context, std::uint32_t* width, std::uint32_t* height) {
    const auto size = static_cast<WindowContext*>(context)->layer.drawableSize;
    *width = static_cast<std::uint32_t>(size.width);
    *height = static_cast<std::uint32_t>(size.height);
}

void* metalLayer(void* context) {
    return (__bridge void*)static_cast<WindowContext*>(context)->layer;
}

std::uint64_t processTime(void*) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct CompletionEvents { unsigned count = 0; };

void flipEvent(void* context, VideoOutConfig&, std::int64_t) {
    ++static_cast<CompletionEvents*>(context)->count;
}

struct DriverSession {
    ~DriverSession() {
        try { AgcDriver::Metal::MetalDriver::Get().Shutdown(); }
        catch (...) {}
    }
};

void run(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::uint64_t address = 0x400000000ULL;
    constexpr auto rw = Cpu::Permission::Read | Cpu::Permission::Write;
    Backing first, second;
    Cpu::Machine machine;
    machine.MapBorrowed(address, first.Bytes(), rw);
    machine.MapBorrowed(address + 65536, second.Bytes(), Cpu::Permission::Write);
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 2> ranges{{
        {address, first.Bytes(), false}, {address + 65536, second.Bytes(), false}}};
    AgcDriver::Metal::MetalDriver::Get().Configure((__bridge void*)device, (__bridge void*)library, ranges);
    DriverSession driver;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    auto window = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 120, 64, 64)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    auto layer = [CAMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.drawableSize = CGSizeMake(64, 64);
    window.contentView.wantsLayer = YES;
    window.contentView.layer = layer;
    WindowContext context{layer};
    const AgcDriver::PresentationWindow presentation{&context, {}, nullptr, drawableSize, 64, 64, {}, metalLayer};
    CompletionEvents events;
    const VideoOutCompletionCallbacks completion{&events, processTime, processTime, flipEvent};
    Cpu::SceVideoOutBackend retiredCallbacks;
    {
        Cpu::SceNativeVideoOutBackend backend(machine, presentation, completion);
        const auto callbacks = backend.GetCallbacks();
        const auto handle = callbacks.Open(255, 0, 0, std::nullopt);
        require(handle > 0, "Native backend failed to open actual output");
        auto original = backend.GetConfig(handle);
        const auto generation = original->generation;
        Cpu::SceVideoOutAttribute attribute;
        attribute.Width = 64;
        attribute.Height = 64;
        attribute.PixelFormat = 0x8000000000000000ULL;
        const std::array<Cpu::SceVideoOutBuffer, 2> invalid{{
            {address, 0, {}}, {0x500000000ULL, 0, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 0, 0, invalid, attribute, 0); },
            "Guest access denied at 0x500000000 for permissions 1",
            "Native backend accepted an unmapped second display buffer");
        const std::array<Cpu::SceVideoOutBuffer, 1> valid{{{address, 0, {}}}};
        require(callbacks.RegisterBuffers(handle, 0, 0, valid, attribute, 0) == 0,
            "Failed display batch partially published its group or first slot");
        {
            std::lock_guard lock(original->mutex);
            require(original->buffers[0].dataAddress == address && !original->buffers[1].Occupied(),
                "Native backend cast guest display VA to host pointer or retained invalid second slot");
        }
        const std::array<Cpu::SceVideoOutBuffer, 2> unreadable{{
            {address, 0, {}}, {address + 65536, 0, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 1, 3, unreadable, attribute, 0); },
            "Guest access denied at 0x400010000 for permissions 1",
            "Native backend accepted a write-only second display span");
        require(callbacks.RegisterBuffers(handle, 1, 3, valid, attribute, 0) == 0,
            "Unreadable display batch partially published native state");
        auto compressed = attribute;
        compressed.DccControl = 0x208;
        const std::array<Cpu::SceVideoOutBuffer, 1> shortMetadata{{{address, address + 65536 - 128, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 2, 6, shortMetadata, compressed, 1); },
            "Guest access denied at 0x400010000 for permissions 1",
            "Native backend checked only the first half of DCC metadata");
        const std::array<Cpu::SceVideoOutBuffer, 1> completeMetadata{{{address, address + 64000, {}}}};
        require(callbacks.RegisterBuffers(handle, 2, 6, completeMetadata, compressed, 1) == 0,
            "Rejected DCC metadata partially published native group state");
        require(callbacks.UnregisterBuffers(handle, 0) == 0 && callbacks.UnregisterBuffers(handle, 1) == 0 &&
            callbacks.UnregisterBuffers(handle, 2) == 0,
            "Native backend did not retire successfully registered groups");
        require(callbacks.Close(handle) == 0, "Native backend failed to unregister its GPU output");
        const auto reopened = callbacks.Open(255, 0, 0, std::nullopt);
        require(reopened == handle, "Native output reopen changed bus handle");
        auto replacement = backend.GetConfig(reopened);
        {
            std::lock_guard lock(original->mutex);
            require(!original->opened && original->closing, "Native close left old port live");
        }
        {
            std::lock_guard lock(replacement->mutex);
            require(replacement != original && replacement->generation > generation &&
                !replacement->groups[0].occupied && !replacement->buffers[0].Occupied(),
                "Native reopen reused the closed port's generation or display slots");
        }
        require(callbacks.Close(reopened) == 0, "Native reopened output failed to close");
        retiredCallbacks = callbacks;
        backend.Shutdown();
    }
    rejects([&] { retiredCallbacks.Open(255, 0, 0, std::nullopt); }, "expired",
        "Retired native callbacks opened an output after their session ended");
    require(events.count == 0, "Registration lifecycle fabricated a completed frame event");
    AgcDriver::Metal::MetalDriver::Get().Shutdown();
    [window close];
    std::cout << "PASS native VideoOut numeric guest spans, atomic batch publication, fresh GPU output generation and callback retirement\n";
}

}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            require(argc == 2, "Native VideoOut keeper requires utility metallib path");
            const auto device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Native VideoOut keeper requires actual Metal device");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            if (library == nil) throw std::runtime_error(std::string("Utility metallib load failed: ") + error.localizedDescription.UTF8String);
            run(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << error.what() << '\n';
            return 1;
        }
    }
}
