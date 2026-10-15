#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <sys/mman.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The guest-visible contract of lazy render-target copy-backs (MetalLazyCopyBacks.hpp), through
// the public driver: a draw renders a guest color target, and the rendered bytes must be in guest
// memory whenever someone outside the GPU work after it can look:
// - the guest CPU, once a label the submission stores after the draw becomes visible (while the
//   submission is still running, so nothing but the label orders the copy-back), and in the
//   end-of-pipe interrupt;
// - a DMA_DATA copy of the target's bytes later in the same submission;
// - a compute dispatch that reads the target's bytes as a buffer.
// Run with ANYPS5_METAL_BATCH=0 too: every work then has its own command buffer, so a label that
// were not ordered behind the copy-back would land before it.
namespace {

constexpr std::uint32_t Width = 64, Height = 32, TargetBytes = Width * Height * 4;
constexpr std::uint64_t VertexAddress = 0x100000, ColorAllocation = 0x200000, ColorAddress = ColorAllocation + 256;
constexpr std::uint64_t LabelAddress = 0x900000, DmaAddress = 0x910000, ComputeCodeAddress = 0x800000, ComputeOutputAddress = 0x820000;
constexpr std::uint64_t CommandAddress = 0xa00000, PacketAddress = 0x8000000;
constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000};
constexpr std::array<std::uint32_t, 4> FullPixelCode{0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000};
// output[i * 4] = input[i * 4] + 1 for 64 threads (two buffer V#s in user data 0-7).
constexpr std::array<std::uint32_t, 17> ComputeCode{
    0x34020082, 0xe0302000, 0x80000401, 0xbf8c3f70, 0x7e000000, 0x7e003600, 0x7e008200, 0x4a080881,
    0xd5800000, 0x00000000, 0xd59b0000, 0x00000000, 0xd5c10000, 0x00000000, 0xe0702000, 0x80010401, 0xbf810000};
constexpr std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

void RegisterPacket(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::uint32_t offset, std::span<const std::uint32_t> words) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(words.size()) << 16u) | (opcode << 8u));
    commands.push_back(offset);
    commands.insert(commands.end(), words.begin(), words.end());
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload.begin(), payload.end());
}

// The state of a full-target draw of 255s into the linear RGBA8 target at ColorAddress, and the draw.
void Draw(std::vector<std::uint32_t>& commands) {
    const std::array<std::uint32_t, 1> primitive{4};
    RegisterPacket(commands, 0x79, 0x242, primitive);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x00402000}, {0x1b6, 0x8000}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240}, {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028}, {0x31b, 0}, {0x31d, 0},
        {0x3b0, ((Width - 1) << 14u) | (Height - 1)}, {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, (Height << 16u) | Width}, {0x81, 0x80000000}, {0x82, (Height << 16u) | Width},
        {0x90, 0x80000000}, {0x91, (Height << 16u) | Width}, {0x94, 0x80000000}, {0x95, (Height << 16u) | Width},
        {0x318, static_cast<std::uint32_t>(ColorAddress >> 8u)}, {0x390, 0},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-16.0f)}, {0x112, std::bit_cast<std::uint32_t>(16.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x1b3, ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) |
                    ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY)},
        {0x1b4, ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) |
                    ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY)}};
    for (const auto& [offset, value] : context) RegisterPacket(commands, 0x69, offset, std::span(&value, 1));
    const std::array<std::uint32_t, 2> vertexProgram{0x500000u >> 8u, 0}, fragmentProgram{0x600000u >> 8u, 0};
    const std::array<std::uint32_t, 1> vertexResources{8}, fragmentResources{0};
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 3, 0x01016fac};
    RegisterPacket(commands, 0x76, 0xc8, vertexProgram);
    RegisterPacket(commands, 0x76, 0x008, fragmentProgram);
    RegisterPacket(commands, 0x76, 0x08b, vertexResources);
    RegisterPacket(commands, 0x76, 0x00b, fragmentResources);
    RegisterPacket(commands, 0x76, 0x08c, userData);
    Append(commands, 0x2d, {3, 2});
}

