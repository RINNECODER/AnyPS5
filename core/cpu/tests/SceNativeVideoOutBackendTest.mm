#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cpu/Cpu.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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
        std::memset(bytes.get(), 0, 65536);
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
    auto pinned = std::make_shared<std::array<Backing, 3>>();
    std::weak_ptr<std::array<Backing, 3>> lease = pinned;
    auto& first = (*pinned)[0];
    auto& second = (*pinned)[1];
    auto& commands = (*pinned)[2];
    constexpr auto commandAddress = address + 2 * 65536;
    constexpr auto labelAddress = commandAddress + 256;
    Cpu::Machine machine;
    machine.MapBorrowed(address, first.Bytes(), rw);
    machine.MapBorrowed(address + 65536, second.Bytes(), Cpu::Permission::Write);
    machine.MapBorrowed(commandAddress, commands.Bytes(), rw);
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 2> ranges{{
        {address, first.Bytes(), false}, {commandAddress, commands.Bytes(), true}}};
    auto& metal = AgcDriver::Metal::MetalDriver::Get();
    auto initial = Cpu::BorrowGuestMemoryForMetal({0, {}, {}}, ranges, pinned);
    metal.Configure((__bridge void*)device, (__bridge void*)library, initial.Ranges);
    DriverSession driver;
    initial.Owner.reset();
    Cpu::SceNativeVideoOutBackend* videoMemoryOwner = nullptr;
    Cpu::GuestMemoryRuntime memory(machine, 1024 * 1024,
        [&](const Cpu::GuestMemorySnapshot& before, const Cpu::GuestMemorySnapshot& after, const std::function<void()>& mutateCpu) {
            auto owner = lease.lock();
            require(owner != nullptr, "Live mapping mutation lost genuine borrowed backing");
            auto previous = Cpu::BorrowGuestMemoryForMetal(before, ranges, owner);
            auto next = Cpu::BorrowGuestMemoryForMetal(after, ranges, owner);
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
    {
        auto ownedMappings = Cpu::BorrowGuestMemoryForMetal(memory.Snapshot(), ranges, pinned);
        Cpu::SceNativeVideoOutBackend backend(machine, presentation, completion, {},
            {ownedMappings.Ranges, ownedMappings.Owner, memory.Snapshot().Generation});
        ownedMappings.Owner.reset();
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
        // Capture a real native wait synchronously, but keep its execution behind
        // a genuine borrowed WAIT_REG_MEM label. No flip packet/reservation exists.
        require(callbacks.RegisterBuffers(handle, 0, 0, valid, attribute, 0) == 0,
            "Cannot register retired rendering-wait buffer");
        auto controlOutput = CreateVideoOutput(original, std::make_shared<FlipQueue>());
        auto shutdownWait = controlOutput->CaptureRenderingWait(0);
        auto failureWait = controlOutput->CaptureRenderingWait(0);
        const std::array<std::uint32_t, 11> waiting{
            0xc0053c00, 0x13, static_cast<std::uint32_t>(labelAddress),
            static_cast<std::uint32_t>(labelAddress >> 32), 1, 0xffffffffu, 0x19,
            AgcDriver::RenderingWaitPacketHeader, static_cast<std::uint32_t>(handle), 0, 0};
        const std::array<std::uint32_t, 5> release{
            0xc0033700, 0x00100200, static_cast<std::uint32_t>(labelAddress),
            static_cast<std::uint32_t>(labelAddress >> 32), 1};
        machine.Write(commandAddress, std::as_bytes(std::span(waiting)));
        machine.Write(commandAddress + 128, std::as_bytes(std::span(release)));
        auto expectedCommands = std::vector<std::byte>(commands.Bytes().begin(), commands.Bytes().end());
        const auto expectedDisplay = std::vector<std::byte>(first.Bytes().begin(), first.Bytes().end());
        metal.SubmitCommandBuffer(commandAddress, waiting.size(), 0, 0);
        {
            std::lock_guard lock(original->mutex);
            require(original->flipStatus.flipPendingNum == 0 && original->bufferPending[0] == 0 &&
                original->flipStatus.count == 0 && !original->failure,
                "No-flip rendering wait reserved a pending presentation or failed before close");
        }
        require(callbacks.Close(handle) == 0, "Zero-pending rendering wait prevented legal native close");
        const auto reopened = callbacks.Open(255, 0, 0, std::nullopt);
        require(reopened > 0 && reopened != handle, "Native output reopen reused a stale opaque handle");
        rejects([&] { callbacks.GetOutputStatus(handle); }, "invalid output handle", "Stale handle selected reopened output");
        auto replacement = backend.GetConfig(reopened);
        const auto replacementGeneration = replacement->generation;
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
        rejects([&] { std::lock_guard lock(original->mutex); original->Check(); },
            "port is closed", "Title-facing closed-port check was weakened");
        rejects([&] { controlOutput->CaptureRenderingWait(0); }, "port is closed",
            "Retired output accepted a new rendering wait");
        pinned.reset();
        require(!lease.expired(), "Native backend did not retain retired wait's genuine borrowed backing");
        // Release via another real Metal queue, avoiding a CPU/worker label race.
        metal.SubmitCommandBuffer(commandAddress + 128, release.size(), 0, 0x20);
        std::cout << "Draining captured native rendering wait after legal zero-pending close/reopen and label release\n";
        metal.WaitIdle();
        require(!lease.expired(), "Rendering wait lost borrowed command/label backing before drain");
        std::uint32_t published = 1;
        std::memcpy(expectedCommands.data() + 256, &published, sizeof(published));
        require(std::equal(expectedCommands.begin(), expectedCommands.end(), commands.Bytes().begin()) &&
            std::equal(expectedDisplay.begin(), expectedDisplay.end(), first.Bytes().begin()),
            "Retired wait changed source commands/display bytes beyond the released label");
        {
            std::lock_guard lock(original->mutex);
            require(!original->failure && original->flipStatus.count == 0 &&
                original->flipStatus.flipPendingNum == 0 && original->flipStatus.flipArg == -1 &&
                original->flipStatus.currentBuffer == -1,
                "Retired wait poisoned its original port or fabricated flip status");
        }
        {
            std::lock_guard lock(replacement->mutex);
            require(replacement->generation == replacementGeneration && !replacement->failure && replacement->opened &&
                !replacement->closing && replacement->flipStatus.count == 0 &&
                replacement->flipStatus.flipPendingNum == 0 && replacement->flipStatus.flipArg == -1 &&
                replacement->flipStatus.currentBuffer == -1 && !replacement->buffers[0].Occupied(),
                "Old captured wait changed replacement port identity/status");
        }
        require(backend.GetConfig(reopened) == replacement && events.count == 0,
            "Retired wait rerouted output identity or fabricated a completion event");
        require(callbacks.Close(reopened) == 0, "Native reopened output failed to close");
        retiredCallbacks = callbacks;
        memory.Shutdown();
        require(!lease.expired(), "Backend released real borrowed backing before shutdown");
        backend.Shutdown();
        videoMemoryOwner = nullptr;
        try {
            shutdownWait->Wait();
            throw std::runtime_error("Retired rendering wait swallowed actual backend process shutdown");
        } catch (const ProcessShutdown&) {}
        struct StoredFailure final : std::runtime_error { using std::runtime_error::runtime_error; };
        const auto failure = std::make_exception_ptr(StoredFailure("stored native rendering failure"));
        controlOutput->Fail(failure);
        try {
            failureWait->Wait();
            throw std::runtime_error("Retired rendering wait swallowed stored failure");
        } catch (const StoredFailure&) {
            require(std::current_exception() == failure, "Rendering wait replaced the stored failure identity");
        }
    }
    rejects([&] { retiredCallbacks.Open(255, 0, 0, std::nullopt); }, "expired",
        "Retired native callbacks opened an output after their session ended");
    require(events.count == 0, "Registration lifecycle fabricated a completed frame event");
    require(lease.expired(), "Native shutdown retained borrowed command/display owners");
    AgcDriver::Metal::MetalDriver::Get().Shutdown();
    [window close];
    std::cout << "PASS native VideoOut GPU-only readable backing, full data/DCC spans, atomic batch publication, fresh output generation and callback retirement; retired no-flip rendering wait drains cleanly, "
        "stored failure/shutdown propagate\n";
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
