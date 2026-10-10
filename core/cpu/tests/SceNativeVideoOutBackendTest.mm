#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cpu/Cpu.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

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

// Equeue side of the backend: records registrations and producer publications.
struct EventLog {
    std::mutex mutex;
    struct Row { std::int32_t handle, kind; std::int64_t payload; };
    std::vector<Row> published;
    unsigned adds = 0, deletes = 0;
    Cpu::SceVideoOutEventSink Sink() {
        Cpu::SceVideoOutEventSink sink;
        sink.Add = [this](std::uint64_t queue, std::int32_t, std::int32_t kind, std::uint64_t udata) {
            require(queue == 0x77 && kind == VIDEO_OUT_EVENT_FLIP && udata == 0x55, "Event sink add lost arguments");
            ++adds; return 0;
        };
        sink.Delete = [this](std::uint64_t queue, std::int32_t, std::int32_t kind) {
            require(queue == 0x77 && kind == VIDEO_OUT_EVENT_FLIP, "Event sink delete lost arguments");
            ++deletes; return 0;
        };
        sink.Publish = [this](std::int32_t handle, std::int32_t kind, std::int64_t payload) {
            std::lock_guard lock(mutex); published.push_back({handle, kind, payload});
        };
        return sink;
    }
    std::vector<Row> Snapshot() { std::lock_guard lock(mutex); return published; }
};