// RELEASE_MEM of a 32-bit `value` to `address`, raising the end-of-pipe interrupt when `interrupt`.
void Label(std::vector<std::uint32_t>& commands, std::uint64_t address, std::uint32_t value, bool interrupt = false) {
    Append(commands, 0x49, {0, (1u << 29u) | (interrupt ? 1u << 24u : 0u), static_cast<std::uint32_t>(address), 0, value, 0, 0});
}

std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> Header(std::uint64_t address, std::uint64_t code, std::uint32_t bytes, std::uint8_t type) {
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> data{};
    Shader header{};
    header.file_header = 0x34333231;
    header.version = 0x18;
    header.user_data = reinterpret_cast<ShaderUserData*>(address + sizeof(Shader));
    header.code = reinterpret_cast<const volatile void*>(code);
    header.header_size = static_cast<std::uint32_t>(data.size());
    header.shader_size = bytes;
    header.type = type;
    std::memcpy(data.data(), &header, sizeof(header));
    return data;
}

// Guest memory is one page-aligned host mapping, guest address = Base + host offset, as the game's
// is: copy-backs defer only into no-copy aliases of the guest bytes, and a snapshot that has to
// copy any range makes the work complete synchronously.
struct Guest {
    static constexpr std::uint64_t Base = 0x100000, Bytes = 0x8000000;
    std::byte* host = nullptr;
    template <typename T>
    std::span<T> At(std::uint64_t address, std::size_t count) {
        Require(address >= Base && address - Base + count * sizeof(T) <= Bytes, "lazy copy-back guest layout does not fit");
        return {reinterpret_cast<T*>(host + (address - Base)), count};
    }
    std::span<std::array<float, 4>> vertices;
    std::span<std::byte> pixels, vertexHeader, fragmentHeader, computeHeader, dma;
    std::span<std::uint32_t> vertexCode, fragmentCode, computeCode, labels, output, commands;
    std::span<::Packet> packets;

    Guest() {
        void* mapping = ::mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        Require(mapping != MAP_FAILED, "lazy copy-back guest mapping failed");
        host = static_cast<std::byte*>(mapping);
        vertices = At<std::array<float, 4>>(VertexAddress, 65536);
        std::fill(vertices.begin(), vertices.end(), std::array<float, 4>{8, 8, 0.5f, 1});
        std::copy(Triangle.begin(), Triangle.end(), vertices.begin());
        pixels = At<std::byte>(ColorAllocation, 256 + TargetBytes + 256);
        std::fill(pixels.begin(), pixels.end(), std::byte{0x7b});
        vertexCode = At<std::uint32_t>(0x500000, 9);
        std::copy(VertexCode.begin(), VertexCode.end(), vertexCode.begin());
        vertexCode[1] = 0x80020005;
        fragmentCode = At<std::uint32_t>(0x600000, 32);
        std::copy(FullPixelCode.begin(), FullPixelCode.end(), fragmentCode.begin());
        computeCode = At<std::uint32_t>(ComputeCodeAddress, ComputeCode.size());
        std::copy(ComputeCode.begin(), ComputeCode.end(), computeCode.begin());
        const auto header = [&](std::uint64_t address, std::uint64_t code, std::uint32_t bytes, std::uint8_t type) {
            const auto data = Header(address, code, bytes, type);
            auto span = At<std::byte>(address, data.size());
            std::copy(data.begin(), data.end(), span.begin());
            return span;
        };
        vertexHeader = header(0x700000, 0x500000, sizeof(VertexCode), 2);
        fragmentHeader = header(0x710000, 0x600000, sizeof(FullPixelCode), 1);
        computeHeader = header(0x720000, ComputeCodeAddress, sizeof(ComputeCode), 0);
        labels = At<std::uint32_t>(LabelAddress, 16);
        dma = At<std::byte>(DmaAddress, TargetBytes);
        output = At<std::uint32_t>(ComputeOutputAddress, 64 * 4);
        commands = At<std::uint32_t>(CommandAddress, 15u << 20u);
        packets = At<::Packet>(PacketAddress, 1);
    }

    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> Ranges() {
        return {{VertexAddress, std::as_writable_bytes(vertices), false}, {ColorAllocation, pixels, true},
            {0x500000, std::as_writable_bytes(vertexCode), false}, {0x600000, std::as_writable_bytes(fragmentCode), false},
            {0x700000, vertexHeader, false}, {0x710000, fragmentHeader, false}, {0x720000, computeHeader, false},
            {ComputeCodeAddress, std::as_writable_bytes(computeCode), false},
            {ComputeOutputAddress, std::as_writable_bytes(output), true},
            {LabelAddress, std::as_writable_bytes(labels), true}, {DmaAddress, dma, true},
            {CommandAddress, std::as_writable_bytes(commands), false},
            {PacketAddress, std::as_writable_bytes(packets), false}};
    }

