#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Black title frames: a 4-fragment color target renders into a resident 2D multisample texture
// that never reaches guest memory, and the title resolves it with a compute dispatch that reads
// the same address through a 2D MSAA image descriptor. Dispatches bound no resident surfaces, so
// that resolve was skipped every frame and everything downstream of it stayed black.
//
// One graphics command buffer through the public MetalDriver: a draw covers the lower-left half
// of an 8x8 4x RGBA8 target with white, then a wave32 dispatch image_loads chosen (x, y, sample)
// fragments of that address and stores them. Covered fragments must read 1.0, uncovered ones the
// surface's cleared 0.0, and neither the draw nor the dispatch may be skipped.
namespace {
using AgcDriver::NativeGuestMemory::BorrowedRange;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Edge = 8, Samples = 4, Lanes = 32, Sentinel = 0xcafebeefu;
constexpr std::uint64_t VertexAddress = 0x100000, ColorAddress = 0x200000;
constexpr std::uint64_t VertexCodeAddress = 0x500000, PixelCodeAddress = 0x600000;
constexpr std::uint64_t VertexHeaderAddress = 0x700000, PixelHeaderAddress = 0x710000, ComputeHeaderAddress = 0x720000;
constexpr std::uint64_t ComputeCodeAddress = 0x800000, InputAddress = 0x810000, OutputAddress = 0x820000;
constexpr std::uint64_t LabelAddress = 0x900000, CommandAddress = 0xa00000;

// buffer_load_format_xyzw v[0:3] from s[8:11] by vertex index; export position; end.
constexpr std::array<std::uint32_t, 6> VertexCode{0xe0382000, 0x80020005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000};
// v7 = 1.0; export mrt0 (v7, v7, v7, v7) done; end.
constexpr std::array<std::uint32_t, 4> PixelCode{0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000};
// v1 = v3 = lane * 16; buffer_load_dwordx4 v[4:7] (x, y, sample, sentinel) from s[0:3];
// v10..v13 = v7; image_load v10, v[4:6] dmask 1 dim 2D_MSAA from s[8:15]; store v[10:13] to s[4:7].
constexpr std::array<std::uint32_t, 15> ComputeCode{0x34020084, 0x34060084, 0xe0381000, 0x80000401, 0xbf8c3f70,
    0x7e140307, 0x7e160307, 0x7e180307, 0x7e1a0307, 0xf0001130, 0x00020a04, 0xbf8c3f70,
    0xe0781000, 0x80010a03, 0xbf810000};

template<std::size_t Bytes>
struct alignas(4096) Region {
    std::array<std::byte, Bytes> bytes{};
    std::span<std::byte> Span() { return bytes; }
    template<typename T> T* As() { return reinterpret_cast<T*>(bytes.data()); }
};

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

void Append(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    words.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    words.insert(words.end(), payload.begin(), payload.end());
}

void Registers(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::span<const std::uint32_t> values) {
    words.push_back(0xc0000000u | (static_cast<std::uint32_t>(values.size()) << 16u) | (opcode << 8u));
    words.push_back(offset);
    words.insert(words.end(), values.begin(), values.end());
}

void Register(std::vector<std::uint32_t>& words, std::uint32_t opcode, std::uint32_t offset, std::uint32_t value) {
    Registers(words, opcode, offset, std::span(&value, 1));
}

std::uint32_t Float(float value) { return std::bit_cast<std::uint32_t>(value); }

// The draw: 8x8 viewport, one 4-sample 4-fragment RGBA8 target in CB slot 0, no depth.
void Draw(std::vector<std::uint32_t>& words) {
    Register(words, 0x79, 0x242, 4);
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x00402000}, {0x1b6, 0x8000}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 2}, {0x292, 3}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240}, {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028}, {0x31b, 0},
        // CB_COLOR0_ATTRIB NUM_SAMPLES and NUM_FRAGMENTS = 4 (log2 2).
        {0x31d, (2u << 12u) | (2u << 15u)},
        {0x3b0, ((Edge - 1) << 14u) | (Edge - 1)}, {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, (Edge << 16u) | Edge}, {0x81, 0x80000000}, {0x82, (Edge << 16u) | Edge},
        {0x90, 0x80000000}, {0x91, (Edge << 16u) | Edge}, {0x94, 0x80000000}, {0x95, (Edge << 16u) | Edge},
        {0x318, static_cast<std::uint32_t>(ColorAddress >> 8u)}, {0x390, 0},
        {0x10f, Float(Edge / 2.0f)}, {0x110, Float(Edge / 2.0f)}, {0x111, Float(-(Edge / 2.0f))}, {0x112, Float(Edge / 2.0f)},
        {0x113, Float(1.0f)}, {0x114, 0}, {0xb4, 0}, {0xb5, Float(1.0f)},
        // SPI_PS_INPUT_ENA/ADDR: PositionX and PositionY.
        {0x1b3, 0x300}, {0x1b4, 0x300}};
    for (const auto& [offset, value] : context) Register(words, 0x69, offset, value);
    const std::array<std::uint32_t, 2> vertexProgram{static_cast<std::uint32_t>(VertexCodeAddress >> 8u), 0};
    const std::array<std::uint32_t, 2> pixelProgram{static_cast<std::uint32_t>(PixelCodeAddress >> 8u), 0};
    const std::array<std::uint32_t, 4> vertexUsers{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 3, 0x01016fac};
    Registers(words, 0x76, 0xc8, vertexProgram);
    Registers(words, 0x76, 0x008, pixelProgram);
    Register(words, 0x76, 0x08b, 8);
    Register(words, 0x76, 0x00b, 0);
    Registers(words, 0x76, 0x08c, vertexUsers);
    Register(words, 0x79, 0x24a, 0);
    Append(words, 0x2f, {1});
    Append(words, 0x2d, {3, 2});
}

