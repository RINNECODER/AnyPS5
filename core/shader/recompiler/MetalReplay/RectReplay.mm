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
constexpr std::uint32_t Width = 64, Height = 32;
constexpr std::uint64_t VertexAddress = 0x100000, ColorAddress = 0x200100, IndexAddress = 0x300000;
constexpr std::uint64_t CounterAddress = 0x400004;
constexpr std::array<std::uint32_t, 4> ConstantPixel{0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000};
constexpr std::array<std::uint32_t, 7> FlatPixel{0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000};
constexpr std::array<std::uint32_t, 11> SmoothPixel{0xc8100000, 0xc8110001, 0xc8140100, 0xc8150101, 0xc8180200, 0xc8190201, 0xc81c0300, 0xc81d0301, 0xf800180f, 0x07060504, 0xbf810000};
struct Vertex { std::array<float, 4> position, color; };
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
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
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-16.0f)}, {0x112, std::bit_cast<std::uint32_t>(16.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x1b3, PixelInputBit(PixelInput::PositionX) | PixelInputBit(PixelInput::PositionY)},
        {0x1b4, PixelInputBit(PixelInput::PositionX) | PixelInputBit(PixelInput::PositionY)}
    };
    for (const auto& [offset, value] : context) RegisterPacket(commands, 0x69, offset, std::span(&value, 1));
    const std::array<std::uint32_t, 2> vertexProgram{0x500000u >> 8u, 0}, fragmentProgram{0x600000u >> 8u, 0};
    const std::array<std::uint32_t, 1> vertexResources{8}, fragmentResources{0};
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 3, 0x01016fac};
    RegisterPacket(commands, 0x76, 0xc8, vertexProgram);
    RegisterPacket(commands, 0x76, 0x008, fragmentProgram);
    RegisterPacket(commands, 0x76, 0x08b, vertexResources);
    RegisterPacket(commands, 0x76, 0x00b, fragmentResources);
    RegisterPacket(commands, 0x76, 0x08c, userData);
    return commands;
}