template<class TPredicate> void waitFor(TPredicate&& done, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
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
    auto pinned = std::make_shared<std::array<Backing, 2>>();
    auto& first = (*pinned)[0];
    auto& second = (*pinned)[1];
    Cpu::Machine machine;
    machine.MapBorrowed(address, first.Bytes(), rw);
    machine.MapBorrowed(address + 65536, second.Bytes(), Cpu::Permission::Write);
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 1> ranges{{
        {address, first.Bytes(), false}}};
    auto& metal = AgcDriver::Metal::MetalDriver::Get();
    auto initial = Cpu::BorrowGuestMemoryForMetal({0, {}, {}}, ranges, pinned);
    metal.Configure((__bridge void*)device, (__bridge void*)library, initial.Ranges);
    DriverSession driver;
    Cpu::SceNativeVideoOutBackend* videoMemoryOwner = nullptr;
    Cpu::GuestMemoryRuntime memory(machine, 1024 * 1024,
        [&](const Cpu::GuestMemorySnapshot& before, const Cpu::GuestMemorySnapshot& after, const std::function<void()>& mutateCpu) {
            auto previous = Cpu::BorrowGuestMemoryForMetal(before, ranges, pinned);
            auto next = Cpu::BorrowGuestMemoryForMetal(after, ranges, pinned);
            if (videoMemoryOwner) videoMemoryOwner->MutateBorrowedRanges(next.Ranges, after.Generation, mutateCpu,
                std::move(previous.Owner), std::move(next.Owner));
            else metal.MutateBorrowedRanges(next.Ranges, after.Generation, mutateCpu,
                std::move(previous.Owner), std::move(next.Owner));
        });
    constexpr std::uint64_t gpuRead = 0x1000800000ULL;
    constexpr std::uint64_t gpuReadWrite = gpuRead + 2 * 65536;
    const auto physical = memory.AllocateDirect(0, 1024 * 1024, 2 * 65536, 65536, 0);
    require(memory.MapDirect(gpuRead, 65536, 0x10, 0x90, physical, 65536) == gpuRead &&
        memory.MapDirect(gpuReadWrite, 65536, 0x30, 0x90, physical + 65536, 65536) == gpuReadWrite,
        "Cannot create GPU-readable display mappings without CPU permissions");
    require(memory.Query(gpuRead).Protection == 0x10 && memory.Query(gpuReadWrite).Protection == 0x30,
        "GPU-only display setup accidentally granted CPU permissions");
    rejects([&] { machine.CheckAccess(gpuRead, 65536, Cpu::Permission::Read); },
        "Guest access denied at 0x1000800000 for permissions 1", "GPU read-only mapping became CPU-readable");
    rejects([&] { machine.CheckAccess(gpuReadWrite, 65536, Cpu::Permission::Read); },
        "Guest access denied at 0x1000820000 for permissions 1", "GPU read/write mapping became CPU-readable");
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
    EventLog eventLog;
    {
        auto ownedMappings = Cpu::BorrowGuestMemoryForMetal(memory.Snapshot(), ranges, pinned);
        Cpu::SceNativeVideoOutBackend backend(machine, presentation, completion, {},
            {ownedMappings.Ranges, ownedMappings.Owner, memory.Snapshot().Generation}, eventLog.Sink());
        videoMemoryOwner = &backend;
        const auto callbacks = backend.GetCallbacks();
        const auto handle = callbacks.Open(255, 0, 0, std::nullopt);
        require(handle > 0, "Native backend failed to open actual output");
        auto original = backend.GetConfig(handle);
        const auto generation = original->generation;
        Cpu::SceVideoOutAttribute attribute;
        attribute.Width = 64;
        attribute.Height = 64;
        attribute.PixelFormat = 0x8000000000000000ULL;
        const std::array<Cpu::SceVideoOutBuffer, 1> gpuReadBuffer{{{gpuRead, 0, {}}}};
        std::cout << "Checking native registration of raw 0x10 GPU-readable, CPU-inaccessible display backing\n";
        require(callbacks.RegisterBuffers(handle, 0, 0, gpuReadBuffer, attribute, 0) == 0,
            "Native display registration required CPU Read for a valid GPU-readable allocation");
        require(callbacks.UnregisterBuffers(handle, 0) == 0, "GPU read-only display failed to unregister");
        const std::array<Cpu::SceVideoOutBuffer, 1> gpuReadWriteBuffer{{{gpuReadWrite, 0, {}}}};
        require(callbacks.RegisterBuffers(handle, 0, 0, gpuReadWriteBuffer, attribute, 0) == 0,
            "Native display registration required CPU Read for a valid GPU read/write allocation");
        require(callbacks.UnregisterBuffers(handle, 0) == 0, "GPU read/write display failed to unregister");
        auto oversized = attribute;
        oversized.Width = 129;
        oversized.Height = 128;
        rejects([&] { callbacks.RegisterBuffers(handle, 0, 0, gpuReadBuffer, oversized, 0); },
            "Metal driver: readable guest range is not fully borrowed",
            "Native display registration validated only the first of two 64 KiB tiles");
        require(callbacks.RegisterBuffers(handle, 0, 0, gpuReadBuffer, attribute, 0) == 0,
            "Incomplete GPU display extent partially published group or slot state");
        require(callbacks.UnregisterBuffers(handle, 0) == 0, "Retried GPU display failed to unregister");
        constexpr std::uint64_t gpuWriteOnly = gpuRead + 4 * 65536;
        const auto beforeWriteOnly = memory.Snapshot();
        rejects([&] { memory.MapDirect(gpuWriteOnly, 65536, 0x20, 0x90, physical, 65536); },
            "GPU write-only guest memory is not representable by Metal borrowed ranges",
            "GPU write-only mapping was silently published as readable");
        const auto afterWriteOnly = memory.Snapshot();
        require(afterWriteOnly.Generation == beforeWriteOnly.Generation &&
            afterWriteOnly.Views.size() == beforeWriteOnly.Views.size(),
            "Rejected GPU write-only publication changed the live allocation snapshot");
        const std::array<Cpu::SceVideoOutBuffer, 1> rejectedWriteOnly{{{gpuWriteOnly, 0, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 0, 0, rejectedWriteOnly, attribute, 0); },
            "Metal driver: readable guest range is not fully borrowed",
            "Native display registration accepted a rejected GPU write-only mapping");
        require(callbacks.RegisterBuffers(handle, 0, 0, gpuReadBuffer, attribute, 0) == 0,
            "Rejected GPU write-only display partially published native state");
        require(callbacks.UnregisterBuffers(handle, 0) == 0, "GPU write-only rejection retry failed to unregister");
        auto gpuCompressed = attribute;
        gpuCompressed.DccControl = 0x208;
        const std::array<Cpu::SceVideoOutBuffer, 1> gpuShortMetadata{{{gpuRead, gpuRead + 65536 - 128, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 0, 0, gpuShortMetadata, gpuCompressed, 1); },
            "Metal driver: readable guest range is not fully borrowed",
            "Native DCC registration validated only 128 of the required 256 GPU-readable metadata bytes");
        const std::array<Cpu::SceVideoOutBuffer, 1> gpuMetadata{{{gpuRead, gpuReadWrite + 64000, {}}}};
        require(callbacks.RegisterBuffers(handle, 0, 0, gpuMetadata, gpuCompressed, 1) == 0,
            "Native DCC registration required CPU Read for GPU-readable metadata");
        {
            std::lock_guard lock(original->mutex);
            require(original->buffers[0].dataAddress == gpuRead &&
                original->buffers[0].metadataAddress == gpuReadWrite + 64000,
                "GPU-only display registration lost numeric data or metadata guest VAs");
        }
        require(callbacks.UnregisterBuffers(handle, 0) == 0, "GPU-only DCC display failed to unregister");
        const std::array<Cpu::SceVideoOutBuffer, 2> invalid{{
            {address, 0, {}}, {0x500000000ULL, 0, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 0, 0, invalid, attribute, 0); },
            "Metal driver: readable guest range is not fully borrowed",
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
            "Metal driver: readable guest range is not fully borrowed",
            "Native backend accepted a CPU write-only span without readable GPU backing");
        require(callbacks.RegisterBuffers(handle, 1, 3, valid, attribute, 0) == 0,
            "Unreadable display batch partially published native state");
        auto compressed = attribute;
        compressed.DccControl = 0x208;
        const std::array<Cpu::SceVideoOutBuffer, 1> shortMetadata{{{address, address + 65536 - 128, {}}}};
        rejects([&] { callbacks.RegisterBuffers(handle, 2, 6, shortMetadata, compressed, 1); },
            "Metal driver: readable guest range is not fully borrowed",
            "Native backend checked only the first half of DCC metadata");
        const std::array<Cpu::SceVideoOutBuffer, 1> completeMetadata{{{address, address + 64000, {}}}};
        require(callbacks.RegisterBuffers(handle, 2, 6, completeMetadata, compressed, 1) == 0,
            "Rejected DCC metadata partially published native group state");
        require(callbacks.UnregisterBuffers(handle, 0) == 0 && callbacks.UnregisterBuffers(handle, 1) == 0 &&
            callbacks.UnregisterBuffers(handle, 2) == 0,
            "Native backend did not retire successfully registered groups");
        std::cout << "Checking CPU SubmitFlip, flip/vblank status and equeue publication on a 2-buffer linear set\n";
        auto linear = attribute;
        linear.TilingMode = 1;
        const std::array<Cpu::SceVideoOutBuffer, 2> pair{{{gpuRead, 0, {}}, {gpuReadWrite, 0, {}}}};
        require(callbacks.RegisterBuffers(handle, 3, 8, pair, linear, 0) == 0,
            "Native backend refused a 2-buffer linear registration");
        require(callbacks.RegisterBuffers(handle, 3, 10, valid, attribute, 0) == VIDEO_OUT_ERROR_INVALID_INDEX &&
            callbacks.RegisterBuffers(handle, 0, 9, valid, attribute, 0) == VIDEO_OUT_ERROR_SLOT_OCCUPIED,
            "Occupied set/slot was not an SCE result");
        require(callbacks.AddEvent(0x77, handle, VIDEO_OUT_EVENT_FLIP, 0x55) == 0 && eventLog.adds == 1 &&
            callbacks.AddEvent(0x77, 0x7fffffff, VIDEO_OUT_EVENT_FLIP, 0x55) == VIDEO_OUT_ERROR_INVALID_HANDLE &&
            eventLog.adds == 1, "Flip event registration did not reach the equeue sink for a valid handle only");
        require(callbacks.SubmitFlip(handle, 7, 1, 1) == VIDEO_OUT_ERROR_INVALID_INDEX &&
            callbacks.SubmitFlip(handle, 8, 0, 1) == VIDEO_OUT_ERROR_INVALID_VALUE &&
            callbacks.SubmitFlip(0x7fffffff, 8, 1, 1) == VIDEO_OUT_ERROR_INVALID_HANDLE,
            "Invalid CPU flip was not an SCE result");
        const auto vblankBefore = callbacks.GetVblankStatus(handle);
        require(vblankBefore.Result == 0 && callbacks.WaitVblank(handle) == 0 &&
            callbacks.GetVblankStatus(handle).Status.Count > vblankBefore.Status.Count,
            "WaitVblank returned before the next vblank");
        require(callbacks.SubmitFlip(handle, 9, 1, -42) == 0, "CPU SubmitFlip of a registered linear buffer failed");
        waitFor([&] { return callbacks.GetFlipStatus(handle).Status.Count == 1; }, "CPU flip never completed");
        const auto flipped = callbacks.GetFlipStatus(handle).Status;
        require(flipped.FlipArg == -42 && flipped.CurrentBuffer == 9 && flipped.FlipPendingNum == 0 &&
            callbacks.IsFlipPending(handle) == 0, "Flip status differs after a completed CPU flip");
        require(callbacks.SubmitFlip(handle, VIDEO_OUT_BUFFER_INDEX_BLANK, 1, 7) == 0, "CPU blank flip failed");
        waitFor([&] { return callbacks.GetFlipStatus(handle).Status.Count == 2; }, "CPU blank flip never completed");
        require(callbacks.GetFlipStatus(handle).Status.CurrentBuffer == -1 && events.count == 2,
            "Blank flip status or owner completion count differs");
        std::vector<std::int64_t> flips, vblanks;
        for (const auto& row : eventLog.Snapshot()) {
            require(row.handle == handle, "Event published for another output");
            (row.kind == VIDEO_OUT_EVENT_FLIP ? flips : vblanks).push_back(row.payload);
        }
        require((flips == std::vector<std::int64_t>{-42, 7}), "Flip events did not carry each flip argument once");
        require(!vblanks.empty() && std::is_sorted(vblanks.begin(), vblanks.end()) &&
            std::adjacent_find(vblanks.begin(), vblanks.end()) == vblanks.end() &&
            vblanks.back() >= static_cast<std::int64_t>(vblankBefore.Status.Count + 1),
            "Vblank events did not carry the increasing vblank count");
        const auto resolution = callbacks.GetResolutionStatus(handle);
        require(resolution.Result == 0 && resolution.Status.FullWidth == 64 && resolution.Status.FullHeight == 64 &&
            resolution.Status.RefreshRate == VIDEO_OUT_REFRESH_RATE_59_94HZ, "Resolution status differs from the output");
        require(callbacks.ConfigureOutput(handle, VIDEO_OUT_OUTPUT_MODE_DEFAULT) == 0 &&
            callbacks.ConfigureOutput(handle, VIDEO_OUT_OUTPUT_MODE_119_88HZ) == VIDEO_OUT_ERROR_UNAVAILABLE_OUTPUT_MODE &&
            callbacks.SetWindowModeMargins(handle, 0, 0) == 0 &&
            callbacks.SetWindowModeMargins(0x7fffffff, 0, 0) == VIDEO_OUT_ERROR_INVALID_HANDLE,
            "Output mode or window margins differ");
        require(callbacks.DeleteEvent(0x77, handle, VIDEO_OUT_EVENT_FLIP) == 0 && eventLog.deletes == 1,
            "Flip event deletion did not reach the equeue sink");
        AgcDriver::Metal::MetalDriver::Get().WaitIdle();
        require(callbacks.UnregisterBuffers(handle, 3) == 0, "Flipped linear set failed to unregister");
        require(callbacks.Close(handle) == 0, "Native backend failed to unregister its GPU output");
        const auto reopened = callbacks.Open(255, 0, 0, std::nullopt);
        require(reopened > 0 && reopened != handle, "Native output reopen reused a stale opaque handle");
        require(callbacks.GetOutputStatus(handle).Result == VIDEO_OUT_ERROR_INVALID_HANDLE,
            "Stale handle selected reopened output");
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
        memory.Shutdown();
        backend.Shutdown();
        videoMemoryOwner = nullptr;
    }
    rejects([&] { retiredCallbacks.Open(255, 0, 0, std::nullopt); }, "expired",
        "Retired native callbacks opened an output after their session ended");
    require(events.count == 2, "Registration lifecycle fabricated or lost a completed frame event");
    AgcDriver::Metal::MetalDriver::Get().Shutdown();
    [window close];
    std::cout << "PASS native VideoOut GPU-only readable backing, full data/DCC spans, atomic batch publication, CPU flips on a 2-buffer linear set with flip/vblank equeue publication, fresh output generation and callback retirement\n";
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
