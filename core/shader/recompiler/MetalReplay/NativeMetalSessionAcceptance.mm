#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using AnyPS5::Host::NativeMetalSession;
using AnyPS5::Host::NativeMetalSessionConfiguration;
using Range = AgcDriver::NativeGuestMemory::BorrowedRange;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class Exception, class Operation>
void Reject(Operation&& operation, const std::string& message) {
    try { operation(); }
    catch (const Exception& error) {
        Require(std::string(error.what()).find(message) != std::string::npos,
                "Factory or lifecycle failed at a different guard: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected checked failure: " + message);
}

std::size_t VisibleWindows() {
    std::size_t count = 0;
    for (NSWindow* window in NSApp.windows) if (window.visible) ++count;
    return count;
}

NSWindow* NamedWindow(const std::string& title) {
    auto nativeTitle = [NSString stringWithUTF8String:title.c_str()];
    for (NSWindow* window in NSApp.windows)
        if (window.visible && [window.title isEqualToString:nativeTitle]) return window;
    return nil;
}

struct Retirement {
    bool released = false;
    bool windowClosedBeforeRelease = false;
    bool driverStoppedBeforeRelease = false;
};

struct Backing {
    std::array<std::byte, 8 * 4 * 4> pixels{};
    Retirement* retirement = nullptr;
    NSWindow* __weak window = nil;
    ~Backing() {
        if (!retirement) return;
        retirement->released = true;
        retirement->windowClosedBeforeRelease = !window.visible;
        try { AgcDriver::Metal::MetalDriver::Get().WaitIdle(); }
        catch (const std::runtime_error& error) {
            retirement->driverStoppedBeforeRelease = std::string(error.what()).find("shut down") != std::string::npos;
        }
    }
};

NativeMetalSessionConfiguration Configuration(const std::filesystem::path& library, const std::string& title) {
    NativeMetalSessionConfiguration configuration;
    configuration.window = {title, 96, 64};
    configuration.utilityMetallib = library;
    configuration.initialGeneration = 17;
    return configuration;
}

void FactoryGuards(const std::filesystem::path& library, const std::filesystem::path& invalidLibrary) {
    const auto visible = VisibleWindows();
    id previousDelegate = NSApp.delegate;
    auto config = Configuration(library, "AnyPS5 session factory guard");
    config.utilityMetallib.clear();
    Reject<std::invalid_argument>([&] { NativeMetalSession::CreateMainThread(config); }, "explicit utility metallib path");
    config.utilityMetallib = library.parent_path();
    Reject<std::invalid_argument>([&] { NativeMetalSession::CreateMainThread(config); }, "not a regular file");
    config.utilityMetallib = invalidLibrary;
    Reject<std::runtime_error>([&] { NativeMetalSession::CreateMainThread(config); }, "utility metallib load failed");
    config.utilityMetallib = library;
    auto owner = std::make_shared<Backing>();
    std::array<Range, 2> ranges{{{0x10000, owner->pixels, false, 31}, {0x10004, owner->pixels, false, 32}}};
    config.initialRanges = std::span(ranges).first(1);
    Reject<std::invalid_argument>([&] { NativeMetalSession::CreateMainThread(config); }, "require a backing owner");
    config.initialRangeOwner = owner;
    config.initialRanges = ranges;
    Reject<std::invalid_argument>([&] { NativeMetalSession::CreateMainThread(config); }, "borrows overlap");
    config.initialRanges = {};
    config.initialRangeOwner.reset();
    config.window.contentWidth = 0;
    Reject<std::invalid_argument>([&] { NativeMetalSession::CreateMainThread(config); }, "positive content extent");
    config.window.contentWidth = 96;
    auto offMain = std::async(std::launch::async, [&] {
        @autoreleasepool {
            Reject<std::logic_error>([&] { NativeMetalSession::CreateMainThread(config); }, "AppKit main thread");
        }
    });
    offMain.get();
    Require(VisibleWindows() == visible && NSApp.delegate == previousDelegate,
            "Checked factory failure changed visible windows or application delegate");
    std::cout << "PASS: explicit path, real invalid-library load, initial owner/range, host extent and main-thread factory guards\n";
}

void Run(const std::filesystem::path& library, const std::string& mode) {
    const auto visible = VisibleWindows();
    id previousDelegate = NSApp.delegate;
    Retirement retirement;
    auto owner = std::make_shared<Backing>();
    owner->retirement = &retirement;
    for (std::size_t pixel = 0; pixel < 32; ++pixel) {
        owner->pixels[pixel * 4] = std::byte{23};
        owner->pixels[pixel * 4 + 1] = std::byte{67};
        owner->pixels[pixel * 4 + 2] = std::byte{149};
        owner->pixels[pixel * 4 + 3] = std::byte{255};
    }
    std::weak_ptr<Backing> retained = owner;
    std::vector<Range> ranges{{0x10000, owner->pixels, false, 31}};
    const auto title = "AnyPS5 native session acceptance " + mode;
    auto config = Configuration(library, title);
    config.initialRanges = ranges;
    config.initialRangeOwner = owner;
    auto session = NativeMetalSession::CreateMainThread(config);
    NSWindow* nativeWindow = NamedWindow(title);
    Require(nativeWindow != nil && session->Window().Snapshot().open,
            "Session factory did not publish its actual open native window");
    owner->window = nativeWindow;
    Require(&session->Driver() == &AgcDriver::Metal::MetalDriver::Get(),
            "Session did not expose the singleton used by production entry points");
    config.initialRangeOwner.reset();
    config.initialRanges = {};
    owner.reset();
    ranges.clear();
    ranges.shrink_to_fit();
    Require(!retained.expired() && !retirement.released,
            "Session did not retain the genuine initial backing owner after all caller owners were dropped");
    const auto presentation = session->Window().Presentation(8, 4);
    auto layer = (__bridge CAMetalLayer*)presentation.metalLayer(presentation.context);
    Require(layer != nil && [layer isKindOfClass:CAMetalLayer.class] &&
            nativeWindow.contentView.layer == layer && layer.device != nil,
            "Session did not bind an actual native window CAMetalLayer to its Metal device");
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    Require(drawable != nil && drawable.texture.device == layer.device &&
            drawable.texture.width == session->Window().Snapshot().drawableWidth &&
            drawable.texture.height == session->Window().Snapshot().drawableHeight,
            "Session native layer did not produce a genuine drawable with its backing extent/device");
    drawable = nil;
    unsigned ready = 0;
    const AgcDriver::DisplayBuffer display{0x10000, 0x8000000000000000ull, 8, 4, 1, 8};
    session->Driver().Present(presentation, &display, true, +[](void* value) { ++*static_cast<unsigned*>(value); }, &ready);
    Require(ready == 1 && !retained.expired(),
            "Configured native driver did not complete presentation from the retained initial borrowed bytes");
    auto offMain = std::async(std::launch::async, [&] {
        @autoreleasepool {
            Reject<std::logic_error>([&] { session->ShutdownAfterCpuStoppedMainThread(); }, "AppKit main thread");
        }
    });
    offMain.get();
    Require(session->Window().Snapshot().open && !retained.expired(),
            "Rejected off-main shutdown changed live session ownership");
    session->Driver().WaitIdle();
    if (mode == "configured-unwind") {
        session->Window().RequestCloseMainThread();
        session->Window().CloseAfterGpuDrainMainThread();
        Require(!nativeWindow.visible && NSApp.delegate == previousDelegate,
                "Caller-drained host did not retire before configured factory unwind exercise");
        auto failedOwner = std::make_shared<Backing>();
        std::weak_ptr<Backing> failedRetained = failedOwner;
        std::array<Range, 1> failedRanges{{{0x20000, failedOwner->pixels, false, 41}}};
        auto failed = Configuration(library, "AnyPS5 configured session unwind");
        failed.initialRanges = failedRanges;
        failed.initialRangeOwner = failedOwner;
        Reject<std::runtime_error>([&] { NativeMetalSession::CreateMainThread(failed); },
                                   "native resources may only be configured before submission");
        failed.initialRangeOwner.reset();
        failedOwner.reset();
        Require(failedRetained.expired() && VisibleWindows() == visible && NSApp.delegate == previousDelegate,
                "Configured factory unwind retained its backing owner/window or application delegate");
        session->Driver().WaitIdle();
        Require(!retained.expired(), "Failed replacement factory drained or released the original session");
        std::cout << "PASS: configured factory failure unwound its real replacement window and backing owner before singleton drain\n";
    }
    if (mode == "backend-stopped") {
        session->Driver().Shutdown();
        Require(!retained.expired() && nativeWindow.visible,
                "Caller-side backend stop prematurely retired the session window or initial owner");
    }
    session->ShutdownAfterCpuStoppedMainThread();
    Require(retained.expired() && retirement.released && retirement.windowClosedBeforeRelease &&
            retirement.driverStoppedBeforeRelease && !nativeWindow.visible && VisibleWindows() == visible &&
            NSApp.delegate == previousDelegate,
            "Post-CPU-drain session shutdown did not stop driver/close window before releasing the initial owner");
    Reject<std::logic_error>([&] { session->Driver(); }, "session is closed");
    Reject<std::logic_error>([&] { session->Window(); }, "window is closed");
    session->ShutdownAfterCpuStoppedMainThread();
    session.reset();
    Require(VisibleWindows() == visible && NSApp.delegate == previousDelegate,
            "Idempotent session teardown changed retired native ownership");
    std::cout << "PASS: " << mode << " real device/library/layer, retained initial mapping, post-CPU-drain shutdown, access guards and idempotent owner release\n";
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 3, "Session acceptance requires explicit utility metallib and fresh-process mode");
            const std::string mode = argv[2];
            Require(mode == "own-shutdown" || mode == "backend-stopped" || mode == "configured-unwind",
                    "Unknown native session acceptance mode");
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            const auto invalidLibrary = std::filesystem::path(argv[0]).parent_path() / "invalid.metallib";
            FactoryGuards(argv[1], invalidLibrary);
            Run(argv[1], mode);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
