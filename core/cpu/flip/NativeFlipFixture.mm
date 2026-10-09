#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include "FlipFixtureSupport.hpp"
#include "NativeAgcBackend.hpp"
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>

// Authoring gate: this fixture owns compiled qualified wait/flip+submit calls
// into registered native presentation. Existing VideoOut keeper injects the
// internal packet directly; existing AGC keeper never presents. Missing producer
// wiring, stale numeric pointer/handle routing, capture ownership or reuse wait
// ordering changes real drawable pixels/completion. No production seam is added.
namespace {
using namespace FlipFixture;
constexpr std::uint64_t Display = 0x400000000ULL, Attribute = Control + 512,
    Buffers = Control + 640;
constexpr unsigned Width = 64, Height = 64, Pitch = 128;
constexpr std::array<std::array<unsigned char, 4>, 3> Pixels{{
    {23, 67, 149, 255}, {191, 43, 71, 255}, {37, 179, 83, 255}}};
struct Page {
    std::unique_ptr<std::byte, decltype(&std::free)> bytes{nullptr, &std::free};
    explicit Page(unsigned color = 3) {
        void* allocation = nullptr;
        require(posix_memalign(&allocation, 65536, 65536) == 0, "Cannot allocate genuine native guest backing");
        bytes.reset(static_cast<std::byte*>(allocation)); std::memset(bytes.get(), 0xa7, 65536);
        if (color < Pixels.size())
            for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x)
                std::memcpy(bytes.get() + (y * Pitch + x) * 4, Pixels[color].data(), 4);
    }
    std::span<std::byte> Bytes() { return {bytes.get(), 65536}; }
};
struct Owners {
    std::array<std::shared_ptr<Page>, 3> display;
    std::shared_ptr<Page> commands;
};
id<CAMetalDrawable> captured = nil;
IMP originalNextDrawable = nullptr;
std::atomic<unsigned> acquisitions{0};
std::promise<void>* captureReady = nullptr;
std::shared_future<void> captureRelease;
id<CAMetalDrawable> observeDrawable(CAMetalLayer* layer, SEL selector) {
    layer.framebufferOnly = NO;
    captured = reinterpret_cast<id<CAMetalDrawable> (*)(id, SEL)>(originalNextDrawable)(layer, selector);
    require(captured != nil, "Native flip failed to acquire an actual drawable");
    ++acquisitions;
    if (captureReady) {
        captureReady->set_value(); captureReady = nullptr;
        require(captureRelease.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "Fixture did not release held native drawable");
    }
    return captured;
}
struct Capture {
    CAMetalLayer* layer;
    Class originalClass;
    explicit Capture(CAMetalLayer* value) : layer(value), originalClass(object_getClass(value)) {
        const auto method = class_getInstanceMethod(originalClass, @selector(nextDrawable));
        originalNextDrawable = method_getImplementation(method);
        auto observed = objc_allocateClassPair(originalClass, "GPU08TargetFlipCaptureLayer", 0);
        require(observed != Nil && class_addMethod(observed, @selector(nextDrawable),
            reinterpret_cast<IMP>(observeDrawable), method_getTypeEncoding(method)),
            "Cannot observe actual native flip drawable");
        objc_registerClassPair(observed);
        require(class_getInstanceSize(observed) == class_getInstanceSize(originalClass),
            "Native flip observer changed layer layout");
        object_setClass(layer, observed);
    }
    ~Capture() { captured = nil; object_setClass(layer, originalClass); }
};
struct Hold {
    std::promise<void> ready, release;
    std::future<void> entered = ready.get_future();
    bool active = true;
    Hold() { captureRelease = release.get_future().share(); captureReady = &ready; }
    void Resume() { if (active) { release.set_value(); active = false; } }
    ~Hold() { captureReady = nullptr; if (active) { try { release.set_value(); } catch (...) {} } }
};
struct Completion {
    id<MTLCommandQueue> queue;
    std::weak_ptr<Owners> owners;
    std::atomic<unsigned> count{0};
    std::promise<void> done;
    bool terminal = false;
    bool shutdownCase = false;
};
std::uint64_t clock(void*) { return 101; }
std::uint64_t counter(void*) { return 202; }
void completed(void* context, VideoOutConfig& config, std::int64_t argument) {
    auto& state = *static_cast<Completion*>(context);
    try {
        require(!state.shutdownCase, "Stopped pending flip fabricated a successful completion");
        const auto number = state.count.load();
        constexpr std::array<unsigned, 3> order{0, 1, 0};
        require(number < order.size() && argument == 0 && config.flipStatus.count == number &&
            config.flipStatus.flipPendingNum >= 1 && config.bufferPending[order[number]] >= 1,
            "Qualified flip completion order/argument/pending status differs");
        require(!state.owners.expired(), "Pending native flip lost genuine registered backing");
        require(captured != nil, "Native completion has no actual drawable");
        const auto texture = captured.texture;
        require(texture.width >= Width && texture.height >= Height, "Native drawable extent is empty");
        const auto row = (texture.width * 4 + 255) & ~NSUInteger{255};
        auto readback = [texture.device newBufferWithLength:row * texture.height options:MTLResourceStorageModeShared];
        require(readback != nil, "Cannot allocate independent drawable readback");
        std::memset(readback.contents, 0x35, readback.length);
        auto command = [state.queue commandBuffer]; auto blit = [command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(texture.width, texture.height, 1) toBuffer:readback destinationOffset:0
            destinationBytesPerRow:row destinationBytesPerImage:row * texture.height];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted, "Independent native drawable readback failed");
        for (NSUInteger y = 0; y < texture.height; ++y) {
            for (NSUInteger x = 0; x < texture.width; ++x)
                require(std::memcmp(static_cast<unsigned char*>(readback.contents) + y * row + x * 4,
                    Pixels[order[number]].data(), 4) == 0, "Qualified flip drawable/FIFO pixel oracle differs");
            for (NSUInteger byte = texture.width * 4; byte < row; ++byte)
                require(static_cast<unsigned char*>(readback.contents)[y * row + byte] == 0x35,
                    "Independent readback overwrote row padding");
        }
        captured = nil; ++state.count;
        if (number + 1 == order.size()) { state.terminal = true; state.done.set_value(); }
    } catch (...) {
        if (!state.terminal) { state.terminal = true; state.done.set_exception(std::current_exception()); }
        throw;
    }
}
// VideoOut setup uses existing real guest CALL/RET gates; the independently
// compiled primary entry is wait->flip and submission, rather than setup APIs.
std::uint64_t videoCall(Cpu::Machine& machine, Cpu::SceNativeGraphicsSession& session,
                       const char* nid, const std::array<std::uint64_t, 8>& args,
                       bool publicExtension = false) {
    const auto gate = publicExtension ? session.ResolveVideoOutPublicFixture(videoIdentity(nid)) :
        session.ResolveVideoOut(videoIdentity(nid), 2, 0);
    constexpr std::array registers{Cpu::Register::Rdi, Cpu::Register::Rsi, Cpu::Register::Rdx,
        Cpu::Register::Rcx, Cpu::Register::R8, Cpu::Register::R9};
    for (unsigned i = 0; i < registers.size(); ++i) machine.Set(registers[i], args[i]);
    machine.Set(Cpu::Register::Rax, gate);
    // The gate's stack args are at caller RSP before CALL. A dedicated wrapper
    // allocates two slots before its call and restores them before returning.
    store(machine, 0x5fd0, args[6]); store(machine, 0x5fd8, args[7]);
    machine.Set(Cpu::Register::Rsp, 0x5fd0);
    constexpr std::array<unsigned char, 11> call{0xff, 0xd0, 0x48, 0x83, 0xc4, 0x10,
        0xc3, 0x90, 0x90, 0x90, 0x90};
    machine.Write(0x2000, std::as_bytes(std::span(call)));
    store(machine, 0x5fe0, std::uint64_t{0x1000});
    require(machine.Run(0x2000, 0x1000, 200) == Cpu::StopReason::Address &&
        machine.Get(Cpu::Register::Rsp) == 0x5fe8, "VideoOut setup guest call did not return");
    return machine.Get(Cpu::Register::Rax);
}
void pumpUntil(Cpu::SceNativeGraphicsSession& session, std::future<void>& future, const char* reason) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
        std::chrono::steady_clock::now() < deadline)
        session.Window().PumpMainThread(std::chrono::milliseconds(1));
    require(future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, reason);
    future.get();
}
void run(const char* utility, const char* const* paths, bool shutdownCase) {
    Cpu::Machine machine;
    auto owners = std::make_shared<Owners>();
    owners->commands = std::make_shared<Page>();
    Caller caller(machine, paths, owners->commands->Bytes().first(4096), owners->commands->Bytes());
    machine.Map(0x2000, 4096, RX);
    std::array<std::vector<std::byte>, 3> original;
    for (unsigned i = 0; i < owners->display.size(); ++i) {
        owners->display[i] = std::make_shared<Page>(i);
        const auto bytes = owners->display[i]->Bytes();
        original[i] = std::vector<std::byte>(bytes.begin(), bytes.end());
        machine.MapBorrowed(Display + i * 0x100000, bytes, RW);
    }
    const std::array ranges{
        AgcDriver::NativeGuestMemory::BorrowedRange{Display, owners->display[0]->Bytes(), false, 171},
        AgcDriver::NativeGuestMemory::BorrowedRange{Display + 0x100000, owners->display[1]->Bytes(), false, 172},
        AgcDriver::NativeGuestMemory::BorrowedRange{Display + 0x200000, owners->display[2]->Bytes(), false, 173},
        AgcDriver::NativeGuestMemory::BorrowedRange{Commands, owners->commands->Bytes().first(4096), true, 271}};
    AnyPS5::Host::NativeMetalSessionConfiguration configuration;
    configuration.window = {"GPU08 qualified imported wait/flip fixture", Width, Height};
    configuration.utilityMetallib = utility; configuration.initialRanges = ranges;
    configuration.initialRangeOwner = owners; configuration.initialGeneration = 17;
    Completion completion; completion.owners = owners; completion.shutdownCase = shutdownCase;
    auto finished = completion.done.get_future();
    const VideoOutCompletionCallbacks callbacks{&completion, clock, counter, completed};
    auto session = Cpu::SceNativeGraphicsSession::CreateMainThread(machine, configuration, callbacks,
        {}, 0x7ffdfd000000ULL, Cpu::TargetVideoOutAdmissions());
    Cpu::SceAgcImports imports(machine, Cpu::MakeNativeAgcBackend(session->Driver()),
        Cpu::TargetAgcAdmissions());
    const auto flip = imports.Resolve(identity(FlipNid), 2, 0);
    const auto wait = imports.Resolve(identity(WaitNid), 2, 0);
    const auto submit = imports.Resolve(identity(SubmitNid, true), 2, 0);
    const auto presentation = session->Window().Presentation(Width, Height);
    auto layer = (__bridge CAMetalLayer*)presentation.metalLayer(presentation.context);
    completion.queue = [layer.device newCommandQueue]; require(completion.queue != nil, "No independent native readback queue");
    Capture capture(layer);
    const auto handle = videoCall(machine, *session, "Up36PTk687E", {255, 0, 0, 0});
    require(handle > 0 && handle <= INT32_MAX, "Qualified VideoOut did not return an opaque handle");
    // Exact qualified target registration shape, separate from pixel format
    // support and the explicitly public linear extension used for readback.
    videoCall(machine, *session, "PjS5uASwcV8", {Attribute, 0x8000000000000000ULL, 0, Width, Height, 0, 0, 0});
    for (unsigned i = 0; i < 3; ++i) {
        const std::array<std::uint64_t, 4> row{Display + i * 0x100000, 0, 0, 0};
        store(machine, Buffers + i * 32, row);
    }
    require(videoCall(machine, *session, "rKBUtgRrtbk", {handle, 0, 0, Buffers, 3, Attribute, 0, 0}) == 0 &&
        videoCall(machine, *session, "N5KDtkIjjJ4", {handle, 0}) == 0,
        "Qualified three-distinct-backing tiled registration failed");
    videoCall(machine, *session, "PjS5uASwcV8", {Attribute, 0x8000000000000000ULL, 1, Width, Height, 0, 0, 0}, true);
    store(machine, Attribute + 20, std::uint32_t{Pitch});
    require(videoCall(machine, *session, "rKBUtgRrtbk", {handle, 0, 0, Buffers, 3, Attribute, 0, 0}, true) == 0 &&
        videoCall(machine, *session, "CBiu4mCE1DA", {handle, 0}) == 0,
        "Public linear registered pixel extension failed");
    const auto produce = [&](unsigned index, std::uint64_t outputHandle = 0, bool reset = true) {
        if (reset) builder(machine);
        const auto cursor = load<std::uint64_t>(machine, Builder + 16);
        const auto h = outputHandle ? outputHandle : handle;
        require(caller.call(Caller::Sequence, flip, {Builder, h, index, 1, 0, wait}) == cursor + 16,
            "Qualified compiled producer returned wrong flip pointer");
        const std::array<std::uint32_t, 10> expected{0xc0021018, static_cast<std::uint32_t>(h), index, 0,
            0xc004105c, static_cast<std::uint32_t>(h), index, 1, 0, 0};
        require(load<std::array<std::uint32_t, 10>>(machine, cursor) == expected &&
            load<std::uint64_t>(machine, Builder + 16) == cursor + 40,
            "Qualified compiled producer differs from independent internal bridge oracle");
    };
    const auto dispatch = [&](unsigned words = 10, std::uint64_t address = Commands, unsigned flags = 0) {
        return caller.call(Caller::Submit, submit, {address, words, flags});
    };
    const auto rejectSubmission = [&](auto&& action, const char* reason) {
        const auto before = std::vector<std::byte>(owners->commands->Bytes().begin(), owners->commands->Bytes().end());
        rejects(action, reason); session->Driver().WaitIdle();
        require(std::equal(before.begin(), before.end(), owners->commands->Bytes().begin()) &&
            completion.count == 0 && acquisitions == 0,
            "Rejected native flip changed command backing or published a display/completion");
    };
    produce(0); rejectSubmission([&] { dispatch(9); }, "truncated PM4 packet");
    produce(0); store(machine, Commands + 16, std::uint32_t{0xc004105d});
    rejectSubmission([&] { dispatch(); }, "unsupported NOP header flags");
    produce(0); rejectSubmission([&] { dispatch(10, Commands + 1); }, "misaligned");
    produce(0); rejectSubmission([&] { dispatch(10, 0); }, "misaligned");
    produce(0); rejectSubmission([&] { dispatch(10, Control); }, "not borrowed");
    produce(0); rejectSubmission([&] { dispatch(10, Commands, 1); }, "nonzero command submission flags");
    produce(0, 0x7fffffff); rejectSubmission([&] { dispatch(); }, "unregistered video output");
    produce(0); store(machine, Commands + 8, std::uint32_t{3});
    rejectSubmission([&] { dispatch(); }, "wait buffer is not registered");
    produce(0); store(machine, Commands + 28, std::uint32_t{0});
    rejectSubmission([&] { dispatch(); }, "unsupported flip mode");
    // First reservation succeeds; a later unregistered slot must unwind it without
    // a display or retained pending ticket before the next valid submission.
    produce(0);
    const std::array<std::uint32_t, 6> unregistered{0xc004105c, static_cast<std::uint32_t>(handle), 3, 1, 0, 0};
    machine.Write(Commands + 40, std::as_bytes(std::span(unregistered)));
    rejectSubmission([&] { dispatch(16); }, "flip buffer is not registered");

    Hold held;
    produce(0);
    if (!shutdownCase) {
        produce(1, 0, false);
        produce(0, 0, false);
        require(load<std::uint64_t>(machine, Builder + 16) == Commands + 120,
            "Three compiled wait/flip pairs did not produce one thirty-word stream");
    }
    // Presentation holds the native memory capture lock across nextDrawable.
    // Build the full stream first so the fixture's own hold cannot block a
    // subsequent submission. All three reservations precede worker execution.
    require(dispatch(shutdownCase ? 10 : 30) == 0, "Qualified compiled native flip submit failed");
    pumpUntil(*session, held.entered, "Qualified flip did not reach held actual drawable");
    require(completion.count == 0 && acquisitions == 1, "Held native frame completed before presentation release");
    rejects([&] { videoCall(machine, *session, "N5KDtkIjjJ4", {handle, 0}); }, "pending flip");
    rejects([&] { videoCall(machine, *session, "uquVH4-Du78", {handle}); }, "pending flip");
    auto replacement = std::make_shared<Page>(1);
    auto rebound = ranges; rebound[0].host = replacement->Bytes();
    bool mutated = false;
    rejects([&] { session->MutateBorrowedRanges(rebound, 18, [&] { mutated = true; }, owners, replacement); },
        "registered VideoOut backing cannot be retired or rebound");
    require(!mutated && !completion.owners.expired(), "Pending flip retired registered mapping before CPU rejection");
    configuration.initialRangeOwner.reset(); configuration.initialRanges = {};
    std::weak_ptr<Owners> retained = owners;
    if (shutdownCase) {
        owners.reset(); require(!retained.expired(), "Pending shutdown lost genuine native backing");
        session->RequestStop(); held.Resume();
        session->ShutdownAfterCpuStoppedMainThread();
        require(completion.count == 0 && acquisitions == 1 && retained.expired(),
            "Pending stop published completion or retained backing after native shutdown");
        std::cout << "PASS qualified compiled wait/flip+submit pending stop: actual held drawable; "
            "no fabricated completion; genuine registered backing retained until shutdown\n";
        return;
    }
    // Reuse wait0 is captured before reserving the third flip0. The stream can
    // wait only for the preceding ticket, rather than deadlocking on its own.
    require(completion.count == 0 && acquisitions == 1, "Reuse/ordered submission bypassed held prior presentation");
    owners.reset(); require(!retained.expired(), "Queued native flips lost genuine command/display owners");
    held.Resume(); pumpUntil(*session, finished, "Qualified wait/flip FIFO or reuse ticket did not complete");
    session->Driver().WaitIdle();
    require(completion.count == 3 && acquisitions == 3, "Qualified native path lost or fabricated a presentation");
    {
        auto live = retained.lock(); require(live != nullptr, "Completed registered buffers lost backing before unregister");
        for (unsigned i = 0; i < 3; ++i)
            require(std::equal(original[i].begin(), original[i].end(), live->display[i]->Bytes().begin()),
                "Qualified presentation changed source pixels/pitch padding");
    }
    require(videoCall(machine, *session, "N5KDtkIjjJ4", {handle, 0}) == 0 &&
        videoCall(machine, *session, "uquVH4-Du78", {handle}) == 0,
        "Completed qualified output failed to retire");
    const auto reopened = videoCall(machine, *session, "Up36PTk687E", {255, 0, 0, 0});
    require(reopened != handle, "Reopened native output reused retired handle generation");
    produce(0, handle);
    rejects([&] { dispatch(); }, "unregistered video output");
    session->Driver().WaitIdle();
    require(completion.count == 3 && acquisitions == 3, "Retired handle published into reopened output");
    require(videoCall(machine, *session, "uquVH4-Du78", {reopened}) == 0, "Reopened output failed to close");
    session->RequestStop(); session->ShutdownAfterCpuStoppedMainThread();
    require(retained.expired(), "Native shutdown retained genuine registered backing");
    std::cout << "PASS compiled x86 qualified wait/flip/submit -> ARM64 native registered drawable: "
        "target-shaped registration separate from public linear pixel oracle; three FIFO pixels; "
        "reuse ticket; rejected input has no display; pending/retired handle and owner guards\n";
}
}
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            FlipFixture::require(argc == 6 || argc == 7, "Native flip fixture requires metallib/four callers/optional shutdown");
            run(argv[1], argv + 2, argc == 7 && std::string(argv[6]) == "shutdown"); return 0;
        } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    }
}
