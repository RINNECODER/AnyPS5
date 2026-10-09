#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include <cpu/Cpu.hpp>
#include <cpu/ElfLoader.hpp>
#include <cpu/Runtime.hpp>
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include "prx/libSceVideoOut/include/UnobtrusiveWindows.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

@interface CpuGuestMetalLayer : CAMetalLayer
@property(nonatomic, strong) id<CAMetalDrawable> capturedDrawable;
@end
@implementation CpuGuestMetalLayer
- (id<CAMetalDrawable>)nextDrawable {
    self.framebufferOnly = NO;
    id<CAMetalDrawable> drawable = [super nextDrawable];
    self.capturedDrawable = drawable;
    return drawable;
}
@end

namespace {
constexpr std::uint32_t Width = 64, Height = 32, PixelBytes = Width * Height * 4;
constexpr std::uint64_t ColorAddress = 0x210000, GateAddress = 0x10000000;
static_assert(sizeof(Shader) == 96 && sizeof(ShaderUserData) == 56);
static_assert(offsetof(Shader, user_data) == 8 && offsetof(Shader, code) == 16);
static_assert(offsetof(Shader, header_size) == 64 && offsetof(Shader, shader_size) == 68 && offsetof(Shader, type) == 90);
static_assert(sizeof(Packet) == 16 && offsetof(Packet, dw_num) == 8 && offsetof(Packet, flags) == 12);
static_assert(sizeof(AgcDriver::DisplayBuffer) == 48 && offsetof(AgcDriver::DisplayBuffer, tilingMode) == 24);

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Page {
    std::uint64_t address;
    std::size_t size;
    std::unique_ptr<std::byte, decltype(&std::free)> memory{nullptr, &std::free};
    Page(std::uint64_t guestAddress, std::size_t bytes = 4096) : address(guestAddress) {
        const auto pageSize = sysconf(_SC_PAGESIZE);
        Require(pageSize > 0, "Cannot determine shared guest host page size");
        const auto alignment = static_cast<std::size_t>(pageSize);
        size = ((bytes + alignment - 1) / alignment) * alignment;
        void* pointer = nullptr;
        Require(posix_memalign(&pointer, alignment, size) == 0, "Shared guest page allocation failed");
        memory.reset(static_cast<std::byte*>(pointer));
        std::memset(memory.get(), 0, size);
    }
    std::span<std::byte> Bytes() { return {memory.get(), size}; }
};

struct WindowContext { CpuGuestMetalLayer* layer; };
void Size(void*, std::uint32_t* width, std::uint32_t* height) { *width = Width; *height = Height; }
void* Layer(void* context) { return (__bridge void*)static_cast<WindowContext*>(context)->layer; }

struct PresentationReceipt {
    WindowContext* window;
    id<MTLCommandQueue> queue;
    std::uint32_t count = 0;
};
void Presented(void* context) {
    auto& receipt = *static_cast<PresentationReceipt*>(context);
    Require(++receipt.count == 1, "Guest presentation completion repeated");
    id<CAMetalDrawable> drawable = receipt.window->layer.capturedDrawable;
    Require(drawable != nil, "Guest presentation did not acquire a real CAMetalDrawable");
    id<MTLTexture> texture = drawable.texture;
    Require(texture.width == Width && texture.height == Height, "Guest presented the wrong frame extent");
    constexpr NSUInteger rowBytes = Width * 4;
    id<MTLBuffer> readback = [texture.device newBufferWithLength:PixelBytes options:MTLResourceStorageModeShared];
    Require(readback != nil, "Independent drawable readback allocation failed");
    std::memset(readback.contents, 0x35, PixelBytes);
    id<MTLCommandBuffer> commands = [receipt.queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
              sourceSize:MTLSizeMake(Width, Height, 1) toBuffer:readback destinationOffset:0
       destinationBytesPerRow:rowBytes destinationBytesPerImage:PixelBytes];
    [blit endEncoding]; [commands commit]; [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted, "Independent drawable GPU readback failed");
    const auto* bytes = static_cast<const unsigned char*>(readback.contents);
    for (std::uint32_t i = 0; i < PixelBytes; ++i)
        Require(bytes[i] == 255, "Guest Metal drawable byte differs from independent full-white triangle expectation at " + std::to_string(i));
    receipt.window->layer.capturedDrawable = nil;
}

struct Session {
    NSWindow* window;
    WindowContext* context;
    ~Session() {
        try { AgcDriverReleaseWindow_nid_postfix(context); } catch (...) {}
        try { AgcDriverShutdown_nid_postfix(); } catch (...) {}
        [window orderOut:nil]; [window close];
    }
};

void Run(const char* fixture, id<MTLDevice> device, id<MTLLibrary> library) {
    std::array<Page, 8> pages{Page{0x100000}, Page{0x20f000, 0x4000}, Page{0x500000}, Page{0x600000},
        Page{0x700000}, Page{0x710000}, Page{0xa00000}, Page{0xb00000}};
    Cpu::Machine machine;
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges;
    for (auto& page : pages) {
        machine.MapBorrowed(page.address, page.Bytes(), Cpu::Permission::Read | Cpu::Permission::Write);
        ranges.push_back({page.address, page.Bytes(), page.address == 0x20f000 || page.address == 0xb00000});
    }
    Cpu::LinuxRuntime runtime(machine);
    auto image = Cpu::Load(machine, fixture);
    Cpu::SetupStack(machine, image, {fixture});
    machine.Set(Cpu::Register::Rsp, image.StackPointer);
    machine.Map(GateAddress, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    auto window = [[NSWindow alloc] initWithContentRect:NSMakeRect(40, 40, 256, 128)
                                            styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    window.releasedWhenClosed = NO;
    window.title = @"AnyPS5 x86 guest on native ARM64 CPU and Metal";
    auto layer = [CpuGuestMetalLayer layer];
    layer.device = device; layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.frame = window.contentView.bounds;
    window.contentView.wantsLayer = YES; window.contentView.layer = layer;
    AnyPS5::Host::ParkUnobtrusively(window);
    [window orderFront:nil]; [CATransaction flush];
    WindowContext context{layer};
    AgcDriver::PresentationWindow presentation{&context, {}, nullptr, Size, Width, Height, {}, Layer};
    PresentationReceipt receipt{&context, [device newCommandQueue]};
    Require(receipt.queue != nil, "Independent drawable readback queue allocation failed");
    std::atomic<std::uint32_t> interrupts{0};
    const auto checkPixels = [&] {
        const auto bytes = pages[1].Bytes();
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto expected = i >= 4096 && i < 4096 + PixelBytes ? std::byte{255} : std::byte{0x7b};
            Require(bytes[i] == expected, "Guest Metal color copyback or allocation guard differs at " + std::to_string(i));
        }
    };
    Session session{window, &context};
    AgcDriver::Metal::MetalDriver::Get().Configure((__bridge void*)device, (__bridge void*)library, ranges,
        [&](std::uint32_t queue) {
            Require(queue == 0, "Guest EOP came from an unexpected queue");
            std::uint32_t label;
            std::memcpy(&label, pages[7].memory.get() + 512, sizeof(label));
            Require(label == 0x12345678, "Guest EOP arrived before its release marker was copied back");
            checkPixels();
            interrupts.fetch_add(1);
        });
    std::uint32_t registered = 0, submitted = 0, waited = 0;
    const auto checkCall = [](Cpu::Machine& cpu) {
        Require((cpu.Get(Cpu::Register::Rsp) & 15) == 8, "Guest host import violated SysV stack alignment");
    };
    machine.AddHostCall(GateAddress, [&](Cpu::Machine& cpu) {
        checkCall(cpu);
        const auto address = cpu.Get(Cpu::Register::Rdi);
        Require(registered < 2 && address == (registered ? 0x710000u : 0x700000u), "Guest shader import arguments/order were incorrect");
        cpu.CheckAccess(address, sizeof(Shader) + sizeof(ShaderUserData), Cpu::Permission::Read);
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(address));
        ++registered;
    });
    machine.AddHostCall(GateAddress + 0x10, [&](Cpu::Machine& cpu) {
        checkCall(cpu);
        const auto address = cpu.Get(Cpu::Register::Rdi);
        const auto queue = cpu.Get(Cpu::Register::Rsi);
        Require(registered == 2 && submitted == 0 && address == 0xb00000 && queue == 0, "Guest Submit import arguments/order were incorrect");
        cpu.CheckAccess(address, sizeof(Packet), Cpu::Permission::Read);
        AgcDriver::Submit(reinterpret_cast<const Packet*>(address), static_cast<std::uint32_t>(queue));
        ++submitted;
    });
    machine.AddHostCall(GateAddress + 0x20, [&](Cpu::Machine& cpu) {
        checkCall(cpu);
        Require(submitted == 1 && waited == 0, "Guest WaitIdle import order was incorrect");
        AgcDriverWaitIdle_nid_postfix();
        Require(interrupts.load() == 1, "Guest draw did not deliver exactly one completed EOP interrupt");
        checkPixels(); ++waited;
    });
    machine.AddHostCall(GateAddress + 0x30, [&](Cpu::Machine& cpu) {
        checkCall(cpu);
        Require(waited == 1 && cpu.Get(Cpu::Register::Rdi) == 0xb00400, "Guest Present import arguments/order were incorrect");
        AgcDriver::DisplayBuffer buffer{};
        cpu.Read(cpu.Get(Cpu::Register::Rdi), std::as_writable_bytes(std::span(&buffer, 1)));
        Require(buffer.address == ColorAddress && buffer.width == Width && buffer.height == Height &&
            buffer.pixelFormat == 0x8000000022000000ull && buffer.tilingMode == 1 && buffer.pitchInPixel == Width &&
            buffer.dccAddress == 0 && buffer.dccClearColor == 0, "Guest authored an invalid display descriptor");
        AgcDriverPresentBuffer_nid_postfix(presentation, buffer, Presented, &receipt);
    });
    Require(machine.Run(image.Entry, 0, 2000000) == Cpu::StopReason::Exit, "Guest did not exit from the real ELF entry point");
    Require(machine.ExitCode() == 0, "Guest observed incorrect GPU output: exit " + std::to_string(machine.ExitCode()));
    Require(registered == 2 && submitted == 1 && waited == 1 && receipt.count == 1 && interrupts.load() == 1,
        "Guest omitted an AGC submission, completion, interrupt or presentation");
    checkPixels();
    constexpr std::array<float, 12> triangle{-1, -1, .5f, 1, 3, -1, .5f, 1, -1, 3, .5f, 1};
    Require(std::memcmp(pages[0].memory.get(), triangle.data(), sizeof(triangle)) == 0, "Guest vertex input changed during draw/presentation");
    AgcDriverReleaseWindow_nid_postfix(&context);
    AgcDriverShutdown_nid_postfix();
    std::cout << "PASS: real x86-64 ELF entry executed via native ARM64 translation; guest-authored PM4/VS/PS/vertices; SysV AGC imports; "
              << "64x32 full-white Metal triangle copyback with complete allocation guards; completed EOP; guest-observed pixels; "
              << "actual CAMetalDrawable independently read back as 8192 white bytes; exit=0\n";
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 3, "CPU Metal guest test requires x86 ELF and utility metallib paths");
            const auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "CPU Metal guest fixture requires Metal 3");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]] error:&error];
            Require(library != nil, std::string("Utility metallib load failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
            std::cout << "CPU translation backend: " << Cpu::Machine::Backend() << "; Metal device: " << device.name.UTF8String << '\n';
            Run(argv[1], device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
