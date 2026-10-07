#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include <cpu/SceElf.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// Authoring gate: this protects the combined production ownership boundary:
// actual x86 CALL/RET -> typed VideoOut import -> registered native output ->
// PM4 flip -> drawable pixels -> callback/status order -> ordered retirement.
// A wiring omission, early status publication, or early owner/window release
// breaks it. Existing import tests mock callbacks and native-backend tests only
// register buffers; separate session/presentation tests do not combine them.
// The capture subclass exists only in this fixture; no production test seam.
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejects(Action&& action, const char* expected) {
    try { action(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Expected rejection: ") + expected);
}

id<CAMetalDrawable> captured = nil;
IMP originalNextDrawable = nullptr;
id<CAMetalDrawable> captureDrawable(CAMetalLayer* layer, SEL selector) {
    layer.framebufferOnly = NO;
    auto drawable = reinterpret_cast<id<CAMetalDrawable> (*)(id, SEL)>(originalNextDrawable)(layer, selector);
    captured = drawable;
    return drawable;
}

struct Retirement { bool released = false, closed = false, drained = false; };
struct Backing {
    std::unique_ptr<std::byte, decltype(&std::free)> bytes{nullptr, &std::free};
    Retirement* retirement;
    NSWindow* __weak window = nil;
    explicit Backing(Retirement& state) : retirement(&state) {
        void* allocation = nullptr;
        require(posix_memalign(&allocation, 65536, 65536) == 0, "Cannot allocate fixture display backing");
        bytes.reset(static_cast<std::byte*>(allocation));
        std::memset(bytes.get(), 0, 65536);
        for (unsigned offset = 0; offset < 64 * 64 * 4; offset += 4) {
            bytes.get()[offset] = std::byte{23}; bytes.get()[offset + 1] = std::byte{67};
            bytes.get()[offset + 2] = std::byte{149}; bytes.get()[offset + 3] = std::byte{255};
        }
    }
    ~Backing() {
        retirement->released = true;
        retirement->closed = !window.visible;
        try { AgcDriver::Metal::MetalDriver::Get().WaitIdle(); }
        catch (const std::runtime_error& error) {
            retirement->drained = std::string(error.what()).find("shut down") != std::string::npos;
        }
    }
    std::span<std::byte> Bytes() { return {bytes.get(), 65536}; }
};
struct Completion {
    id<MTLCommandQueue> queue;
    std::promise<void> done;
    unsigned count = 0;
};
std::uint64_t clock(void*) { return 101; }
std::uint64_t counter(void*) { return 202; }
void completed(void* context, VideoOutConfig& config, std::int64_t argument) {
    auto& state = *static_cast<Completion*>(context);
    try {
        require(argument == 0x1122334455667788LL && state.count == 0 && config.flipStatus.count == 0 &&
            config.flipStatus.flipPendingNum == 1 && config.bufferPending[0] == 1,
            "Flip callback did not precede completed status/pending-ticket publication");
        require(captured != nil, "Production native presenter did not acquire a real drawable");
        const auto texture = captured.texture;
        require(texture.width >= 64 && texture.height >= 64, "Actual native drawable extent is empty");
        const auto pitch = (texture.width * 4 + 255) & ~NSUInteger{255};
        auto bytes = [texture.device newBufferWithLength:pitch * texture.height options:MTLResourceStorageModeShared];
        require(bytes != nil, "Cannot allocate independent drawable readback");
        std::memset(bytes.contents, 0x35, bytes.length);
        auto commands = [state.queue commandBuffer];
        auto blit = [commands blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(texture.width, texture.height,1) toBuffer:bytes destinationOffset:0
            destinationBytesPerRow:pitch destinationBytesPerImage:pitch * texture.height];
        [blit endEncoding]; [commands commit]; [commands waitUntilCompleted];
        require(commands.status == MTLCommandBufferStatusCompleted, "Independent drawable GPU readback failed");
        constexpr std::array<unsigned char,4> golden{23,67,149,255};
        for (NSUInteger y = 0; y < texture.height; ++y) {
            for (NSUInteger x = 0; x < texture.width; ++x)
                require(std::memcmp(static_cast<unsigned char*>(bytes.contents) + y * pitch + x * 4,
                    golden.data(), golden.size()) == 0, "Production drawable differs from independent solid-color golden");
            for (NSUInteger byte = texture.width * 4; byte < pitch; ++byte)
                require(static_cast<unsigned char*>(bytes.contents)[y * pitch + byte] == 0x35,
                    "Drawable readback overwrote row-padding guards");
        }
        captured = nil;
        ++state.count;
        state.done.set_value();
    } catch (...) {
        state.done.set_exception(std::current_exception());
        throw;
    }
}
Cpu::SceImport videoImport(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid; value.LibraryName = value.ModuleName = "libSceVideoOut";
    value.LibraryVersion = 1; value.ModuleMajor = value.ModuleMinor = 1;
    value.LibraryId = 39; value.ModuleId = 40;
    return value;
}

void run(const char* utilityLibrary) {
    using Cpu::Register;
    using Cpu::Permission;
    Cpu::Machine machine;
    machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
    machine.Map(0x2000, 4096, Permission::Read | Permission::Write);
    machine.Map(0x3000, 4096, Permission::Read | Permission::Write);
    constexpr std::array<std::uint8_t,17> caller{
        0xff,0x15,0xfa,0x0f,0,0, 0x48,0x89,0x05,0x03,0x10,0,0, 0x48,0xff,0xc3,0x90};
    machine.Write(0x1000, std::as_bytes(std::span(caller)));
    Retirement retirement;
    auto backing = std::make_shared<Backing>(retirement);
    constexpr std::uint64_t displayAddress = 0x400000000ULL;
    machine.MapBorrowed(displayAddress, backing->Bytes(), Permission::Read | Permission::Write);
    std::array<AgcDriver::NativeGuestMemory::BorrowedRange,1> ranges{{{displayAddress, backing->Bytes(), false}}};
    AnyPS5::Host::NativeMetalSessionConfiguration configuration;
    configuration.window = {"GPU04 production VideoOut lifecycle fixture",64,64};
    configuration.utilityMetallib = utilityLibrary;
    configuration.initialRanges = ranges;
    configuration.initialRangeOwner = backing;
    configuration.initialGeneration = 17;
    Completion completion;
    auto finished = completion.done.get_future();
    const VideoOutCompletionCallbacks callbacks{&completion, clock, counter, completed};
    auto session = Cpu::SceNativeGraphicsSession::CreateMainThread(machine, configuration, callbacks);
    const auto presentation = session->Window().Presentation(64,64);
    auto layer = (__bridge CAMetalLayer*)presentation.metalLayer(presentation.context);
    completion.queue = [layer.device newCommandQueue];
    require(completion.queue != nil, "Cannot allocate independent readback queue");
    const auto originalClass = object_getClass(layer);
    const auto method = class_getInstanceMethod(originalClass, @selector(nextDrawable));
    originalNextDrawable = method_getImplementation(method);
    auto captureClass = objc_allocateClassPair(originalClass, "GPU04FixtureCaptureLayer", 0);
    require(captureClass != Nil && class_addMethod(captureClass, @selector(nextDrawable),
        reinterpret_cast<IMP>(captureDrawable), method_getTypeEncoding(method)), "Cannot observe actual fixture drawable");
    objc_registerClassPair(captureClass);
    require(class_getInstanceSize(captureClass) == class_getInstanceSize(originalClass), "Fixture capture changed layer storage size");
    object_setClass(layer, captureClass);
    for (NSWindow* window in NSApp.windows)
        if ([window.title isEqualToString:@"GPU04 production VideoOut lifecycle fixture"]) backing->window = window;
    require(backing->window != nil, "Native graphics session did not publish its actual fixture window");
    std::weak_ptr<Backing> retained = backing;
    backing.reset(); configuration.initialRangeOwner.reset(); configuration.initialRanges = {};
    auto callGate = [&](std::uint64_t gate, const std::array<std::uint64_t,8>& args) {
        constexpr std::array registers{Register::Rdi,Register::Rsi,Register::Rdx,Register::Rcx,Register::R8,Register::R9};
        for (unsigned index = 0; index < registers.size(); ++index) machine.Set(registers[index], args[index]);
        machine.Write(0x2000, std::as_bytes(std::span(&gate,1)));
        machine.Write(0x3fc0, std::as_bytes(std::span(args).last(2)));
        machine.Set(Register::Rsp, 0x3fc0); machine.Set(Register::Rbx, 41);
        require(machine.Run(0x1000,0x1011,100) == Cpu::StopReason::Address &&
            machine.Get(Register::Rsp) == 0x3fc0 && machine.Get(Register::Rbx) == 42,
            "Actual x86 VideoOut import did not preserve CALL/RET continuation");
        return machine.Get(Register::Rax);
    };
    auto call = [&](const char* nid, const std::array<std::uint64_t,8>& args) {
        return callGate(session->ResolveVideoOut(videoImport(nid)), args);
    };
    const auto openGate = session->ResolveVideoOut(videoImport("Up36PTk687E"));
    const auto handle = callGate(openGate, {255,0,0,0,0,0,0,0});
    require(handle == 1, "Actual guest VideoOut open failed to register native output");
    call("PjS5uASwcV8", {0x3000,0x8000000000000000ULL,1,64,64,0,0,0});
    const std::array<std::uint64_t,4> buffer{displayAddress,0,0,0};
    machine.Write(0x3100,std::as_bytes(std::span(buffer)));
    require(call("rKBUtgRrtbk",{handle,0,0,0x3100,1,0x3000,0,0}) == 0 &&
        call("CBiu4mCE1DA",{handle,0,0,0,0,0,0,0}) == 0 &&
        call("utPrVdxio-8",{handle,0x3200,0,0,0,0,0,0}) == 0,
        "Actual guest VideoOut registration/rate/status path failed");
    Cpu::SceVideoOutStatus status;
    machine.Read(0x3200,std::as_writable_bytes(std::span(&status,1)));
    require(status.Resolution == 1 && status.DynamicRange == 1 && status.RefreshRate == 3,
        "Guest status differs from actual native output geometry/refresh");
    auto offMain = std::async(std::launch::async,[&] {
        rejects([&] { session->ShutdownAfterCpuStoppedMainThread(); }, "AppKit main thread");
    });
    offMain.get();
    require(session->Window().Snapshot().open && !retained.expired(), "Off-main rejection retired live graphics ownership");
    constexpr std::array<std::uint32_t,6> flip{AgcDriver::FlipPacketHeader,1,0,1,0x55667788,0x11223344};
    machine.Write(displayAddress + 32768,std::as_bytes(std::span(flip)));
    session->Driver().SubmitCommandBuffer(displayAddress + 32768,flip.size(),0,0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (finished.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) session->Window().PumpMainThread(std::chrono::milliseconds(1));
    require(finished.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
        "Native production presenter did not complete the submitted flip");
    finished.get();
    session->Driver().WaitIdle();
    require(call("N5KDtkIjjJ4",{handle,0,0,0,0,0,0,0}) == 0 &&
        call("uquVH4-Du78",{handle,0,0,0,0,0,0,0}) == 0,
        "Guest buffer/output lifecycle failed after actual presentation");
    object_setClass(layer, originalClass);
    session->RequestStop();
    session->ShutdownAfterCpuStoppedMainThread();
    session->ShutdownAfterCpuStoppedMainThread();
    require(retained.expired() && retirement.released && retirement.closed && retirement.drained,
        "Graphics teardown released backing before GPU drain/window close or retained it after shutdown");
    rejects([&] { callGate(openGate,{255,0,0,0,0,0,0,0}); }, "runtime has expired");
    rejects([&] { session->ResolveVideoOut(videoImport("Up36PTk687E")); }, "session is closed");
    require(completion.count == 1, "Native graphics lifecycle fabricated or repeated a flip completion");
    std::cout << "PASS seven VideoOut guest imports through native session; actual drawable pixel/padding oracle; callback before status; off-main rejection; CPU-joined teardown, gate retirement and idempotence\n";
}
}
int main(int argc,const char* argv[]) {
    @autoreleasepool {
        try { require(argc == 2,"Graphics fixture needs utility metallib path"); run(argv[1]); return 0; }
        catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    }
}