// The resolve-shaped dispatch: one wave32 group reading the target through a 2D MSAA descriptor.
void Dispatch(std::vector<std::uint32_t>& words) {
    constexpr std::uint32_t Width = Edge - 1, Height = Edge - 1;
    const std::array<std::uint32_t, 16> users{
        static_cast<std::uint32_t>(InputAddress), 0, Lanes * 16, 0x31016fac,
        static_cast<std::uint32_t>(OutputAddress), 0, Lanes * 16, 0x31016fac,
        // 8_8_8_8 UNORM, 8x8, linear, type 14 (2D MSAA) with LAST_LEVEL = MAX_MIP = log2(samples).
        static_cast<std::uint32_t>(ColorAddress >> 8u), (56u << 20u) | ((Width & 3u) << 30u), (Width >> 2u) | (Height << 14u),
        0xfacu | (2u << 16u) | (14u << 28u), 0, 2u << 4u, 0, 0};
    const std::array<std::uint32_t, 3> threads{Lanes, 1, 1};
    const std::array<std::uint32_t, 2> program{static_cast<std::uint32_t>(ComputeCodeAddress >> 8u), 0};
    Registers(words, 0x76, 0x207, threads);
    Registers(words, 0x76, 0x20c, program);
    Register(words, 0x76, 0x213, static_cast<std::uint32_t>(users.size()) << 1u);
    Registers(words, 0x76, 0x240, users);
    Append(words, 0x15, {1, 1, 1, 0x8041});
}

