#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libc/include/Shutdown.hpp"
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <exception>
#include <stdexcept>
#include <utility>

namespace AnyPS5::Host {
namespace {

void requireMainThread() {
    if (![NSThread isMainThread])
        throw std::logic_error("Native Metal session requires the AppKit main thread");
}

}

struct NativeMetalSession::Impl {
    std::shared_ptr<const void> initialRangeOwner;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = nil;
    std::unique_ptr<NativeHostWindow> window;
    bool configured = false;
    bool closed = false;
    std::exception_ptr shutdownFailure;

    ~Impl() {
        try { shutdown(); } catch (...) {}
    }

    void shutdown() {
        requireMainThread();
        if (closed) {
            if (shutdownFailure) std::rethrow_exception(shutdownFailure);
            return;
        }
        const auto attempt = [&](auto&& operation) {
            try { operation(); }
            // A process stop is the reason for this shutdown, not a failure of it.
            catch (const ProcessShutdown&) {}
            catch (...) { if (!shutdownFailure) shutdownFailure = std::current_exception(); }
        };
        if (window) attempt([&] { window->RequestCloseMainThread(); });
        if (configured) attempt([] { AgcDriver::Metal::MetalDriver::Get().Shutdown(); });
        if (window) {
            window->CloseAfterGpuDrainMainThread();
            window.reset();
        }
        initialRangeOwner.reset();
        library = nil;
        device = nil;
        closed = true;
        if (shutdownFailure) std::rethrow_exception(shutdownFailure);
    }
};

NativeMetalSession::NativeMetalSession(std::unique_ptr<Impl> state) : impl(std::move(state)) {}
NativeMetalSession::~NativeMetalSession() = default;

std::unique_ptr<NativeMetalSession> NativeMetalSession::CreateMainThread(
    const NativeMetalSessionConfiguration& configuration) {
    requireMainThread();
    if (configuration.utilityMetallib.empty())
        throw std::invalid_argument("Native Metal session requires an explicit utility metallib path");
    if (!std::filesystem::is_regular_file(configuration.utilityMetallib))
        throw std::invalid_argument("Native Metal session utility metallib path is not a regular file");
    if (!configuration.initialRanges.empty() && !configuration.initialRangeOwner)
        throw std::invalid_argument("Native Metal session initial guest ranges require a backing owner");
    {
        AgcDriver::NativeGuestMemory::BorrowedRangesScope validated(configuration.initialRanges);
    }
    const auto path = configuration.utilityMetallib.u8string();
    auto nativePath = [[NSString alloc] initWithBytes:path.data() length:path.size() encoding:NSUTF8StringEncoding];
    if (nativePath == nil)
        throw std::invalid_argument("Native Metal session utility metallib path is not UTF-8");
    auto state = std::make_unique<Impl>();
    state->initialRangeOwner = configuration.initialRangeOwner;
    state->device = MTLCreateSystemDefaultDevice();
    if (state->device == nil)
        throw std::runtime_error("Native Metal session requires a native Metal device");
    NSError* error = nil;
    state->library = [state->device newLibraryWithURL:[NSURL fileURLWithPath:nativePath] error:&error];
    if (state->library == nil)
        throw std::runtime_error(std::string("Native Metal session utility metallib load failed: ") +
            (error.localizedDescription.UTF8String ?: "unknown Metal library error"));
    if (state->library.device != state->device)
        throw std::runtime_error("Native Metal session utility metallib uses an incompatible device");
    state->window = NativeHostWindow::CreateMainThread(configuration.window);
    const auto presentation = state->window->Presentation(
        configuration.window.contentWidth, configuration.window.contentHeight);
    auto layer = (__bridge CAMetalLayer*)presentation.metalLayer(presentation.context);
    if (layer == nil)
        throw std::runtime_error("Native Metal session requires a native presentation layer");
    layer.device = state->device;
    AgcDriver::Metal::MetalDriver::Get().Configure(
        (__bridge void*)state->device, (__bridge void*)state->library,
        configuration.initialRanges, configuration.eopInterrupt, configuration.initialGeneration);
    state->configured = true;
    return std::unique_ptr<NativeMetalSession>(new NativeMetalSession(std::move(state)));
}

AgcDriver::Metal::MetalDriver& NativeMetalSession::Driver() {
    if (impl->closed) throw std::logic_error("Native Metal session is closed");
    return AgcDriver::Metal::MetalDriver::Get();
}

NativeHostWindow& NativeMetalSession::Window() {
    if (impl->closed || !impl->window) throw std::logic_error("Native Metal session window is closed");
    return *impl->window;
}

void NativeMetalSession::ShutdownAfterCpuStoppedMainThread() { impl->shutdown(); }

}