void Replay(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::uint64_t CommandAddress = 0xa00000, PacketAddress = 0xa10000;
    std::array<Vertex, 8> vertices;
    std::fill(vertices.begin(), vertices.end(), Vertex{{8, 8, 0.5f, 1}, {1, 0, 0, 1}});
    vertices[1] = {{-.75f, -.5f, .5f, 1}, {0, 0, .25f, 1}};
    vertices[2] = {{-.25f, -.5f, .5f, 1}, {1, 0, .25f, 1}};
    vertices[3] = {{-.75f, .5f, .5f, 1}, {0, 1, .25f, 1}};
    vertices[4] = {{.25f, -.5f, .5f, 1}, {0, 0, .75f, 1}};
    vertices[5] = {{.75f, -.5f, .5f, 1}, {1, 0, .75f, 1}};
    vertices[6] = {{.25f, .5f, .5f, 1}, {0, 1, .75f, 1}};
    std::array<std::uint16_t, 8> indices{0xffff, 3, 1, 2, 6, 4, 5, 0xffff};
    std::array<std::uint32_t, 3> counter{0xcafef00d, 0, 0xdeadbeef};
    std::array<std::uint32_t, 64> vertexCode{}, pixelCode{};
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
    auto vertexHeader = header(0x700000, 0x500000, 0, 2);
    auto pixelHeader = header(0x710000, 0x600000, 0, 1);
    std::array<std::uint32_t, 512> commands{};
    ::Packet packet{reinterpret_cast<std::uint32_t*>(CommandAddress), 0, 0, {}};
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false},
        {ColorAddress - 256, pixels, true}, {IndexAddress, std::as_writable_bytes(std::span(indices)), false},
        {CounterAddress - 4, std::as_writable_bytes(std::span(counter)), true},
        {0x500000, std::as_writable_bytes(std::span(vertexCode)), false},
        {0x600000, std::as_writable_bytes(std::span(pixelCode)), false},
        {0x700000, vertexHeader, false}, {0x710000, pixelHeader, false},
        {CommandAddress, std::as_writable_bytes(std::span(commands)), false},
        {PacketAddress, std::as_writable_bytes(std::span(&packet, 1)), false}};
    AgcDriver::Metal::MetalDriver::Get().Configure((__bridge void*)device, (__bridge void*)library, ranges);
    const auto registerPrograms = [&](std::span<const std::uint32_t> fragment, bool unusedExport, bool shear = false) {
        std::vector<std::uint32_t> vertex{0xe0382000, 0x80020005, 0xe0382010, 0x80020405, 0xbf8c3f70};
        if (shear) vertex.insert(vertex.end(), {0x101002ff, 0x3c23d70a, 0x06001100});
        vertex.insert(vertex.end(), {0x7e100281, 0xe0c80000, 0x80030800, 0xf80008cf, 0x03020100, 0xf800020f, 0x07060504});
        if (unusedExport) vertex.insert(vertex.end(), {0xf800021f, 0x07060504});
        vertex.push_back(0xbf810000);
        std::copy(vertex.begin(), vertex.end(), vertexCode.begin());
        std::copy(fragment.begin(), fragment.end(), pixelCode.begin());
        vertexHeader = header(0x700000, 0x500000, static_cast<std::uint32_t>(vertex.size() * 4), 2);
        pixelHeader = header(0x710000, 0x600000, static_cast<std::uint32_t>(fragment.size() * 4), 1);
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x700000));
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    };
    const auto append = [](std::vector<std::uint32_t>& words, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
        words.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
        words.insert(words.end(), payload.begin(), payload.end());
    };
    const auto setup = [&](std::uint32_t primitive, std::uint32_t interpolation, bool positive = false, bool negativeDepth = false) {
        auto words = GraphicsCommands();
        const auto reg = [&](std::uint32_t bank, std::uint32_t offset, std::uint32_t value) { RegisterPacket(words, bank, offset, std::span(&value, 1)); };
        reg(0x79, 0x242, primitive);
        reg(0x76, 0x8b, 16);
        const std::array<std::uint32_t, 8> users{static_cast<std::uint32_t>(VertexAddress), 32u << 16u, 8, 0x01016fac,
            static_cast<std::uint32_t>(CounterAddress), 0, 4, 0x31016fac};
        RegisterPacket(words, 0x76, 0x8c, users);
        reg(0x69, 0x1b6, interpolation == 0 ? 0x8000 : 0x8001);
        reg(0x69, 0x191, interpolation == 1 ? 0x400 : 0);
        reg(0x69, 0x1b3, interpolation == 2 ? 2 : 0);
        reg(0x69, 0x1b4, interpolation == 2 ? 2 : 0);
        reg(0x69, 0x111, std::bit_cast<std::uint32_t>(positive ? 16.0f : -16.0f));
        reg(0x69, 0x204, negativeDepth ? 0u : 0x80000u);
        reg(0x69, 0x113, std::bit_cast<std::uint32_t>(negativeDepth ? 0.5f : 1.0f));
        reg(0x69, 0x114, std::bit_cast<std::uint32_t>(negativeDepth ? 0.5f : 0.0f));
        reg(0x79, 0x24a, 0);
        return words;
    };
    const auto submit = [&](const std::vector<std::uint32_t>& words) {
        Require(words.size() <= commands.size(), "Rectangle command allocation is too small");
        std::copy(words.begin(), words.end(), commands.begin());
        packet.dw_num = static_cast<std::uint32_t>(words.size());
        std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
        counter[1] = 0;
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0);
        AgcDriverWaitIdle_nid_postfix();
    };
    const auto guards = [&](const std::array<Vertex, 8>& originalVertices) {
        Require(std::memcmp(vertices.data(), originalVertices.data(), sizeof(vertices)) == 0, "Rectangle replay changed borrowed vertices");
        Require(indices == std::array<std::uint16_t, 8>{0xffff, 3, 1, 2, 6, 4, 5, 0xffff}, "Rectangle replay changed index input");
        Require(counter[0] == 0xcafef00d && counter[2] == 0xdeadbeef, "Guest VS write changed counter guard");
        for (std::size_t i = 0; i < 256; ++i) Require(pixels[i] == std::byte{0x7b} && pixels[pixels.size() - 1 - i] == std::byte{0x7b}, "Rectangle replay changed target allocation guard");
    };
    const auto check = [&](std::uint32_t mode, bool positive, bool clipped, const std::string& name) {
        for (std::uint32_t y = 0; y < Height; ++y)
            for (std::uint32_t x = 0; x < Width; ++x) {
                std::array<std::uint8_t, 4> expected{0x40, 0x40, 0x40, 0x40};
                const bool covered = y >= 8 && y < 24 && ((x >= 8 && x < 24) || (x >= 40 && x < 56)) &&
                    (!clipped || (x >= 12 && x < 52 && y >= 12 && y < 20));
                if (covered) {
                    if (mode == 0) expected.fill(255);
                    else if (mode == 1) expected = {0, 255, static_cast<std::uint8_t>(x < 32 ? 64 : 191), 255};
                    else {
                        const auto left = x < 32 ? 8u : 40u;
                        const float red = (static_cast<float>(x - left) + 0.5f) / 16;
                        const float green = positive ? (static_cast<float>(y - 8) + 0.5f) / 16 : (23.5f - static_cast<float>(y)) / 16;
                        expected = {static_cast<std::uint8_t>(std::lround(red * 255)), static_cast<std::uint8_t>(std::lround(green * 255)),
                            static_cast<std::uint8_t>(x < 32 ? 64 : 191), 255};
                    }
                }
                for (std::size_t c = 0; c < 4; ++c)
                    Require(pixels[256 + (static_cast<std::size_t>(y) * Width + x) * 4 + c] == std::byte{expected[c]},
                        name + " wrongpixel " + std::to_string(x) + "," + std::to_string(y) + " channel " + std::to_string(c));
            }
        std::cout << name << " independent entire-target pixels passed\n";
    };
    registerPrograms(ConstantPixel, false);
    auto automatic = setup(7, 0);
    const std::uint32_t firstVertex = 1;
    RegisterPacket(automatic, 0x79, 0x24a, std::span(&firstVertex, 1));
    append(automatic, 0x2f, {2});
    append(automatic, 0x2d, {6, 2});
    auto originalVertices = vertices;
    submit(automatic);
    check(0, false, false, "Public rectangle primitive7: unused VS parameter, firstVertex1 and two instances");
    Require(counter[1] == 12, "Rectangle expansion replayed guest VS side effects instead of six vertices per instance");
    guards(originalVertices);
    registerPrograms(FlatPixel, true);
    auto indexed = setup(17, 1);
    append(indexed, 0x2f, {1});
    append(indexed, 0x2a, {0});
    append(indexed, 0x27, {6, static_cast<std::uint32_t>(IndexAddress + 2), 0, 6, 0});
    submit(indexed);
    check(1, false, false, "Public rectangle primitive17: indexed corner order, flat first-vertex color and unused export subset");
    Require(counter[1] == 6, "Indexed rectangle guest VS side effects were not executed once per input vertex");
    guards(originalVertices);
    const auto unbasedIndices = indices;
    for (std::size_t i = 1; i <= 6; ++i) indices[i] += 3;
    const auto biasedIndices = indices;
    auto negativeIndexed = setup(17, 1);
    const std::uint32_t negativeBase = 0xfffffffdu;
    RegisterPacket(negativeIndexed, 0x79, 0x24a, std::span(&negativeBase, 1));
    append(negativeIndexed, 0x2f, {1});
    append(negativeIndexed, 0x2a, {0});
    append(negativeIndexed, 0x27, {6, static_cast<std::uint32_t>(IndexAddress + 2), 0, 6, 0});
    submit(negativeIndexed);
    check(1, false, false, "Public indexed rectangle: signed base -3 with positively biased indices");
    Require(counter[1] == 6, "Signed indexed rectangle guest VS writes were not executed once per input vertex");
    Require(indices == biasedIndices, "Signed indexed rectangle modified biased guest index input");
    indices = unbasedIndices;
    guards(originalVertices);
    registerPrograms(SmoothPixel, true);
    auto smooth = setup(7, 2);
    RegisterPacket(smooth, 0x79, 0x24a, std::span(&firstVertex, 1));
    append(smooth, 0x2d, {6, 2});
    submit(smooth);
    check(2, false, false, "Public rectangle smooth parameter reconstruction");
    Require(counter[1] == 6, "Smooth rectangle changed actual guest VS invocation count");
    guards(originalVertices);
    for (std::size_t i = 1; i <= 6; ++i) vertices[i].position[2] = -0.5f;
    originalVertices = vertices;
    auto clipped = setup(17, 2, true, true);
    RegisterPacket(clipped, 0x79, 0x24a, std::span(&firstVertex, 1));
    const std::uint32_t scissorTopLeft = 0x80000000u | (12u << 16u) | 12u, scissorBottomRight = (20u << 16u) | 52u;
    RegisterPacket(clipped, 0x69, 0x90, std::span(&scissorTopLeft, 1));
    RegisterPacket(clipped, 0x69, 0x91, std::span(&scissorBottomRight, 1));
    append(clipped, 0x2d, {6, 2});
    submit(clipped);
    check(2, true, true, "Public rectangle positive viewport, negativeOneToOne depth and scissor");
    Require(counter[1] == 6, "Clipped rectangle changed actual guest VS invocation count");
    guards(originalVertices);
    for (std::size_t i = 1; i <= 6; ++i) vertices[i].position[2] = 0.5f;
    originalVertices = vertices;
    registerPrograms(ConstantPixel, false, true);
    auto invalid = setup(7, 0);
    RegisterPacket(invalid, 0x79, 0x24a, std::span(&firstVertex, 1));
    append(invalid, 0x2d, {3, 2});
    bool fault = false;
    try { submit(invalid); }
    catch (const std::runtime_error& error) { fault = std::string(error.what()).find("reason=5") != std::string::npos; if (!fault) throw; }
    Require(fault, "Valid rectangle input sheared by guest VS did not report generated GPU InvalidRectangle fault");
    Require(counter[1] == 3, "Malformed post-VS rectangle did not execute exactly three real guest VS writes");
    guards(originalVertices);
    for (std::size_t i = 256; i < 256 + Width * Height * 4; ++i) Require(pixels[i] == std::byte{0x40}, "Malformed post-VS rectangle rendered target bytes");
    try { AgcDriverShutdown_nid_postfix(); }
    catch (const std::runtime_error& error) { Require(std::string(error.what()).find("reason=5") != std::string::npos, "Rectangle shutdown lost the original GPU fault"); }
    std::cout << "Public rectangle malformed post-VS output: three actual guest writes and generated GPU fault reason5 passed\n";
}
}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 2, "Rectangle replay requires utility metallib path");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Rectangle replay requires Metal3");
            NSError* error = nil;
            auto library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, std::string("Rectangle utility library load failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
            std::cout << "Actual RDNA public rectangle replay on " << device.name.UTF8String << '\n';
            Replay(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