    // Starts the submission of `words` on queue 0, with a fresh target (0x40) and labels.
    void Submit(AgcDriver::Metal::MetalDriver& driver, const std::vector<std::uint32_t>& words) {
        Require(words.size() <= commands.size(), "lazy copy-back command words do not fit");
        std::fill(commands.begin(), commands.end(), 0x80000000u);
        std::copy(words.begin(), words.end(), commands.begin());
        std::fill_n(pixels.begin() + 256, TargetBytes, std::byte{0x40});
        for (auto& label : labels) std::atomic_ref(label).store(0, std::memory_order_release);
        packets[0] = {reinterpret_cast<std::uint32_t*>(CommandAddress), static_cast<std::uint32_t>(words.size()), 0, {}};
        driver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0);
    }

    [[nodiscard]] static bool Rendered(std::span<const std::byte> bytes) {
        return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{255}; });
    }
    [[nodiscard]] bool Rendered() const { return Rendered(pixels.subspan(256, TargetBytes)); }
};

std::uint32_t LoadLabel(const std::uint32_t& label) { return std::atomic_ref(const_cast<std::uint32_t&>(label)).load(std::memory_order_acquire); }

void Run(id<MTLDevice> device, id<MTLLibrary> library) {
    Guest guest;
    std::atomic<std::uint32_t> interrupts{0}, staleInterrupts{0};
    AgcDriver::Metal::MetalDriver driver;
    driver.Configure((__bridge void*)device, (__bridge void*)library, guest.Ranges(), [&](std::uint32_t) {
        // The end-of-pipe interrupt is guest-visible: the target's bytes must be there.
        if (!guest.Rendered()) staleInterrupts.fetch_add(1);
        interrupts.fetch_add(1);
    });
    driver.RegisterShader(reinterpret_cast<const Shader*>(0x700000));
    driver.RegisterShader(reinterpret_cast<const Shader*>(0x710000));
    driver.RegisterShader(reinterpret_cast<const Shader*>(0x720000));
    const char* batch = std::getenv("ANYPS5_METAL_BATCH");
    const bool separate = batch != nullptr && batch[0] == '0';

    {
        // 1. The guest CPU polls a label stored after the draw, then reads the target. Register
        // packets keep the submission running after the label, so the copy-back is
        // ordered only by the label (the submission's retirement comes much later).
        const auto words = [](int fillers) {
            std::vector<std::uint32_t> words;
            Draw(words);
            Label(words, LabelAddress, 1);
            const std::array<std::uint32_t, 1> filler{0};
            for (int i = 0; i < fillers; ++i) RegisterPacket(words, 0x76, 0x00b, filler);
            Label(words, LabelAddress + 4, 2, true);
            return words;
        };
        // The window (label visible, submission still running) depends on the GPU finishing the
        // draw within the filler packets; on a loaded GPU it can close, so up to 5 attempts, each
        // with twice the fillers (300,000 to 4.8 million).
        int conclusive = 0, attempts = 0;
        for (; attempts < 5 && conclusive == 0; ++attempts) {
            std::vector<std::byte> seen;
            bool late = false;
            const auto interruptsBefore = interrupts.load();
            // Before the reader starts: it must not see the previous attempt's labels.
            for (auto& label : guest.labels) std::atomic_ref(label).store(0, std::memory_order_release);
            std::thread reader([&] {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (LoadLabel(guest.labels[0]) != 1 && std::chrono::steady_clock::now() < deadline) {}
                seen.assign(guest.pixels.begin() + 256, guest.pixels.begin() + 256 + TargetBytes);
                late = LoadLabel(guest.labels[1]) == 2;
            });
            guest.Submit(driver, words(300000 << attempts));
            reader.join();
            driver.WaitIdle();
            Require(LoadLabel(guest.labels[0]) == 1 && LoadLabel(guest.labels[1]) == 2, "the submission's labels were not stored");
            Require(guest.Rendered(seen), "the guest CPU read the target after its label and saw stale bytes");
            Require(interrupts.load() == interruptsBefore + 1 && staleInterrupts.load() == 0, "the end-of-pipe interrupt saw stale target bytes");
            Require(guest.Rendered(), "the target's bytes are stale after the submission retired");
            if (!late) ++conclusive;
        }
        // With one command buffer per work the label lands while the submission still runs; batched,
        // the work commits at its end, so the window may stay closed (the checks above still hold).
        if (separate) Require(conclusive != 0, "the label never became visible before its submission ended: the check proves nothing");
        std::cout << "lazy copy-back: guest CPU read after a label passed (" << conclusive << " of " << attempts << " attempts inside the window)\n";
    }
    {
        // 2. A DMA_DATA copy of the target's bytes behind the draw.
        std::vector<std::uint32_t> words;
        Draw(words);
        Append(words, 0x50, {0, static_cast<std::uint32_t>(ColorAddress), 0, static_cast<std::uint32_t>(DmaAddress), 0, TargetBytes});
        Label(words, LabelAddress, 3);
        std::fill(guest.dma.begin(), guest.dma.end(), std::byte{0x11});
        guest.Submit(driver, words);
        driver.WaitIdle();
        Require(guest.Rendered(guest.dma), "DMA_DATA copied stale target bytes");
        std::cout << "lazy copy-back: DMA copy passed\n";
    }
    {
        // 3. A compute dispatch reads the target's bytes through a buffer: output = input + 1.
        std::vector<std::uint32_t> words;
        Draw(words);
        const std::array<std::uint32_t, 3> threads{64, 1, 1};
        const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(ComputeCodeAddress >> 8u), 0};
        const std::array<std::uint32_t, 1> resources{16};
        const std::array<std::uint32_t, 8> users{static_cast<std::uint32_t>(ColorAddress), 4u << 16u, 256, 0x01016fac,
            static_cast<std::uint32_t>(ComputeOutputAddress), 4u << 16u, 256, 0x01016fac};
        RegisterPacket(words, 0x76, 0x207, threads);
        RegisterPacket(words, 0x76, 0x20c, program);
        RegisterPacket(words, 0x76, 0x213, resources);
        RegisterPacket(words, 0x76, 0x240, users);
        Append(words, 0x15, {1, 1, 1, 0x8041});
        Label(words, LabelAddress, 4);
        std::fill(guest.output.begin(), guest.output.end(), 0xdeadbeefu);
        guest.Submit(driver, words);
        driver.WaitIdle();
        for (std::uint32_t i = 0; i < guest.output.size(); ++i) {
            const auto expected = i % 4 == 0 ? 0u : 0xdeadbeefu;  // 0xffffffff + 1
            Require(guest.output[i] == expected, "a compute dispatch reading the target as a buffer saw stale bytes (word " + std::to_string(i) +
                " is " + std::to_string(guest.output[i]) + ")");
        }
        std::cout << "lazy copy-back: compute buffer read passed\n";
    }
    driver.Shutdown();
}

}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 2, "lazy copy-back replay requires the compiled utility metallib path");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "lazy copy-back replay requires a Metal device");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, std::string("lazy copy-back utility library load failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
            Run(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