// Fragment centers well inside or outside the triangle (screen (0,0), (0,8), (8,8): x < y covered).
struct Probe { std::uint32_t x, y, sample; bool covered; };
constexpr std::array<Probe, 8> Probes{{
    {0, 7, 0, true}, {1, 6, 3, true}, {2, 7, 1, true}, {0, 3, 2, true},
    {7, 0, 0, false}, {6, 1, 3, false}, {7, 2, 1, false}, {3, 0, 2, false}}};
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        AgcDriver::Metal::MetalDriver driver;
        try {
            require(argc == 2, "usage: anyps5_metal_resident_compute_test <utility metallib>");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "Metal device unavailable");
            require([device supportsTextureSampleCount:Samples], "Metal device lacks 4x multisampling");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
            require(library != nil, "utility metallib load failed");

            static Region<4096> vertices, color, vertexCode, pixelCode, computeCode, input, output, labels;
            static Region<4096> vertexHeader, pixelHeader, computeHeader;
            static Region<65536> commands;
            const std::array<std::array<float, 4>, 3> triangle{{{-1, -1, .5f, 1}, {1, -1, .5f, 1}, {-1, 1, .5f, 1}}};
            std::memcpy(vertices.bytes.data(), triangle.data(), sizeof(triangle));
            std::memcpy(vertexCode.bytes.data(), VertexCode.data(), sizeof(VertexCode));
            std::memcpy(pixelCode.bytes.data(), PixelCode.data(), sizeof(PixelCode));
            std::memcpy(computeCode.bytes.data(), ComputeCode.data(), sizeof(ComputeCode));
            const auto header = [](auto& region, auto bytes) { std::memcpy(region.bytes.data(), bytes.data(), bytes.size()); };
            header(vertexHeader, Header(VertexHeaderAddress, VertexCodeAddress, sizeof(VertexCode), 2));
            header(pixelHeader, Header(PixelHeaderAddress, PixelCodeAddress, sizeof(PixelCode), 1));
            header(computeHeader, Header(ComputeHeaderAddress, ComputeCodeAddress, sizeof(ComputeCode), 0));
            auto* rows = input.As<std::uint32_t>();
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                const auto& probe = Probes[lane % Probes.size()];
                rows[lane * 4 + 0] = probe.x;
                rows[lane * 4 + 1] = probe.y;
                rows[lane * 4 + 2] = probe.sample;
                rows[lane * 4 + 3] = Sentinel;
            }
            auto* results = output.As<std::uint32_t>();
            std::fill_n(results, output.bytes.size() / 4, 0xdeadbeefu);

            std::vector<std::uint32_t> words;
            Draw(words);
            Dispatch(words);
            Append(words, 0x49, {0, (1u << 29u) | (1u << 24u), static_cast<std::uint32_t>(LabelAddress), 0, 1, 0, 0});
            require(words.size() * 4 <= commands.bytes.size(), "command buffer overflows its region");
            std::memcpy(commands.bytes.data(), words.data(), words.size() * 4);

            const std::vector<BorrowedRange> ranges{
                {VertexAddress, vertices.Span(), false}, {ColorAddress, color.Span(), true},
                {VertexCodeAddress, vertexCode.Span(), false}, {PixelCodeAddress, pixelCode.Span(), false},
                {VertexHeaderAddress, vertexHeader.Span(), false}, {PixelHeaderAddress, pixelHeader.Span(), false},
                {ComputeHeaderAddress, computeHeader.Span(), false}, {ComputeCodeAddress, computeCode.Span(), false},
                {InputAddress, input.Span(), false}, {OutputAddress, output.Span(), true},
                {LabelAddress, labels.Span(), true}, {CommandAddress, commands.Span(), false}};
            driver.Configure((__bridge void*)device, (__bridge void*)library, ranges, [](std::uint32_t) {});
            driver.RegisterShader(reinterpret_cast<const Shader*>(VertexHeaderAddress));
            driver.RegisterShader(reinterpret_cast<const Shader*>(PixelHeaderAddress));
            driver.RegisterShader(reinterpret_cast<const Shader*>(ComputeHeaderAddress));
            driver.SubmitCommandBuffer(CommandAddress, static_cast<std::uint32_t>(words.size()), 0, 0);
            driver.WaitIdle();

            const auto skipped = driver.SkippedWork();
            std::string reasons;
            for (const auto& reason : skipped.reasons) reasons += " [" + reason.kind + ": " + reason.what + "]";
            require(skipped.draws == 0 && skipped.dispatches == 0, "the MSAA draw or its resolve dispatch was skipped:" + reasons);
            require(labels.As<std::uint32_t>()[0] == 1, "the command buffer's EOP label did not land");
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                const auto& probe = Probes[lane % Probes.size()];
                const auto wanted = Float(probe.covered ? 1.0f : 0.0f);
                const auto at = "lane " + std::to_string(lane) + " fragment (" + std::to_string(probe.x) + ", " +
                    std::to_string(probe.y) + ", sample " + std::to_string(probe.sample) + ")";
                require(results[lane * 4] == wanted, at + " read " + std::to_string(results[lane * 4]) + ", wanted " + std::to_string(wanted));
                for (std::uint32_t word = 1; word < 4; ++word) require(results[lane * 4 + word] == Sentinel, at + " changed a neighboring result word");
            }
            require(std::all_of(results + Lanes * 4, results + output.bytes.size() / 4, [](auto value) { return value == 0xdeadbeefu; }),
                "the dispatch wrote past its output");
            driver.Shutdown();
            std::cout << "PASS: a compute dispatch reads the fragments a draw rendered into a resident 4x color surface\n";
            return 0;
        } catch (const std::exception& error) {
            try { driver.Shutdown(); } catch (...) {}
            std::cerr << "FAIL: " << error.what() << '\n';
            return 1;
        }
    }
}
