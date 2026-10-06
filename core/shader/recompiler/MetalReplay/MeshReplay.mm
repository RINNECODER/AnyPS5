#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "SceShaders.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
constexpr std::uint32_t Width = 192, Height = 128;
constexpr std::uint64_t VertexAddress = 0x300000, ColorAddress = 0x200100, IndexAddress = 0x400000;
constexpr std::array<std::uint8_t, 4> Background{16, 24, 40, 255};
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
alignas(256) constexpr std::array<std::uint32_t, 104> GeometryCode{
    0x8f6a9003, 0x94fe6ac1, 0xbf88000b, 0xd7650006, 0x000100c1, 0xd7660006, 0x00020cc1, 0x93ebff03,
    0x00040018, 0xd7460006, 0x04190c6b, 0x340c0c82, 0xd8340000, 0x00000506, 0xbf8cc07f, 0xbefe04c1,
    0xbf8a0000, 0x938dff02, 0x00090016, 0x9382ff03, 0x00040018, 0x938cff03, 0x00080008, 0xd7650009,
    0x000100c1, 0xd7660009, 0x000212c1, 0xd746000a, 0x04250c02, 0x7da8120c, 0xbf88002e, 0x361600ff,
    0x0000ffff, 0x2c180090, 0x361a02ff, 0x0000ffff, 0xd8d80000, 0x0b00000b, 0xd8d80000, 0x0c00000c,
    0xd8d80000, 0x0d00000d, 0xbf8cc07f, 0xe0382000, 0x8002100b, 0xe0382010, 0x8002140b, 0xe0382000,
    0x8002180c, 0xe0382010, 0x80021c0c, 0xe0382000, 0x8002200d, 0xe0382010, 0x8002240d, 0x161c1483,
    0x161e14ff, 0x00000060, 0xbf8c3f70, 0xdb7c0400, 0x0000100f, 0xdb7c0410, 0x0000140f, 0xdb7c0420,
    0x0000180f, 0xdb7c0430, 0x00001c0f, 0xdb7c0440, 0x0000200f, 0xdb7c0450, 0x0000240f, 0x4a501c81,
    0x4a521c82, 0x3450508a, 0x34525294, 0xd772002a, 0x04a6510e, 0xbf8cc07f, 0xbefe04c1, 0xbf8a0000,
    0x930e830d, 0xbf078002, 0xbf850003, 0x8f0f8c0d, 0x887c0f0e, 0xbf900009, 0x7da8140d, 0xbf880002,
    0xf8000941, 0x0000002a, 0xbefe04c1, 0x7da8140e, 0xbf88000a, 0x34561485, 0xdbfc0400, 0x2c00002b,
    0xdbfc0410, 0x3000002b, 0xbf8cc07f, 0xf80008cf, 0x2f2e2d2c, 0xf800020f, 0x33323130, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

constexpr std::uint32_t Columns = 8;
constexpr std::uint32_t Rows = 5;
constexpr std::uint32_t Triangles = Columns * Rows;

std::array<std::uint8_t, 4> TriangleColor(std::uint32_t triangle) {
    return {static_cast<std::uint8_t>(40u + 29u * triangle), static_cast<std::uint8_t>(200u + 53u * triangle), static_cast<std::uint8_t>(triangle % 2u == 0u ? 60u : 200u), 255};
}

std::array<Vertex, 3> TriangleVertices(std::uint32_t triangle) {
    const float cellWidth = 2.0f / Columns;
    const float cellHeight = 2.0f / Rows;
    const float x = -1.0f + cellWidth * static_cast<float>(triangle % Columns);
    const float y = -1.0f + cellHeight * static_cast<float>(triangle / Columns);
    const auto color = TriangleColor(triangle);
    const std::array<float, 4> rgba{color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f};
    return {{
        {{x + 0.1f * cellWidth, y + 0.1f * cellHeight, 0.5f, 1.0f}, rgba},
        {{x + 0.9f * cellWidth, y + 0.1f * cellHeight, 0.5f, 1.0f}, rgba},
        {{x + 0.5f * cellWidth, y + 0.9f * cellHeight, 0.5f, 1.0f}, rgba},
    }};
}

std::size_t CellPixel(std::uint32_t triangle) {
    const auto column = triangle % Columns;
    const auto row = triangle / Columns;
    const auto x = (column * Width + Width / 2u) / Columns;
    const auto y = Height - 1u - (row * Height + (Height * 4u) / 10u) / Rows;
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

void RegisterPacket(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::uint32_t offset,
                    std::span<const std::uint32_t> words) {
    std::vector<std::uint32_t> packet{0xc0000000u | (static_cast<std::uint32_t>(words.size()) << 16u) | (opcode << 8u), offset};
    packet.insert(packet.end(), words.begin(), words.end());
    commands.insert(commands.end(), packet.begin(), packet.end());
}

std::vector<std::uint32_t> GraphicsCommands() {
    std::vector<std::uint32_t> commands;
    const std::array<std::uint32_t, 1> primitive{4};
    RegisterPacket(commands, 0x79, 0x242, primitive);
    const AgcDriver::Registers context{
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
        {0x10f, std::bit_cast<std::uint32_t>(96.0f)}, {0x110, std::bit_cast<std::uint32_t>(96.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-64.0f)}, {0x112, std::bit_cast<std::uint32_t>(64.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x1b3, PixelInputBit(PixelInput::PositionX) | PixelInputBit(PixelInput::PositionY)},
        {0x1b4, PixelInputBit(PixelInput::PositionX) | PixelInputBit(PixelInput::PositionY)}
    };
    for (const auto& [offset, value] : context) RegisterPacket(commands, 0x69, offset, std::span(&value, 1));
    const std::array<std::uint32_t, 2> vertexProgram{0x600000u >> 8u, 0}, fragmentProgram{0x900000u >> 8u, 0};
    const std::array<std::uint32_t, 1> vertexResources{8}, fragmentResources{0};
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(VertexAddress), 32u << 16u, 120, 0x01016fac};
    RegisterPacket(commands, 0x76, 0xc8, vertexProgram);
    RegisterPacket(commands, 0x76, 0x008, fragmentProgram);
    RegisterPacket(commands, 0x76, 0x08b, vertexResources);
    RegisterPacket(commands, 0x76, 0x00b, fragmentResources);
    RegisterPacket(commands, 0x76, 0x08c, userData);
    return commands;
}

void Replay(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::uint64_t CommandAddress = 0xb00000, PacketAddress = 0xb10000, ArgumentAddress = 0xb20000;
    std::array<Vertex, 120> vertices;
    std::array<std::uint16_t, 122> indices;
    indices.fill(0xffff);
    const auto populate = [&](bool scrambled, float depth = 0.5f) {
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            auto tri = TriangleVertices(triangle);
            for (std::uint32_t k = 0; k < 3; ++k) {
                tri[k].position[2] = depth;
                const auto slot = scrambled ? 3 * (Triangles - 1 - triangle) + (k + 1) % 3 : 3 * triangle + k;
                vertices[slot] = tri[k];
                indices[2 + 3 * triangle + k] = static_cast<std::uint16_t>(slot);
            }
        }
    };
    populate(true);
    auto geometry = GeometryCode;
    auto fragment = PixelCode;
    const auto header = [](std::uint64_t address, std::uint64_t code, std::uint32_t bytes, std::uint8_t type) {
        std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> data{};
        Shader shader{};
        shader.file_header = 0x34333231;
        shader.version = 0x18;
        shader.code = reinterpret_cast<const volatile void*>(code);
        shader.user_data = reinterpret_cast<ShaderUserData*>(address + sizeof(Shader));
        shader.header_size = static_cast<std::uint32_t>(data.size());
        shader.shader_size = bytes;
        shader.type = type;
        std::memcpy(data.data(), &shader, sizeof(shader));
        return data;
    };
    auto geometryHeader = header(0xa00000, 0x600000, sizeof(GeometryCode), 2);
    auto pixelHeader = header(0xa10000, 0x900000, sizeof(PixelCode), 1);
    std::array<std::uint32_t, 512> commands{};
    std::array<std::uint32_t, 5> arguments{};
    std::array<std::uint64_t, 36> queries;
    queries.fill(0x9192939495969798);
    ::Packet packet{reinterpret_cast<std::uint32_t*>(CommandAddress), 0, 0, {}};
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false},
        {ColorAddress - 256, pixels, true},
        {IndexAddress, std::as_writable_bytes(std::span(indices)), false},
        {0x600000, std::as_writable_bytes(std::span(geometry)), false},
        {0x900000, std::as_writable_bytes(std::span(fragment)), false},
        {0xa00000, geometryHeader, false}, {0xa10000, pixelHeader, false},
        {CommandAddress, std::as_writable_bytes(std::span(commands)), false},
        {PacketAddress, std::as_writable_bytes(std::span(&packet, 1)), false},
        {ArgumentAddress, std::as_writable_bytes(std::span(arguments)), false},
        {0xb30000, std::as_writable_bytes(std::span(queries)), true}};
    auto& driver = AgcDriver::Metal::MetalDriver::Get();
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0xa00000));
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0xa10000));
    const auto append = [](std::vector<std::uint32_t>& words, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
        words.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
        words.insert(words.end(), payload.begin(), payload.end());
    };
    const auto setup = [&](bool wide, bool positive, bool negativeDepth, std::uint32_t primitive = 4) {
        auto words = GraphicsCommands();
        const auto reg = [&](std::uint32_t bank, std::uint32_t offset, std::uint32_t value) { RegisterPacket(words, bank, offset, std::span(&value, 1)); };
        reg(0x69, 0x2d5, 0x2020);
        reg(0x69, 0x1b6, 1);
        reg(0x69, 0x191, 0x400);
        reg(0x69, 0x1b3, 0);
        reg(0x69, 0x1b4, 0);
        reg(0x69, 0x1ff, primitive == 4 ? (wide ? 96 : 12) : 9);
        reg(0x69, 0x2ce, 3);
        reg(0x69, 0x29b, 2);
        reg(0x69, 0x2ab, 4);
        reg(0x79, 0x242, primitive);
        reg(0x79, 0x25b, primitive == 4 ? ((wide ? 96u : 12u) << 9u) | (wide ? 32u : 4u) : (5u << 9u) | 3u);
        reg(0x76, 0x8a, 0x60000000);
        reg(0x76, 0x8b, (3u << 16u) | ((wide ? 16u : 8u) << 19u) | 8u);
        reg(0x69, 0x111, std::bit_cast<std::uint32_t>(positive ? 64.0f : -64.0f));
        reg(0x69, 0x204, negativeDepth ? 0u : 0x80000u);
        reg(0x69, 0x113, std::bit_cast<std::uint32_t>(negativeDepth ? 0.5f : 1.0f));
        reg(0x69, 0x114, std::bit_cast<std::uint32_t>(negativeDepth ? 0.5f : 0.0f));
        reg(0x79, 0x24a, 0);
        return words;
    };
    const auto submit = [&](const std::vector<std::uint32_t>& words, const std::string& name) {
        Require(words.size() <= commands.size(), "Mesh replay command allocation is too small");
        std::copy(words.begin(), words.end(), commands.begin());
        packet.dw_num = static_cast<std::uint32_t>(words.size());
        for (std::size_t pixel = 0; pixel < Width * Height; ++pixel)
            for (std::size_t c = 0; c < 4; ++c) pixels[256 + pixel * 4 + c] = std::byte{Background[c]};
        const auto savedVertices = vertices;
        const auto savedIndices = indices;
        const auto savedArguments = arguments;
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0);
        AgcDriverWaitIdle_nid_postfix();
        Require(std::memcmp(vertices.data(), savedVertices.data(), sizeof(vertices)) == 0 && indices == savedIndices && arguments == savedArguments,
            name + " changed borrowed mesh input");
        for (std::size_t i = 0; i < 256; ++i) Require(pixels[i] == std::byte{0x7b} && pixels[pixels.size() - 1 - i] == std::byte{0x7b}, name + " changed color allocation guard");
        Require(geometry == GeometryCode && fragment == PixelCode, name + " modified original RDNA programs");
    };
    const auto check = [&](const std::string& name, bool positive, std::uint32_t first = 0, std::uint32_t count = Triangles) {
        for (std::uint32_t triangle = 0; triangle < Triangles; ++triangle) {
            auto offset = CellPixel(triangle);
            if (positive) offset = ((Height - 1 - offset / (Width * 4)) * Width + (offset / 4) % Width) * 4;
            const auto expected = triangle >= first && triangle < first + count ? TriangleColor(triangle) : Background;
            for (std::size_t c = 0; c < 4; ++c)
                Require(pixels[256 + offset + c] == std::byte{expected[c]}, name + " wrong cell " + std::to_string(triangle) + " channel " + std::to_string(c));
        }
        for (std::size_t c = 0; c < 4; ++c) Require(pixels[256 + c] == std::byte{Background[c]} && pixels[256 + Width * Height * 4 - 4 + c] == std::byte{Background[c]}, name + " changed corner pixels");
        std::cout << name << " independent40cell colors passed\n";
    };
    for (const bool wide : {false, true}) {
        populate(true);
        auto indexed = setup(wide, false, false);
        append(indexed, 0x2a, {0});
        append(indexed, 0x27, {120, static_cast<std::uint32_t>(IndexAddress + 4), 0, 120, 0});
        const auto indexedName = std::string("Public geometry indexed ") + (wide ? "guest128/native64" : "guest64/native32");
        submit(indexed, indexedName);
        check(indexedName, false);
        populate(false, wide ? -0.5f : 0.5f);
        auto automatic = setup(wide, true, wide);
        append(automatic, 0x2d, {120, 2});
        const auto autoName = std::string("Public geometry auto positive viewport ") + (wide ? "negativeOneToOne" : "zeroToOne");
        submit(automatic, autoName);
        check(autoName, true);
    }
    populate(true);
    auto indirect = setup(true, false, false);
    append(indirect, 0x26, {static_cast<std::uint32_t>(IndexAddress + 4), 0});
    append(indirect, 0x2a, {0});
    append(indirect, 0x13, {120});
    append(indirect, 0x11, {1, static_cast<std::uint32_t>(ArgumentAddress), 0});
    arguments = {30, 1, 15, 0, 0};
    append(indirect, 0x25, {0, 0x280, 0x280, 0});
    submit(indirect, "Public geometry indirect firstIndex subset");
    check("Public geometry indirect firstIndex subset", false, 5, 10);
    arguments = {1000, 1, 114, 0, 0};
    submit(indirect, "Public geometry indirect near-end index clamp");
    check("Public geometry indirect near-end index clamp", false, 38, 2);
    std::vector<std::uint32_t> activate;
    append(activate, 0x46, {0x139, 0xb30010, 0});
    submit(activate, "Public mesh query activation");
    const auto checkQuery = [&](std::uint64_t total) {
        for (std::size_t i = 0; i < queries.size(); ++i) {
            auto expected = 0x9192939495969798ull;
            if (i >= 2 && i <= 32 && i % 2 == 0) expected = (1ull << 63u) | (i == 2 ? total : 0);
            Require(queries[i] == expected, "Public mesh draw sample count or sparse guard differs atword " + std::to_string(i));
        }
    };
    checkQuery(0);
    std::fill(vertices.begin(), vertices.end(), Vertex{{8, 8, 0.5f, 1}, {1, 0, 0, 1}});
    for (std::uint32_t i = 0; i < 2 * Columns + 2; ++i) {
        const float x = -1.0f + 2.0f * static_cast<float>(i / 2) / Columns;
        const float y = i % 2 == 0 ? -1.0f : 0.0f;
        vertices[i + 1] = {{x, y, 0.5f, 1}, {128.0f / 255.0f, 1, 64.0f / 255.0f, 1}};
    }
    auto strip = setup(false, false, false, 6);
    const std::uint32_t stripCount = 2 * Columns + 3, firstVertex = 1;
    RegisterPacket(strip, 0x76, 0x8e, std::span(&stripCount, 1));
    RegisterPacket(strip, 0x79, 0x24a, std::span(&firstVertex, 1));
    append(strip, 0x2f, {2});
    append(strip, 0x2d, {2 * Columns + 2, 2});
    append(strip, 0x46, {0x139, 0xb30010, 0});
    submit(strip, "Public geometry triangle strip firstVertex1 and two instances");
    for (std::uint32_t y = 0; y < Height; ++y)
        for (std::uint32_t x = 0; x < Width; ++x) {
            const auto expected = y >= Height / 2 ? std::array<std::uint8_t, 4>{128, 255, 64, 255} : Background;
            for (std::size_t c = 0; c < 4; ++c)
                Require(pixels[256 + (static_cast<std::size_t>(y) * Width + x) * 4 + c] == std::byte{expected[c]},
                    "Public geometry strip changed pixel " + std::to_string(x) + "," + std::to_string(y));
        }
    checkQuery(static_cast<std::uint64_t>(Width) * Height);
    std::cout << "Public geometry strip: firstVertex1, two instance dispatches, exact half-screen pixels and24576 hardware samples passed\n";
    constexpr std::uint32_t FanRim = 8;
    constexpr float FanStep = 6.28318530718f / FanRim;
    std::array<Vertex, FanRim + 1> fan;
    fan[0] = {{0, 0, 0.5f, 1}, {1, 1, 1, 1}};
    for (std::uint32_t k = 1; k <= FanRim; ++k) {
        const auto color = TriangleColor(k);
        const float angle = static_cast<float>(k - 1) * FanStep;
        fan[k] = {{0.9f * std::cos(angle), 0.9f * std::sin(angle), 0.5f, 1},
            {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1}};
    }
    for (std::uint32_t k = 0; k <= FanRim; ++k) {
        vertices[FanRim - k] = fan[k];
        indices[2 + k] = static_cast<std::uint16_t>(FanRim - k);
    }
    auto indexedFan = setup(false, false, false, 5);
    const std::uint32_t fanCount = FanRim + 1;
    RegisterPacket(indexedFan, 0x76, 0x8e, std::span(&fanCount, 1));
    append(indexedFan, 0x2f, {1});
    append(indexedFan, 0x27, {fanCount, static_cast<std::uint32_t>(IndexAddress + 4), 0, fanCount, 0});
    submit(indexedFan, "Public geometry indexed triangle fan");
    for (std::uint32_t triangle = 0; triangle < FanRim; ++triangle) {
        const float angle = (static_cast<float>(triangle) + 0.5f) * FanStep;
        const auto x = static_cast<std::uint32_t>((0.5f * std::cos(angle) + 1) * Width * 0.5f);
        const auto y = static_cast<std::uint32_t>(Height - (0.5f * std::sin(angle) + 1) * Height * 0.5f);
        const auto expected = triangle + 1 < FanRim ? TriangleColor(triangle + 1) : Background;
        for (std::size_t c = 0; c < 4; ++c)
            Require(pixels[256 + (static_cast<std::size_t>(y) * Width + x) * 4 + c] == std::byte{expected[c]},
                "Public geometry fan wrong first-vertex color or closed fan segment " + std::to_string(triangle));
    }
    std::cout << "Public geometry indexed triangle fan: scrambled vertices, first-vertex colors and open final segment passed\n";
    AgcDriverShutdown_nid_postfix();
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 2, "Mesh replay requires the utility metallib path");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Mesh replay requires Metal3");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, std::string("Mesh utility library load failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
            std::cout << "Actual original RDNA public mesh replay on " << device.name.UTF8String << '\n';
            Replay(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
