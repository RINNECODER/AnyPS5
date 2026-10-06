#import <Foundation/Foundation.h>
#include "MetalDraw.hpp"
#include "Recompiler.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <future>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;

constexpr std::uint32_t Width = 64, Height = 32;
constexpr std::uint64_t VertexAddress = 0x100000, ColorAllocation = 0x200000, ColorAddress = ColorAllocation + 256;
constexpr std::uint64_t IndexAddress = 0x300000;
constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};
constexpr std::array<std::uint32_t, 4> FullPixelCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};
constexpr std::array<std::uint32_t, 7> MaskedPixelCode{
    0x7e040f00, 0x36060481, 0x7da40680, 0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};
constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

SpirvTarget Target(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
            {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height), static_cast<std::uint32_t>(maximum.depth)},
            static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
}

AgcDriver::Graphics::State State() {
    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0, 32, 32, {}, {}};
    state.color = {ColorAddress, {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Width * Height * 4, 0xe4};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, 0, static_cast<float>(Width), static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    return state;
}

ShaderPixelStageInfo PixelInfo() {
    ShaderPixelStageInfo pixel{};
    pixel.wave32 = true;
    pixel.inputAddr = PixelInputBit(PixelInput::PositionX) | PixelInputBit(PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4);
    return pixel;
}

struct ShaderPair {
    RecompileResult vertex;
    RecompileResult fragment;
    std::uint32_t fragmentPush;
};

ShaderPair Compile(id<MTLDevice> device, std::span<const std::uint32_t> fragmentCode, std::uint32_t vertices) {
    const std::array<std::uint32_t, 4> vertexUserData{
        static_cast<std::uint32_t>(VertexAddress), 16u << 16u, vertices, 0x01016fac};
    const std::array<MemoryRegion, 1> vertexMemory{{{0x500000, std::as_bytes(std::span(VertexCode))}}};
    RecompileRequest vertex{{ShaderStage::Vertex, 0x500000, VertexCode, 0, {}},
        {32, 0, vertexUserData, {}, {}, ShaderVertexStageInfo{}, vertexMemory}, Target(device), {0, 0, 0, 64}, {}};
    vertex.useCache = false;
    auto vertexResult = Recompile(vertex);
    const auto fragmentPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());
    const std::array<std::uint32_t, 8> fragmentUserData{};
    const std::array<MemoryRegion, 1> fragmentMemory{{{0x600000, std::as_bytes(fragmentCode)}}};
    RecompileRequest fragment{{ShaderStage::Fragment, 0x600000, fragmentCode, 0, {}},
        {32, 0, fragmentUserData, {}, PixelInfo(), {}, fragmentMemory}, Target(device), {0, 0, fragmentPush, 128u - fragmentPush}, {}};
    fragment.useCache = false;
    return {std::move(vertexResult), Recompile(fragment), fragmentPush};
}

void DrawGuest(id<MTLDevice> device, id<MTLLibrary> library, bool masked, bool indexed, bool maximumIndex = false) {
    std::vector<std::array<float, 4>> vertices;
    if (maximumIndex) {
        vertices.assign(65536, {8, 8, 0.5f, 1});
        vertices[3] = Triangle[0];
        vertices[4] = Triangle[1];
        vertices[65535] = Triangle[2];
    } else {
        if (indexed) vertices.assign(3, {8, 8, 0.5f, 1});
        vertices.insert(vertices.end(), Triangle.begin(), Triangle.end());
    }
    const auto originalVertices = vertices;
    std::array<std::uint16_t, 5> indices{0, 3, 4, 5, 0xffff};
    if (maximumIndex) indices[3] = 65535;
    const auto originalIndices = indices;
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
    const auto code = masked ? std::span<const std::uint32_t>(MaskedPixelCode) : std::span<const std::uint32_t>(FullPixelCode);
    const auto compiled = Compile(device, code, static_cast<std::uint32_t>(vertices.size()));
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &compiled.vertex, 0}, {ShaderStage::Fragment, &compiled.fragment, compiled.fragmentPush}}};
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false}, {ColorAllocation, pixels, true}};
    if (indexed) ranges.push_back({IndexAddress, std::as_writable_bytes(std::span(indices)), false});
    AgcDriver::Pm4::DrawParameters draw{};
    draw.indexAddress = indexed ? IndexAddress + 2u : 0u;
    draw.indexCount = 3;
    draw.indexSize = indexed ? 2u : 0u;
    draw.instanceCount = 1;
    draw.indexed = indexed;
    auto state = State();
    if (maximumIndex) state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    AgcDriver::Metal::MetalDraw adapter(device, library);
    const auto fault = adapter.DrawSynchronously(state, draw, shaders, ranges);
    Require(fault.state == BdaAbi::FaultState::Empty, "Actual guest draw published a GPU memory fault");
    for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
        std::byte expected{0x7b};
        if (offset >= 256 && offset < 256 + Width * Height * 4) {
            const auto x = ((offset - 256) / 4) % Width;
            expected = !masked || x % 2 == 0 ? std::byte{255} : std::byte{0x40};
        }
        Require(pixels[offset] == expected,
            (maximumIndex ? "Actual RDNA uint16 index65535 without primitive restart produced wrong target byte " :
                            "Actual RDNA vertex/fragment draw produced wrong target byte ") + std::to_string(offset));
    }
    Require(vertices == originalVertices && indices == originalIndices, "Actual guest draw modified borrowed vertex or index inputs");
    std::cout << "Actual RDNA guest draw: " << (masked ? "masked even-column fragment exports" : "full fragment exports")
              << (maximumIndex ? ", uint16 vertex index65535 without primitive restart" :
                  indexed ? ", indexed vertex fetch with nonzero guest index offset" : ", descriptor vertex fetch") << " passed\n";
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

struct SubmittedFanReplay {
    static constexpr std::uint64_t VertexAddress = 0x2000000, IndexAddress = 0x2010000, Index32Address = 0x2020000;
    static constexpr std::uint64_t VertexCodeAddress = 0x2030000, PixelCodeAddress = 0x2040000;
    static constexpr std::uint64_t VertexHeaderAddress = 0x2050000, PixelHeaderAddress = 0x2060000;
    static constexpr std::uint64_t CommandAddress = 0x2070000, PacketAddress = 0x2080000;
    struct Vertex { std::array<float, 4> position, color; };
    std::array<Vertex, 16> vertices;
    std::array<std::uint16_t, 32> indices16;
    std::array<std::uint32_t, 32> indices32;
    std::array<std::uint32_t, 10> vertexCode{
        0xe0382000, 0x80020005, 0xe0382010, 0x80020405, 0xbf8c3f70,
        0xf80008cf, 0x03020100, 0xf800020f, 0x07060504, 0xbf810000};
    std::array<std::uint32_t, 7> pixelCode{
        0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000};
    std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> vertexHeader{}, pixelHeader{};
    std::array<std::uint32_t, 512> commands{};
    ::Packet packet{reinterpret_cast<std::uint32_t*>(CommandAddress), 0, 0, {}};

    SubmittedFanReplay() {
        const auto header = [](auto& destination, std::uint64_t address, std::uint64_t code, std::uint32_t bytes, std::uint8_t type) {
            Shader shader{};
            shader.file_header = 0x34333231;
            shader.version = 0x18;
            shader.code = reinterpret_cast<const volatile void*>(code);
            shader.user_data = reinterpret_cast<ShaderUserData*>(address + sizeof(Shader));
            shader.header_size = static_cast<std::uint32_t>(destination.size());
            shader.shader_size = bytes;
            shader.type = type;
            std::memcpy(destination.data(), &shader, sizeof(shader));
        };
        header(vertexHeader, VertexHeaderAddress, VertexCodeAddress, sizeof(vertexCode), 2);
        header(pixelHeader, PixelHeaderAddress, PixelCodeAddress, sizeof(pixelCode), 1);
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.insert(ranges.end(), {
            {VertexAddress, std::as_writable_bytes(std::span(vertices)), false},
            {IndexAddress, std::as_writable_bytes(std::span(indices16)), false},
            {Index32Address, std::as_writable_bytes(std::span(indices32)), false},
            {VertexCodeAddress, std::as_writable_bytes(std::span(vertexCode)), false},
            {PixelCodeAddress, std::as_writable_bytes(std::span(pixelCode)), false},
            {VertexHeaderAddress, vertexHeader, false}, {PixelHeaderAddress, pixelHeader, false},
            {CommandAddress, std::as_writable_bytes(std::span(commands)), false},
            {PacketAddress, std::as_writable_bytes(std::span(&packet, 1)), false}});
    }

    void Run(std::vector<std::byte>& pixels) {
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(VertexHeaderAddress));
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(PixelHeaderAddress));
        const auto append = [](std::vector<std::uint32_t>& words, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
            words.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
            words.insert(words.end(), payload.begin(), payload.end());
        };
        const auto reg = [](std::vector<std::uint32_t>& words, std::uint32_t bank, std::uint32_t offset, std::uint32_t value) {
            RegisterPacket(words, bank, offset, std::span(&value, 1));
        };
        for (const auto mode : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
            const bool indexed = mode != 0;
            const bool restart = mode >= 5;
            const bool culled = mode == 3;
            const std::uint32_t instances = mode == 4 ? 2 : 1;
            const auto strength = mode == 4 ? 0.25f : 1.0f;
            const std::array<Vertex, 4> fan{{
                {{-.75f, -.75f, .5f, 1}, {0, 0, strength, strength}},
                {{.75f, -.75f, .5f, 1}, {strength, 0, 0, strength}},
                {{.75f, .75f, .5f, 1}, {0, strength, 0, strength}},
                {{-.75f, .75f, .5f, 1}, {strength, strength, 0, strength}}}};
            std::fill(vertices.begin(), vertices.end(), Vertex{{8, 8, .5f, 1}, {1, 0, 1, 1}});
            if (restart) {
                const std::array<Vertex, 8> separated{{
                    {{-.75f, -.25f, .5f, 1}, {1, 0, 1, 1}},
                    {{-.25f, -.25f, .5f, 1}, {1, 0, 0, 1}},
                    {{-.25f, .25f, .5f, 1}, {0, 1, 0, 1}},
                    {{-.75f, .25f, .5f, 1}, {1, 1, 1, 1}},
                    {{.25f, -.25f, .5f, 1}, {1, 0, 1, 1}},
                    {{.75f, -.25f, .5f, 1}, {0, 0, 1, 1}},
                    {{.75f, .25f, .5f, 1}, {1, 1, 0, 1}},
                    {{.25f, .25f, .5f, 1}, {1, 1, 1, 1}}}};
                std::copy(separated.begin(), separated.end(), vertices.begin());
            } else if (indexed) std::copy(fan.begin(), fan.end(), vertices.begin());
            else std::copy(fan.begin(), fan.end(), vertices.begin() + 3);
            indices16.fill(0xffff);
            indices32.fill(0xffffffff);
            if (restart) {
                const std::array<std::uint32_t, 17> values{3, UINT32_MAX, 3, 4, 5, 6, UINT32_MAX, 3, 4, UINT32_MAX, 7, 8, 9, 10, UINT32_MAX, 7, 8};
                for (std::uint32_t i = 0; i < values.size(); ++i) {
                    indices16[1 + i] = static_cast<std::uint16_t>(values[i]);
                    indices32[1 + i] = values[i];
                }
            } else for (std::uint32_t i = 0; i < 4; ++i) indices16[1 + i] = indices32[1 + i] = 3 + i;
            const auto originalVertices = vertices;
            const auto original16 = indices16;
            const auto original32 = indices32;
            auto words = GraphicsCommands();
            const std::array<std::uint32_t, 2> vertexProgram{static_cast<std::uint32_t>(VertexCodeAddress >> 8u), 0},
                pixelProgram{static_cast<std::uint32_t>(PixelCodeAddress >> 8u), 0};
            RegisterPacket(words, 0x76, 0xc8, vertexProgram);
            RegisterPacket(words, 0x76, 0x008, pixelProgram);
            reg(words, 0x79, 0x242, 5);
            reg(words, 0x79, 0x24a, indexed ? 0xfffffffdu : 3u);
            reg(words, 0x79, 0x24b, restart ? 1u : 0u);
            reg(words, 0x69, 0x205, culled ? 0x246 : 0x242);
            reg(words, 0x69, 0x1b6, 0x8001);
            reg(words, 0x69, 0x191, 0x400);
            reg(words, 0x69, 0x1b3, 0);
            reg(words, 0x69, 0x1b4, 0);
            reg(words, 0x69, 0x1e0, mode == 4 ? 0x40000101 : 0);
            const std::array<std::uint32_t, 4> users{static_cast<std::uint32_t>(VertexAddress), 32u << 16u, 16, 0x01016fac};
            RegisterPacket(words, 0x76, 0x8c, users);
            append(words, 0x2f, {instances});
            const auto count = restart ? 17u : 4u;
            if (indexed) {
                const bool wide = mode == 2 || mode == 6;
                append(words, 0x2a, {wide ? 1u : 0u});
                append(words, 0x27, {count, static_cast<std::uint32_t>(wide ? Index32Address + 4 : IndexAddress + 2), 0, count, 0});
            } else append(words, 0x2d, {count, 2});
            Require(words.size() <= commands.size(), "Fan commands exceed borrowed allocation");
            std::copy(words.begin(), words.end(), commands.begin());
            packet.dw_num = static_cast<std::uint32_t>(words.size());
            std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0});
            AgcDriver::Submit(reinterpret_cast<const Packet*>(PacketAddress), 0);
            AgcDriverWaitIdle_nid_postfix();
            for (std::uint32_t y = 0; y < Height; ++y) for (std::uint32_t x = 0; x < Width; ++x) {
                std::array<std::uint8_t, 4> expected{};
                if (restart && y >= 12 && y < 20 && ((x >= 8 && x < 24) || (x >= 40 && x < 56))) {
                    const bool left = x < 32;
                    const bool lower = x + 2 * y >= (left ? 47u : 79u);
                    if (left) expected[lower ? 0 : 1] = 255;
                    else if (lower) expected[2] = 255;
                    else expected[0] = expected[1] = 255;
                    expected[3] = 255;
                } else if (!restart && !culled && x >= 8 && x < 56 && y >= 4 && y < 28) {
                    const auto value = static_cast<std::uint8_t>(mode == 4 ? 128 : 255);
                    expected[x + 2 * y >= 63 ? 0 : 1] = value;
                    expected[3] = value;
                }
                for (std::uint32_t c = 0; c < 4; ++c) Require(pixels[256 + (y * Width + x) * 4 + c] == std::byte{expected[c]},
                    "Public fan mode=" + std::to_string(mode) + " pixel=" + std::to_string(x) + "," + std::to_string(y) + " channel=" + std::to_string(c));
            }
            Require(std::memcmp(vertices.data(), originalVertices.data(), sizeof(vertices)) == 0 && indices16 == original16 && indices32 == original32,
                "Public fan changed borrowed vertices or index allocation");
            for (std::size_t i = 0; i < 256; ++i) Require(pixels[i] == std::byte{0x7b} && pixels[pixels.size() - 1 - i] == std::byte{0x7b}, "Fan wrote target guard");
            std::cout << "Public ordinary vertex fan " << mode << " all pixels and guards passed\n";
        }
        std::vector<std::uint32_t> reset;
        reg(reset, 0x79, 0x24b, 0);
        std::copy(reset.begin(), reset.end(), commands.begin());
        packet.dw_num = static_cast<std::uint32_t>(reset.size());
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0);
        AgcDriverWaitIdle_nid_postfix();
    }
};

namespace MinimumLodCompare {
using namespace ShaderRecompiler;
constexpr uint32_t Width=64, Height=32, GuardWords=64, Sentinel=0xdeadbeef;
constexpr uint64_t VertexAddress=0x3100000, ColorAllocation=0x3200000, ColorAddress=ColorAllocation+256;
constexpr uint64_t TextureAllocation=0x3300000, TextureAddress=TextureAllocation+256;
constexpr uint64_t OutputAllocation=0x3400000, OutputAddress=OutputAllocation+256;
constexpr uint64_t VertexCodeAddress=0x3500000, PixelCodeAddress=0x3600000;
constexpr uint64_t VertexHeaderAddress=0x3700000, PixelHeaderAddress=0x3710000;
constexpr uint64_t CommandAddress=0x3800000, PacketAddress=0x3810000;
void Require(bool v,const std::string& m){if(!v)throw std::runtime_error(m);}
constexpr std::array<std::array<float,4>,3> Triangle{{{-1,-1,.5f,1},{3,-1,.5f,1},{-1,3,.5f,1}}};
constexpr std::array<uint32_t,6> VertexCode{0xe0382000,0x80020005,0xbf8c3f70,0xf80008cf,0x03020100,0xbf810000};
constexpr std::array<uint32_t,22> CompareCode{
0xbe90037e,0xbefe097e,0x7e0802f0,
0x100a00ff,0x3c800000,0x100c02ff,0x3d000000,
0x7e0e0f00,0x7e140f01,0x34141486,0x4a0e1507,0x340e0e83,
0xf0a00108,0x00610804,0xbf8c3f70,
0xbefe0310,0xe0701000,0x80000807,
0x7e1402f2,0xf800180f,0x0a0a0a0a,0xbf810000};
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
    const std::array<std::uint32_t, 2> vertexProgram{0x3500000 >> 8u, 0}, fragmentProgram{0x3600000 >> 8u, 0};
    const std::array<std::uint32_t, 1> vertexResources{8}, fragmentResources{0};
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 3, 0x01016fac};
    RegisterPacket(commands, 0x76, 0xc8, vertexProgram);
    RegisterPacket(commands, 0x76, 0x008, fragmentProgram);
    RegisterPacket(commands, 0x76, 0x08b, vertexResources);
    RegisterPacket(commands, 0x76, 0x00b, fragmentResources);
    RegisterPacket(commands, 0x76, 0x08c, userData);
    return commands;
}

void Header(auto& backing,uint64_t address,uint64_t code,uint32_t bytes,uint8_t type){
Shader s{};s.file_header=0x34333231;s.version=0x18;s.user_data=reinterpret_cast<ShaderUserData*>(address+sizeof(Shader));
s.code=reinterpret_cast<const volatile void*>(code);s.header_size=backing.size();s.shader_size=bytes;s.type=type;std::memcpy(backing.data(),&s,sizeof(s));}

struct Replay {
    std::array<std::array<float,4>,3> vertices=Triangle;
    std::array<uint32_t,6> vertexCode=VertexCode;
    std::array<uint32_t,22> pixelCode=CompareCode;
    std::array<std::byte,sizeof(Shader)+sizeof(ShaderUserData)> vertexHeader{},pixelHeader{};
    std::vector<std::byte> pixels=std::vector<std::byte>(256+Width*Height*4+256,std::byte{0x7b});
    std::vector<uint32_t> texture=std::vector<uint32_t>(GuardWords+8192+GuardWords,Sentinel);
    std::vector<uint32_t> output=std::vector<uint32_t>(GuardWords+Width*Height*2+GuardWords,Sentinel);
    std::vector<uint32_t> commands;
    ::Packet packet{};

    Replay() {
        Header(vertexHeader,VertexHeaderAddress,VertexCodeAddress,sizeof(vertexCode),2);
        Header(pixelHeader,PixelHeaderAddress,PixelCodeAddress,sizeof(pixelCode),1);
        constexpr std::array<uint32_t,6> offsets{7936,3840,1792,768,256,0};
        for(uint32_t level=0;level<6;++level)
            for(uint32_t y=0;y<std::max(1u,Height>>level);++y)
                std::fill_n(texture.begin()+GuardWords+offsets[level]/4+y*64,Width>>level,
                    std::bit_cast<uint32_t>(level<2?.25f:.75f));
        commands=GraphicsCommands();
        const std::array<uint32_t,1> zeroBase{0};
        RegisterPacket(commands,0x79,0x24a,zeroBase);
        commands.insert(commands.end(),{0xc0002f00,1});
        const std::array<uint32_t,16> users{
            static_cast<uint32_t>(OutputAddress),0,Width*Height*8,0x01016fac,
            static_cast<uint32_t>(TextureAddress>>8),(22u<<20)|(3u<<30)|(384u<<8),15u|(31u<<14)|(1u<<31),
            0xfacu|(5u<<16)|(9u<<28),0,5u<<4,0,0,
            0x92u|(1u<<12),(8u*256u)<<12,(1u<<22)|(2u<<26),0};
        const std::array<uint32_t,1> resources{32};
        RegisterPacket(commands,0x76,0x00b,resources);RegisterPacket(commands,0x76,0x00c,users);
        commands.insert(commands.end(),{0xc0012d00,3,2});
        packet={reinterpret_cast<uint32_t*>(CommandAddress),static_cast<uint32_t>(commands.size()),0,{}};
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.insert(ranges.end(),{
            {VertexAddress,std::as_writable_bytes(std::span(vertices)),false},{ColorAllocation,pixels,true},
            {TextureAllocation,std::as_writable_bytes(std::span(texture)),false},{OutputAllocation,std::as_writable_bytes(std::span(output)),true},
            {VertexCodeAddress,std::as_writable_bytes(std::span(vertexCode)),false},{PixelCodeAddress,std::as_writable_bytes(std::span(pixelCode)),false},
            {VertexHeaderAddress,vertexHeader,false},{PixelHeaderAddress,pixelHeader,false},
            {CommandAddress,std::as_writable_bytes(std::span(commands)),false},{PacketAddress,std::as_writable_bytes(std::span(&packet,1)),false}});
    }

    void Run(bool levelZero, uint32_t floor=384, uint32_t format=22) {
        pixelCode=CompareCode;
        for(auto& word:commands) if((word&~(0xfffu<<8))==((22u<<20)|(3u<<30))||(word&~(0xfffu<<8))==((7u<<20)|(3u<<30))) word=(format<<20)|(3u<<30)|(floor<<8);
        std::fill(texture.begin(),texture.end(),Sentinel);
        constexpr std::array<uint32_t,6> offsets{7936,3840,1792,768,256,0};
        for(uint32_t level=0;level<6;++level) for(uint32_t y=0;y<std::max(1u,Height>>level);++y) {
            auto* row=reinterpret_cast<std::byte*>(texture.data()+GuardWords)+offsets[level]+y*256;
            if(format==22) std::fill_n(reinterpret_cast<uint32_t*>(row),Width>>level,std::bit_cast<uint32_t>(level<2?.25f:.75f));
            else std::fill_n(reinterpret_cast<uint16_t*>(row),Width>>level,static_cast<uint16_t>(level<2?0x4000:0xbfff));
        }
        if(levelZero) pixelCode[12]=0xf0bc0108;
        std::fill(output.begin(),output.end(),Sentinel);
        std::fill_n(pixels.begin()+256,Width*Height*4,std::byte{0x40});
        const auto originalTexture=texture;
        const auto originalCode=pixelCode;
        const auto originalVertices=vertices;
        const auto originalCommands=commands;
        const auto originalHeader=pixelHeader;
        const auto originalPacket=packet;
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(VertexHeaderAddress));
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(PixelHeaderAddress));
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress),0);AgcDriverWaitIdle_nid_postfix();
        for(size_t word=0;word<output.size();++word) {
            uint32_t wanted=Sentinel;
            if(word>=GuardWords&&word<GuardWords+Width*Height*2)
                wanted=(word-GuardWords)%2==0?std::bit_cast<uint32_t>(floor==0?0.0f:.5f):Sentinel;
            Require(output[word]==wanted,"Public pixel comparison minimumLOD levelZero="+std::to_string(levelZero)+" word="+std::to_string(word)+
                " actual="+std::to_string(output[word])+" expected="+std::to_string(wanted));
        }
        for(size_t i=0;i<pixels.size();++i) {
            const auto wanted=i>=256&&i<256+Width*Height*4?std::byte{255}:std::byte{0x7b};
            Require(pixels[i]==wanted,"Public pixel LOD framebuffer/guard byte="+std::to_string(i));
        }
        Require(texture==originalTexture&&vertices==originalVertices&&pixelCode==originalCode&&commands==originalCommands&&pixelHeader==originalHeader&&
            std::memcmp(&packet,&originalPacket,sizeof(packet))==0,"Public pixel LOD changed read-only shader, inputs or PM4 bytes");
        std::cout<<"Public pixel comparison format="<<format<<" floor="<<floor<<" levelZero="<<levelZero<<": 2048 exact comparison values, untouched neighboring DWORDs, full framebuffer, input padding and guards passed\n";
    }
};

}
namespace MinimumLodPixel {
using namespace ShaderRecompiler;
constexpr uint32_t Width=64, Height=32, GuardWords=64, Sentinel=0xdeadbeef;
constexpr uint64_t VertexAddress=0x2100000, ColorAllocation=0x2200000, ColorAddress=ColorAllocation+256;
constexpr uint64_t TextureAllocation=0x2300000, TextureAddress=TextureAllocation+256;
constexpr uint64_t OutputAllocation=0x2400000, OutputAddress=OutputAllocation+256;
constexpr uint64_t VertexCodeAddress=0x2500000, PixelCodeAddress=0x2600000;
constexpr uint64_t VertexHeaderAddress=0x2700000, PixelHeaderAddress=0x2710000;
constexpr uint64_t CommandAddress=0x2800000, PacketAddress=0x2810000;
void Require(bool v,const std::string& m){if(!v)throw std::runtime_error(m);}
constexpr std::array<std::array<float,4>,3> Triangle{{{-1,-1,.5f,1},{3,-1,.5f,1},{-1,3,.5f,1}}};
constexpr std::array<uint32_t,6> VertexCode{0xe0382000,0x80020005,0xbf8c3f70,0xf80008cf,0x03020100,0xbf810000};
constexpr std::array<uint32_t,23> QueryCode{
0xbe90037e,0xbefe097e,0x100800ff,0x3c800000,0x100a02ff,0x3d000000,
0x7e0c0f00,0x7e0e0f01,0x340e0e86,0x4a0c0f06,0x340c0c83,
0xf1800308,0x00610804,0xbf8c3f70,
0xbefe0310,0xe0701000,0x80000806,0xe0701004,0x80000906,
0x7e1402f2,0xf800180f,0x0a0a0a0a,0xbf810000};
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
    const std::array<std::uint32_t, 2> vertexProgram{0x2500000 >> 8u, 0}, fragmentProgram{0x2600000 >> 8u, 0};
    const std::array<std::uint32_t, 1> vertexResources{8}, fragmentResources{0};
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 3, 0x01016fac};
    RegisterPacket(commands, 0x76, 0xc8, vertexProgram);
    RegisterPacket(commands, 0x76, 0x008, fragmentProgram);
    RegisterPacket(commands, 0x76, 0x08b, vertexResources);
    RegisterPacket(commands, 0x76, 0x00b, fragmentResources);
    RegisterPacket(commands, 0x76, 0x08c, userData);
    return commands;
}

void Header(auto& backing,uint64_t address,uint64_t code,uint32_t bytes,uint8_t type){
Shader s{};s.file_header=0x34333231;s.version=0x18;s.user_data=reinterpret_cast<ShaderUserData*>(address+sizeof(Shader));
s.code=reinterpret_cast<const volatile void*>(code);s.header_size=backing.size();s.shader_size=bytes;s.type=type;std::memcpy(backing.data(),&s,sizeof(s));}

struct Replay {
    std::array<std::array<float,4>,3> vertices=Triangle;
    std::array<uint32_t,6> vertexCode=VertexCode;
    std::array<uint32_t,26> pixelCode{};
    std::array<std::byte,sizeof(Shader)+sizeof(ShaderUserData)> vertexHeader{},pixelHeader{};
    std::vector<std::byte> pixels=std::vector<std::byte>(256+Width*Height*4+256,std::byte{0x7b});
    std::vector<uint32_t> texture=std::vector<uint32_t>(GuardWords+8192+GuardWords,Sentinel);
    std::vector<uint32_t> output=std::vector<uint32_t>(GuardWords+Width*Height*2+GuardWords,Sentinel);
    std::vector<uint32_t> commands;
    ::Packet packet{};
    std::size_t imageWord=0;

    Replay() {
        pixelCode.fill(0xbf800000);
        std::copy(QueryCode.begin(),QueryCode.end(),pixelCode.begin());
        Header(vertexHeader,VertexHeaderAddress,VertexCodeAddress,sizeof(vertexCode),2);
        Header(pixelHeader,PixelHeaderAddress,PixelCodeAddress,sizeof(pixelCode),1);
        constexpr std::array<uint32_t,6> offsets{7936,3840,1792,768,256,0};
        for(uint32_t level=0;level<6;++level)
            for(uint32_t y=0;y<std::max(1u,Height>>level);++y)
                std::fill_n(texture.begin()+GuardWords+offsets[level]/4+y*64,Width>>level,
                    std::bit_cast<uint32_t>(float(level*16u)));
        commands=GraphicsCommands();
        const std::array<uint32_t,1> zeroBase{0};
        RegisterPacket(commands,0x79,0x24a,zeroBase);
        commands.insert(commands.end(),{0xc0002f00,1});
        const std::array<uint32_t,16> users{
            static_cast<uint32_t>(OutputAddress),0,Width*Height*8,0x01016fac,
            static_cast<uint32_t>(TextureAddress>>8),(22u<<20)|(3u<<30)|(384u<<8),15u|(31u<<14)|(1u<<31),
            0xfacu|(5u<<16)|(9u<<28),0,5u<<4,0,0,
            0x92u,(8u*256u)<<12,(1u<<22)|(2u<<26),0};
        const std::array<uint32_t,1> resources{32};
        RegisterPacket(commands,0x76,0x00b,resources);
        imageWord=commands.size()+7;
        RegisterPacket(commands,0x76,0x00c,users);
        commands.insert(commands.end(),{0xc0012d00,3,2});
        packet={reinterpret_cast<uint32_t*>(CommandAddress),static_cast<uint32_t>(commands.size()),0,{}};
    }

    void AddRanges(std::vector<AgcDriver::NativeGuestMemory::BorrowedRange>& ranges) {
        ranges.insert(ranges.end(),{
            {VertexAddress,std::as_writable_bytes(std::span(vertices)),false},{ColorAllocation,pixels,true},
            {TextureAllocation,std::as_writable_bytes(std::span(texture)),false},{OutputAllocation,std::as_writable_bytes(std::span(output)),true},
            {VertexCodeAddress,std::as_writable_bytes(std::span(vertexCode)),false},{PixelCodeAddress,std::as_writable_bytes(std::span(pixelCode)),false},
            {VertexHeaderAddress,vertexHeader,false},{PixelHeaderAddress,pixelHeader,false},
            {CommandAddress,std::as_writable_bytes(std::span(commands)),false},{PacketAddress,std::as_writable_bytes(std::span(&packet,1)),false}});
    }

    void Run(uint32_t mode, uint32_t viewFloor=384) {
        commands[imageWord]=(22u<<20)|(3u<<30)|(viewFloor<<8);
        pixelCode.fill(0xbf800000);
        std::copy(QueryCode.begin(),QueryCode.end(),pixelCode.begin());
        if(mode==0) pixelCode[11]=0xf0800308;
        if(mode>=2) {
            std::copy_backward(pixelCode.begin()+11,pixelCode.begin()+23,pixelCode.end());
            pixelCode[11]=mode==2?0x7e1802f0:0x7e180304;
            pixelCode[12]=mode==2?0x7e1a0304:0x7e1a0305;
            pixelCode[13]=mode==2?0x7e1c0305:0x7e1c02f4;
            pixelCode[14]=mode==2?0xf0940308:0xf0840308;
            pixelCode[15]=0x0061080c;
        }
        std::fill(output.begin(),output.end(),Sentinel);
        std::fill_n(pixels.begin()+256,Width*Height*4,std::byte{0x40});
        const auto originalTexture=texture;
        const auto originalCode=pixelCode;
        const auto originalVertices=vertices;
        const auto originalCommands=commands;
        const auto originalHeader=pixelHeader;
        const auto originalPacket=packet;
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(VertexHeaderAddress));
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(PixelHeaderAddress));
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress),0);AgcDriverWaitIdle_nid_postfix();
        for(size_t word=0;word<output.size();++word) {
            uint32_t wanted=Sentinel;
            if(word>=GuardWords&&word<GuardWords+Width*Height*2)
                wanted=std::bit_cast<uint32_t>((word-GuardWords)%2==0?
                    (mode==1?float(viewFloor)/256.0f:16.0f*std::max(float(viewFloor)/256.0f,mode==2?.5f:mode==3?2.0f:0.0f)):0.0f);
            Require(output[word]==wanted,"Public pixel LOD mode="+std::to_string(mode)+" floor="+std::to_string(viewFloor)+" word="+std::to_string(word)+
                " actual="+std::to_string(output[word])+" expected="+std::to_string(wanted));
        }
        for(size_t i=0;i<pixels.size();++i) {
            const auto wanted=i>=256&&i<256+Width*Height*4?std::byte{255}:std::byte{0x7b};
            Require(pixels[i]==wanted,"Public pixel LOD framebuffer/guard byte="+std::to_string(i));
        }
        Require(texture==originalTexture&&vertices==originalVertices&&pixelCode==originalCode&&commands==originalCommands&&pixelHeader==originalHeader&&
            std::memcmp(&packet,&originalPacket,sizeof(packet))==0,"Public pixel LOD changed read-only shader, inputs or PM4 bytes");
        std::cout<<"Public pixel LOD mode="<<mode<<": 2048 exact float pairs, full framebuffer, input padding and guards passed\n";
    }
};

}

void DrawSubmittedGuest(id<MTLDevice> device, id<MTLLibrary> library) {
    constexpr std::uint64_t ComputeCodeAddress = 0x800000, ComputeInputAddress = 0x810000,
        ComputeOutputAddress = 0x820000, LabelAddress = 0x900000;
    constexpr std::uint64_t GraphicsCommandAddress = 0xa00000, ComputeCommandAddress = 0xa10000,
        SecondCommandAddress = 0xa20000, IndirectAddress = 0xa30000, PacketAddress = 0xb00000,
        IndirectDrawCommandAddress = 0xa40000, ArgumentAddress = 0xc00000, QueryAllocation = 0xd00000;
    std::vector<std::array<float, 4>> vertices(65536);
    std::copy(Triangle.begin(), Triangle.end(), vertices.begin());
    std::fill(vertices.begin() + 3, vertices.end(), std::array<float, 4>{8, 8, 0.5f, 1});
    const auto originalVertices = vertices;
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
    std::array<std::uint32_t, 9> vertexCode{};
    std::copy(VertexCode.begin(), VertexCode.end(), vertexCode.begin());
    vertexCode[1] = 0x80020005;
    std::array<std::uint32_t, 32> fragmentCode{};
    std::copy(MaskedPixelCode.begin(), MaskedPixelCode.end(), fragmentCode.begin());
    std::array<std::uint32_t, 17> computeCode{
        0x34020082, 0xe0302000, 0x80000401, 0xbf8c3f70, 0x7e000000, 0x7e003600, 0x7e008200, 0x4a080881,
        0xd5800000, 0x00000000, 0xd59b0000, 0x00000000, 0xd5c10000, 0x00000000, 0xe0702000, 0x80010401, 0xbf810000};
    std::array<std::uint32_t, 8 + 64 + 9> computeStorage;
    computeStorage.fill(0xa6a6a6a6);
    std::copy_n(computeCode.begin(), 8, computeStorage.begin());
    std::copy(computeCode.begin() + 8, computeCode.end(), computeStorage.begin() + 72);
    const auto originalComputeStorage = computeStorage;
    std::array<std::uint32_t, 64 * 4> input, output;
    input.fill(0xa5a5a5a5);
    for (std::uint32_t i = 0; i < 64; ++i) input[i * 4] = i * 3 + 7;
    const auto originalInput = input;
    output.fill(0xdeadbeef);
    std::array<std::uint32_t, 4> labels{0, 0, 0xcafef00d, 0x12345678};
    const auto makeHeader = [](std::uint64_t address, std::uint64_t codeAddress, std::uint32_t bytes, std::uint8_t type) {
        std::array<std::byte, sizeof(Shader) + sizeof(ShaderUserData)> data{};
        Shader header{};
        header.file_header = 0x34333231;
        header.version = 0x18;
        header.user_data = reinterpret_cast<ShaderUserData*>(address + sizeof(Shader));
        header.code = reinterpret_cast<const volatile void*>(codeAddress);
        header.header_size = static_cast<std::uint32_t>(data.size());
        header.shader_size = bytes;
        header.type = type;
        std::memcpy(data.data(), &header, sizeof(header));
        return data;
    };
    auto vertexHeader = makeHeader(0x700000, 0x500000, sizeof(VertexCode), 2);
    auto fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(MaskedPixelCode), 1);
    auto computeHeader = makeHeader(0x720000, ComputeCodeAddress, sizeof(computeCode), 0);
    const auto append = [](std::vector<std::uint32_t>& commands, std::uint32_t opcode,
                           std::initializer_list<std::uint32_t> payload) {
        commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
        commands.insert(commands.end(), payload.begin(), payload.end());
    };
    auto graphics = GraphicsCommands();
    append(graphics, 0x3c, {0x13, static_cast<std::uint32_t>(LabelAddress), 0, 1, 0xffffffff, 0x190});
    append(graphics, 0x22, {static_cast<std::uint32_t>(LabelAddress), 0, 0, 4});
    append(graphics, 0x3f, {static_cast<std::uint32_t>(IndirectAddress), 0, 3});
    append(graphics, 0x49, {0, (1u << 29u) | (1u << 24u), static_cast<std::uint32_t>(LabelAddress + 4), 0, 2, 0, 0});
    std::array<std::uint32_t, 3> indirect{0xc0012d00, 3, 2};
    std::vector<std::uint32_t> compute;
    const std::array<std::uint32_t, 3> threads{64, 1, 1};
    const std::array<std::uint32_t, 2> computeProgram{static_cast<std::uint32_t>(ComputeCodeAddress >> 8u), 0};
    const std::array<std::uint32_t, 1> resources{16};
    const std::array<std::uint32_t, 8> users{static_cast<std::uint32_t>(ComputeInputAddress), 4u << 16u, 256, 0x01016fac,
        static_cast<std::uint32_t>(ComputeOutputAddress), 4u << 16u, 256, 0x01016fac};
    RegisterPacket(compute, 0x76, 0x207, threads);
    RegisterPacket(compute, 0x76, 0x20c, computeProgram);
    RegisterPacket(compute, 0x76, 0x213, resources);
    RegisterPacket(compute, 0x76, 0x240, users);
    append(compute, 0x15, {1, 1, 1, 0x8041});
    append(compute, 0x49, {0, 1u << 29u, static_cast<std::uint32_t>(LabelAddress), 0, 1, 0, 0});
    std::array<std::uint32_t, 3> second{0xc0012d00, 3, 2};
    std::array<std::uint32_t, 256> indirectCommands{};
    std::array<std::uint32_t, 80> arguments{};
    std::array<std::uint16_t, 4> indices{0, 3, 4, 5};
    const auto originalIndices = indices;
    constexpr std::uint64_t QueryGuard = 0x9192939495969798, Ready = 1ull << 63u;
    std::array<std::uint64_t, 36> queries;
    queries.fill(QueryGuard);
    auto expectedQueries = queries;
    constexpr std::uint64_t Restart16Address = 0xe00000, Restart32Address = 0xe10000, VertexCounterAddress = 0xe20004;
    std::array<std::uint16_t, 11> restart16{0xffff, 0, 1, 2, 3, 0xffff, 4, 5, 6, 7, 0xffff};
    std::array<std::uint32_t, 11> restart32{0xffffffff, 0, 1, 2, 3, 0xffffffff, 4, 5, 6, 7, 0xffffffff};
    std::array<std::uint32_t, 3> vertexCounter{0xcafef00d, 0, 0xdeadbeef};
    std::uint64_t finalSamples = 384, depthRangeGeneration = 0;
    constexpr std::uint64_t DepthAllocation = 0x1000000, DepthOutputAllocation = 0x1800000, DepthOutputAddress = DepthOutputAllocation + 256;
    std::array<std::array<std::byte, 65536>, 8> depthBacking;
    for (auto& plane : depthBacking) plane.fill(std::byte{0xa5});
    std::array<std::byte, 16> unrelatedDepthStorage;
    unrelatedDepthStorage.fill(std::byte{0x6e});
    std::array<std::uint32_t, Width * Height + 128> depthOutput;
    depthOutput.fill(0xdeadbeef);
    std::array<::Packet, 4> packets{{
        {reinterpret_cast<std::uint32_t*>(GraphicsCommandAddress), static_cast<std::uint32_t>(graphics.size()), 0, {}},
        {reinterpret_cast<std::uint32_t*>(ComputeCommandAddress), static_cast<std::uint32_t>(compute.size()), 0, {}},
        {reinterpret_cast<std::uint32_t*>(SecondCommandAddress), static_cast<std::uint32_t>(second.size()), 0, {}},
        {reinterpret_cast<std::uint32_t*>(IndirectDrawCommandAddress), 0, 0, {}}}};
    std::vector<AgcDriver::NativeGuestMemory::BorrowedRange> ranges{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false}, {ColorAllocation, pixels, true},
        {0x500000, std::as_writable_bytes(std::span(vertexCode)), false},
        {0x600000, std::as_writable_bytes(std::span(fragmentCode)), false},
        {ComputeCodeAddress, std::as_writable_bytes(std::span(computeStorage).first(8)), false},
        {ComputeCodeAddress + 8 * sizeof(std::uint32_t), std::as_writable_bytes(std::span(computeStorage).subspan(72)), false},
        {0x700000, vertexHeader, false}, {0x710000, fragmentHeader, false}, {0x720000, computeHeader, false},
        {ComputeInputAddress, std::as_writable_bytes(std::span(input)), false},
        {ComputeOutputAddress, std::as_writable_bytes(std::span(output)), true},
        {LabelAddress, std::as_writable_bytes(std::span(labels)), true},
        {GraphicsCommandAddress, std::as_writable_bytes(std::span(graphics)), false},
        {ComputeCommandAddress, std::as_writable_bytes(std::span(compute)), false},
        {SecondCommandAddress, std::as_writable_bytes(std::span(second)), false},
        {IndirectAddress, std::as_writable_bytes(std::span(indirect)), false},
        {PacketAddress, std::as_writable_bytes(std::span(packets)), false},
        {IndirectDrawCommandAddress, std::as_writable_bytes(std::span(indirectCommands)), false},
        {ArgumentAddress, std::as_writable_bytes(std::span(arguments)), false},
        {IndexAddress, std::as_writable_bytes(std::span(indices)), false},
        {QueryAllocation, std::as_writable_bytes(std::span(queries)), true},
        {Restart16Address, std::as_writable_bytes(std::span(restart16)), false},
        {Restart32Address, std::as_writable_bytes(std::span(restart32)), false},
        {VertexCounterAddress - 4, std::as_writable_bytes(std::span(vertexCounter)), true}};
    for (std::size_t i = 0; i < depthBacking.size(); ++i)
        ranges.push_back({DepthAllocation + i * 65536, depthBacking[i], false});
    ranges.push_back({DepthOutputAllocation, std::as_writable_bytes(std::span(depthOutput)), true});
    SubmittedFanReplay fanReplay;
    fanReplay.AddRanges(ranges);
    MinimumLodPixel::Replay pixelLod;
    pixelLod.AddRanges(ranges);
    MinimumLodCompare::Replay compareLod;
    compareLod.AddRanges(ranges);
    const auto checkPixels = [&](bool masked) {
        for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
            auto expected = std::byte{0x7b};
            if (offset >= 256 && offset < 256 + Width * Height * 4) {
                const auto x = ((offset - 256) / 4) % Width;
                expected = !masked || x % 2 == 0 ? std::byte{255} : std::byte{0x40};
            }
            Require(pixels[offset] == expected, "Public submitted actual RDNA draw produced wrong target byte " + std::to_string(offset));
        }
    };
    std::atomic<std::uint32_t> interrupts{0};
    AgcDriver::Metal::MetalDriver::Get().Configure((__bridge void*)device, (__bridge void*)library, ranges,
        [&](std::uint32_t queue) {
            Require(queue == 0 && labels[0] == 1 && labels[1] == 2, "EOP callback ran before cross-queue label completion");
            for (std::uint32_t i = 0; i < output.size(); ++i) {
                const auto expected = i % 4 == 0 ? (i / 4) * 3 + 8 : 0xdeadbeefu;
                Require(output[i] == expected, "EOP callback observed unfinished actual RDNA compute copyback");
            }
            checkPixels(true);
            interrupts.fetch_add(1);
        });
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x700000));
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress), 0);
    indirect[1] = 0;
    std::fill(graphics.begin(), graphics.end(), 0x80000000);
    std::copy(FullPixelCode.begin(), FullPixelCode.end(), fragmentCode.begin());
    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(FullPixelCode), 1);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
    AgcDriverWaitIdle_nid_postfix();
    Require(interrupts.load() == 1, "Public submitted draw did not deliver exactly one completed EOP callback");
    checkPixels(true);
    Require(vertices == originalVertices && input == originalInput && computeStorage == originalComputeStorage &&
        labels[2] == 0xcafef00d && labels[3] == 0x12345678,
        "Public submission changed borrowed input or label guards");
    std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
    AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress + 2 * sizeof(::Packet)), 0);
    AgcDriverWaitIdle_nid_postfix();
    checkPixels(false);
    Require(interrupts.load() == 1 && vertices == originalVertices && input == originalInput,
        "Persistent queue submission modified input or redelivered EOP");
    const auto submitIndirect = [&](const std::vector<std::uint32_t>& words, std::uint8_t targetByte, const std::string& name) {
        Require(words.size() <= indirectCommands.size(), "Indirect replay command allocation is too small");
        std::copy(words.begin(), words.end(), indirectCommands.begin());
        packets[3].dw_num = static_cast<std::uint32_t>(words.size());
        std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
        const auto originalArguments = arguments;
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress + 3 * sizeof(::Packet)), 0);
        AgcDriverWaitIdle_nid_postfix();
        for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
            const auto expected = offset >= 256 && offset < 256 + Width * Height * 4 ? std::byte{targetByte} : std::byte{0x7b};
            Require(pixels[offset] == expected, name + " produced wrong target byte " + std::to_string(offset));
        }
        Require(indices == originalIndices && arguments == originalArguments, name + " modified borrowed index or argument records");
        std::cout << name << " passed\n";
    };
    std::vector<std::uint32_t> signedCommands;
    append(signedCommands, 0x26, {static_cast<std::uint32_t>(IndexAddress), 0});
    append(signedCommands, 0x2a, {0});
    append(signedCommands, 0x13, {4});
    const std::array<std::uint32_t, 1> negativeBase{0xfffffffdu};
    RegisterPacket(signedCommands, 0x79, 0x24a, negativeBase);
    append(signedCommands, 0x35, {4, 1, 3, 0});
    submitIndirect(signedCommands, 255, "Actual public indexed draw: GE_INDX_OFFSET -3 with raw indices3/4/5");
    std::copy(VertexCode.begin(), VertexCode.end(), vertexCode.begin() + 3);
    vertexCode[0] = 0x4a0a0a0c;
    vertexCode[1] = 0x4a0a0a0d;
    vertexCode[2] = 0x4a0a0a0e;
    vertexCode[4] = 0x80020005;
    vertexHeader = makeHeader(0x700000, 0x500000, sizeof(vertexCode), 2);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x700000));
    std::vector<std::uint32_t> setup;
    const std::array<std::uint32_t, 1> zeroBase{0}, sevenUsers{14};
    const std::array<std::uint32_t, 7> indirectUsers{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 6, 0x01016fac, 5, 5, 5};
    RegisterPacket(setup, 0x79, 0x24a, zeroBase);
    RegisterPacket(setup, 0x76, 0x08b, sevenUsers);
    RegisterPacket(setup, 0x76, 0x08c, indirectUsers);
    append(setup, 0x11, {1, static_cast<std::uint32_t>(ArgumentAddress), 0});
    std::fill(vertices.begin(), vertices.end(), std::array<float, 4>{8, 8, 0.5f, 1});
    std::copy(Triangle.begin(), Triangle.end(), vertices.begin() + 3);
    arguments.fill(0);
    const std::array<std::uint32_t, 4> singleRecord{3, 1, 1, 2};
    std::copy(singleRecord.begin(), singleRecord.end(), arguments.begin() + 4);
    auto single = setup;
    const std::array<std::uint32_t, 1> zeroDrawIndex{0};
    RegisterPacket(single, 0x76, 0x092, zeroDrawIndex);
    append(single, 0x24, {16, 0x90, 0x91, 2});
    submitIndirect(single, 255, "Actual public DRAW_INDIRECT: SET_BASE offset and guest SGPR base/start");
    std::fill(vertices.begin(), vertices.end(), std::array<float, 4>{8, 8, 0.5f, 1});
    std::copy(Triangle.begin(), Triangle.end(), vertices.begin());
    arguments.fill(0);
    const std::array<std::uint32_t, 5> indexedRecord{99, 1, 1, 0xfffffffdu, 0};
    std::copy(indexedRecord.begin(), indexedRecord.end(), arguments.begin() + 4);
    std::vector<std::uint32_t> indexedSingle;
    append(indexedSingle, 0x25, {16, 0x90, 0x91, 0});
    submitIndirect(indexedSingle, 255, "Actual public DRAW_INDEX_INDIRECT: firstIndex and declared index-count clamp");
    const std::array<std::uint32_t, 5> eighthPixel{0x7e0e02ff, 0x3e000000, 0xf800180f, 0x07070707, 0xbf810000};
    std::copy(eighthPixel.begin(), eighthPixel.end(), fragmentCode.begin());
    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(eighthPixel), 1);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    std::fill(vertices.begin(), vertices.end(), std::array<float, 4>{8, 8, 0.5f, 1});
    std::copy(Triangle.begin(), Triangle.end(), vertices.begin() + 3);
    arguments.fill(0);
    const std::array<std::array<std::uint32_t, 4>, 3> multiRecords{{{3, 1, 1, 2}, {3, 1, 0, 2}, {3, 1, 1, 0}}};
    for (std::size_t i = 0; i < multiRecords.size(); ++i) {
        std::copy(multiRecords[i].begin(), multiRecords[i].end(), arguments.begin() + 4 + i * 8);
        std::fill_n(arguments.begin() + 8 + i * 8, 4, 0xabcdef01);
    }
    std::vector<std::uint32_t> multi;
    const std::array<std::uint32_t, 1> additiveBlend{0x40000101};
    RegisterPacket(multi, 0x69, 0x1e0, additiveBlend);
    append(multi, 0x2c, {16, 0x90, 0x91, 0x92u | (1u << 31u) | (1u << 30u), 2,
        static_cast<std::uint32_t>(ComputeOutputAddress), 0, 32, 2});
    submitIndirect(multi, 128, "Actual public DRAW_INDIRECT_MULTI: padded records, GPU-written count cap and guest drawIndex");
    std::fill(vertices.begin(), vertices.end(), std::array<float, 4>{8, 8, 0.5f, 1});
    std::copy(Triangle.begin(), Triangle.end(), vertices.begin());
    arguments.fill(0);
    const std::array<std::array<std::uint32_t, 5>, 3> indexedRecords{{{3, 0, 1, 0, 0}, {99, 1, 1, 0xfffffffcu, 0}, {3, 1, 4, 0, 0}}};
    for (std::size_t i = 0; i < indexedRecords.size(); ++i) {
        std::copy(indexedRecords[i].begin(), indexedRecords[i].end(), arguments.begin() + 4 + i * 8);
        std::fill_n(arguments.begin() + 9 + i * 8, 3, 0xabcdef01);
    }
    std::vector<std::uint32_t> indexedMulti;
    append(indexedMulti, 0x38, {16, 0x90, 0x91, 0x92u | (1u << 31u), 3, 0, 0, 32, 0});
    submitIndirect(indexedMulti, 96, "Actual public DRAW_INDEX_INDIRECT_MULTI: zero instances, clamped firstIndex and excluded out-of-range record");
    fanReplay.Run(pixels);
    vertices = originalVertices;
    std::copy(VertexCode.begin(), VertexCode.end(), vertexCode.begin());
    vertexCode[1] = 0x80020005;
    vertexHeader = makeHeader(0x700000, 0x500000, sizeof(VertexCode), 2);
    std::copy(FullPixelCode.begin(), FullPixelCode.end(), fragmentCode.begin());
    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(FullPixelCode), 1);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x700000));
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    const auto querySubmit = [&](const std::vector<std::uint32_t>& words) {
        Require(words.size() <= indirectCommands.size(), "Sample query replay command allocation is too small");
        std::copy(words.begin(), words.end(), indirectCommands.begin());
        packets[3].dw_num = static_cast<std::uint32_t>(words.size());
        AgcDriver::Submit(reinterpret_cast<const ::Packet*>(PacketAddress + 3 * sizeof(::Packet)), 0);
        AgcDriverWaitIdle_nid_postfix();
    };
    const auto checkQueries = [&](std::uint32_t lane, std::uint64_t total, const std::string& name) {
        for (std::uint32_t db = 0; db < 16; ++db) expectedQueries[2 + lane + db * 2] = Ready | (db == 0 ? total : 0);
        for (std::size_t i = 0; i < queries.size(); ++i)
            Require(queries[i] == expectedQueries[i], name + " produced wrong sparse query word " + std::to_string(i) +
                ": observed " + std::to_string(queries[i]) + ", expected " + std::to_string(expectedQueries[i]));
    };
    const auto checkQueryPixels = [&](bool masked) {
        for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
            auto expected = std::byte{0x7b};
            if (offset >= 256 && offset < 256 + Width * Height * 4) {
                expected = std::byte{0x40};
                const auto pixel = (offset - 256) / 4;
                if (pixel / 64 < 16 && pixel % 64 < 16 && (!masked || pixel % 2 == 0)) expected = std::byte{255};
            }
            Require(pixels[offset] == expected, "Actual sample query draw produced wrong target byte " + std::to_string(offset));
        }
    };
    const auto dump = [&](std::uint32_t lane) {
        std::vector<std::uint32_t> words;
        append(words, 0x46, {0x139, static_cast<std::uint32_t>(QueryAllocation + 16 + lane * 8), 0});
        return words;
    };
    for (const auto mode : {1u, 0u, 2u, 3u}) {
        pixelLod.Run(mode, 0);
        pixelLod.Run(mode);
    }
    for (const auto format : {22u, 7u}) {
        compareLod.Run(false, 0, format);
        compareLod.Run(false, 384, format);
        compareLod.Run(true, 384, format);
    }
    auto querySetup = GraphicsCommands();
    const std::array<std::uint32_t, 1> queryDimensions{(15u << 14u) | 15u}, queryRect{(16u << 16u) | 16u},
        queryScale{std::bit_cast<std::uint32_t>(8.0f)}, queryYScale{std::bit_cast<std::uint32_t>(-8.0f)}, disabledBlend{0};
    RegisterPacket(querySetup, 0x69, 0x3b0, queryDimensions);
    for (const auto offset : {0xdu, 0x82u, 0x91u, 0x95u}) RegisterPacket(querySetup, 0x69, offset, queryRect);
    for (const auto offset : {0x10fu, 0x110u, 0x112u}) RegisterPacket(querySetup, 0x69, offset, queryScale);
    RegisterPacket(querySetup, 0x69, 0x111, queryYScale);
    RegisterPacket(querySetup, 0x69, 0x1e0, disabledBlend);
    RegisterPacket(querySetup, 0x79, 0x24a, zeroBase);
    std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
    append(querySetup, 0x2d, {3, 2});
    const auto firstDump = dump(0);
    querySetup.insert(querySetup.end(), firstDump.begin(), firstDump.end());
    querySubmit(querySetup);
    checkQueryPixels(false);
    checkQueries(0, 0, "First EVENT_WRITE sample dump excludes preactivation draw");
    std::vector<std::uint32_t> countedDraw;
    append(countedDraw, 0x2d, {3, 2});
    const auto shiftedDump = dump(1);
    countedDraw.insert(countedDraw.end(), shiftedDump.begin(), shiftedDump.end());
    querySubmit(countedDraw);
    checkQueryPixels(false);
    checkQueries(1, 256, "Full 16x16 guest draw sample dump");
    querySubmit(shiftedDump);
    checkQueries(1, 256, "Repeated sample dump preserves cumulative total");
    std::copy(MaskedPixelCode.begin(), MaskedPixelCode.end(), fragmentCode.begin());
    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(MaskedPixelCode), 1);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
    std::vector<std::uint32_t> maskedQuery;
    append(maskedQuery, 0x2d, {3, 2});
    maskedQuery.insert(maskedQuery.end(), firstDump.begin(), firstDump.end());
    querySubmit(maskedQuery);
    checkQueryPixels(true);
    checkQueries(0, 384, "Masked guest fragment discard contributes128 visible samples");
    std::cout << "Actual public EVENT_WRITE samples: activation, hardware256+128 cumulative total, repeated dumps and16 sparse ready slots with neighbor guards passed\n";
    const std::array<std::uint32_t, 9> writingVertex{
        0xe0382000, 0x80020005, 0xbf8c3f70, 0x7e100281, 0xe0c80000,
        0x80030800, 0xf80008cf, 0x03020100, 0xbf810000};
    vertexCode = writingVertex;
    vertexHeader = makeHeader(0x700000, 0x500000, sizeof(writingVertex), 2);
    std::copy(FullPixelCode.begin(), FullPixelCode.end(), fragmentCode.begin());
    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(FullPixelCode), 1);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x700000));
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
    const auto effectSetup = [&](std::uint32_t primitive, bool restart, bool cullBoth) {
        auto words = GraphicsCommands();
        const auto reg = [&](std::uint32_t bank, std::uint32_t offset, std::uint32_t value) {
            RegisterPacket(words, bank, offset, std::span(&value, 1));
        };
        reg(0x79, 0x242, primitive);
        reg(0x79, 0x24a, 0);
        reg(0x79, 0x24b, restart ? 1 : 0);
        reg(0x69, 0x103, 0xffffffff);
        reg(0x69, 0x205, cullBoth ? 0x243 : 0x240);
        reg(0x76, 0x8b, 16);
        const std::array<std::uint32_t, 8> users{
            static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 65536, 0x01016fac,
            static_cast<std::uint32_t>(VertexCounterAddress), 0, 4, 0x31016fac};
        RegisterPacket(words, 0x76, 0x8c, users);
        append(words, 0x2f, {1});
        return words;
    };
    const auto checkEffects = [&](const std::vector<std::uint32_t>& words, std::optional<std::uint32_t> writes,
                                  std::uint64_t samples, std::uint32_t shape, const std::string& name) {
        vertexCounter[1] = 0;
        std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
        const auto savedVertices = vertices;
        const auto savedRestart16 = restart16;
        const auto savedRestart32 = restart32;
        auto commandsWithDump = words;
        const auto queryDump = dump(0);
        commandsWithDump.insert(commandsWithDump.end(), queryDump.begin(), queryDump.end());
        querySubmit(commandsWithDump);
        for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
            auto expected = std::byte{0x7b};
            if (offset >= 256 && offset < 256 + Width * Height * 4) {
                const auto pixel = (offset - 256) / 4;
                const auto x = pixel % Width, y = pixel / Width;
                const bool covered = shape == 1 ? y >= 8 && y < 24 &&
                    ((x >= 8 && x < 24) || (x >= 40 && x < 56)) :
                    shape == 2 ? y == 16 && x >= 8 && x < 24 : false;
                expected = covered ? std::byte{255} : std::byte{0x40};
            }
            Require(pixels[offset] == expected, name + " produced wrong target byte " + std::to_string(offset));
        }
        Require(!writes || vertexCounter[1] == *writes, name + " produced wrong actual VS atomic write count: observed " +
            std::to_string(vertexCounter[1]) + ", expected " + std::to_string(writes.value_or(0)));
        Require(vertexCounter[0] == 0xcafef00d && vertexCounter[2] == 0xdeadbeef &&
            vertices == savedVertices && restart16 == savedRestart16 && restart32 == savedRestart32,
            name + " modified borrowed vertex/index inputs or counter guards");
        finalSamples += samples;
        checkQueries(0, finalSamples, name);
        std::cout << name << " passed\n";
    };
    const std::array<std::array<float, 4>, 8> stripVertices{{{-.75f, -.5f, .5f, 1}, {-.25f, -.5f, .5f, 1},
        {-.75f, .5f, .5f, 1}, {-.25f, .5f, .5f, 1},
        {.25f, -.5f, .5f, 1}, {.75f, -.5f, .5f, 1},
        {.25f, .5f, .5f, 1}, {.75f, .5f, .5f, 1}}};
    std::copy(stripVertices.begin(), stripVertices.end(), vertices.begin());
    vertices[65535] = {0, 0, .5f, 1};
    for (const auto indexSize : {2u, 4u}) {
        auto words = effectSetup(6, true, false);
        append(words, 0x2a, {indexSize == 2 ? 0u : 1u});
        append(words, 0x27, {11, static_cast<std::uint32_t>(indexSize == 2 ? Restart16Address : Restart32Address), 0, 11, 0});
        checkEffects(words, std::nullopt, 512, 1, "Public restarted triangle strips UInt" + std::to_string(indexSize * 8) +
            ": leading/internal/trailing markers and disconnected pixel regions");
    }
    restart32.fill(0xffffffff);
    auto markers = effectSetup(6, true, false);
    append(markers, 0x2a, {1});
    append(markers, 0x27, {11, static_cast<std::uint32_t>(Restart32Address), 0, 11, 0});
    checkEffects(markers, 0, 0, 0, "Public all-marker strip preserves VS writes, pixels and sample total");
    vertices = originalVertices;
    auto emptyScissor = effectSetup(4, false, false);
    const std::uint32_t emptyEdge = 0;
    RegisterPacket(emptyScissor, 0x69, 0x91, std::span(&emptyEdge, 1));
    append(emptyScissor, 0x2f, {2});
    append(emptyScissor, 0x2d, {6, 2});
    checkEffects(emptyScissor, 12, 0, 0, "Public empty scissor retains twelve actual guest VS writes");
    auto culledTriangles = effectSetup(4, false, true);
    append(culledTriangles, 0x2f, {2});
    append(culledTriangles, 0x2d, {6, 2});
    checkEffects(culledTriangles, 12, 0, 0, "Public cullboth triangles retain twelve actual guest VS writes");
    vertices[0] = {-.75f, -.03125f, .5f, 1};
    vertices[1] = {-.25f, -.03125f, .5f, 1};
    auto visibleLine = effectSetup(2, false, true);
    append(visibleLine, 0x2d, {2, 2});
    checkEffects(visibleLine, 2, 16, 2, "Public cullboth line retains visible pixels and two actual guest VS writes");
    vertices = originalVertices;
    std::copy(VertexCode.begin(), VertexCode.end(), vertexCode.begin());
    vertexCode[1] = 0x80020005;
    vertexHeader = makeHeader(0x700000, 0x500000, sizeof(VertexCode), 2);
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x700000));
    constexpr std::array<std::uint32_t, 14> depthBitsPixel{
        0x7e080f00, 0x7e0a0f01, 0x340c0a86, 0x4a0c0d04, 0x340c0c82,
        0xf0000188, 0x00000804, 0xbf8c3f70, 0xe0701000, 0x80020806,
        0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000};
    for (std::uint32_t format = 0; format < 4; ++format) {
        const bool d16 = format < 2, stencil = (format & 1u) != 0;
        const auto depthAddress = DepthAllocation + format * 131072;
        const auto stencilAddress = stencil ? depthAddress + 65536 : 0;
        std::copy(FullPixelCode.begin(), FullPixelCode.end(), fragmentCode.begin());
        fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(FullPixelCode), 1);
        AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
        for (std::uint32_t band = 0; band < 3; ++band) {
            const float z = band == 0 ? 0.0f : band == 1 ? 0.5f : 1.0f;
            for (std::uint32_t vertex = 0; vertex < 3; ++vertex) vertices[vertex][2] = z;
            auto writer = GraphicsCommands();
            const auto reg = [&](std::uint32_t offset, std::uint32_t value) {
                RegisterPacket(writer, 0x69, offset, std::span(&value, 1));
            };
            reg(0x0, 0); reg(0x2, 0); reg(0x7, ((Height - 1) << 16u) | (Width - 1));
            reg(0xa, 17); reg(0xb, std::bit_cast<std::uint32_t>(0.75f));
            reg(0x10, d16 ? 1 : 3); reg(0x11, stencil ? 1 : 0);
            reg(0x12, static_cast<std::uint32_t>(depthAddress >> 8u));
            reg(0x14, static_cast<std::uint32_t>(depthAddress >> 8u));
            reg(0x13, static_cast<std::uint32_t>(stencilAddress >> 8u));
            reg(0x15, static_cast<std::uint32_t>(stencilAddress >> 8u));
            reg(0x200, 0x76);
            reg(0x90, 0x80000000u | (band == 0 ? 0 : band == 1 ? 16 : 48));
            reg(0x91, (Height << 16u) | (band == 0 ? 16 : band == 1 ? 48 : 64));
            const std::uint32_t zero = 0;
            RegisterPacket(writer, 0x79, 0x24a, std::span(&zero, 1));
            RegisterPacket(writer, 0x79, 0x24b, std::span(&zero, 1));
            append(writer, 0x2d, {3, 2});
            querySubmit(writer);
        }
        finalSamples += Width * Height;
        vertices = originalVertices;
        const std::array<std::uint32_t, 2> guestFormats{d16 ? 11u : 20u, 21u};
        for (const auto guestFormat : std::span(guestFormats).first(d16 ? 1 : 2)) {
            std::copy(depthBitsPixel.begin(), depthBitsPixel.end(), fragmentCode.begin());
            fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(depthBitsPixel), 1);
            AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
            auto reader = GraphicsCommands();
            const std::array<std::uint32_t, 12> depthUsers{
                static_cast<std::uint32_t>(depthAddress >> 8u), (guestFormat << 20u) | (3u << 30u),
                ((Width - 1) >> 2u) | ((Height - 1) << 14u), 0xfacu | (24u << 20u) | (9u << 28u), 0, 0, 0, 0,
                static_cast<std::uint32_t>(DepthOutputAddress), 0, Width * Height * 4, 0x01016fac};
            const std::uint32_t twelveUsers = 24, zero = 0;
            RegisterPacket(reader, 0x76, 0xb, std::span(&twelveUsers, 1));
            RegisterPacket(reader, 0x76, 0xc, depthUsers);
            RegisterPacket(reader, 0x79, 0x24a, std::span(&zero, 1));
            RegisterPacket(reader, 0x79, 0x24b, std::span(&zero, 1));
            append(reader, 0x2d, {3, 2});
            const auto queryDump = dump(0);
            reader.insert(reader.end(), queryDump.begin(), queryDump.end());
            const auto readDepth = [&](bool cleared, const std::string& name) {
                depthOutput.fill(0xdeadbeef);
                std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
                querySubmit(reader);
                checkPixels(false);
                for (std::size_t i = 0; i < depthOutput.size(); ++i) {
                    std::uint32_t expected = 0xdeadbeef;
                    if (i >= 64 && i < 64 + Width * Height) {
                        const auto x = (i - 64) % Width;
                        expected = cleared ? (d16 ? 49151 : 0x3f400000) : x < 16 ? 0 : x < 48 ? (d16 ? 32768 : 0x3f000000) : (d16 ? 65535 : 0x3f800000);
                    }
                    Require(depthOutput[i] == expected, name +
                        " produced wrong guest DWORD " + std::to_string(i) + ": observed " + std::to_string(depthOutput[i]) +
                        ", expected " + std::to_string(expected));
                }
                for (const auto& plane : depthBacking) for (const auto value : plane)
                    Require(value == std::byte{0xa5}, "Public retained depth reader modified borrowed depth/stencil shadow bytes");
                Require(vertices == originalVertices, "Public retained depth reader modified borrowed vertex inputs");
                finalSamples += Width * Height;
                checkQueries(0, finalSamples, name + " hardware samples");
            };
            readDepth(false, "Public retained depth bits format" + std::to_string(format));
            std::cout << "Public retained " << (d16 ? "D16" : "D32") << (stencil ? "S8" : "")
                      << " guestformat" << guestFormat << " actual RDNA depth writer and integer IMAGE_LOAD reader exact bits with guards passed\n";
            if (format == 0) {
                const auto reacquireReadOnlyDepth = [&] {
                    std::copy(FullPixelCode.begin(), FullPixelCode.end(), fragmentCode.begin());
                    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(FullPixelCode), 1);
                    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
                    auto reacquire = GraphicsCommands();
                    const std::uint32_t readOnlyDepth = 0x72, zero = 0;
                    RegisterPacket(reacquire, 0x69, 0x200, std::span(&readOnlyDepth, 1));
                    RegisterPacket(reacquire, 0x79, 0x24a, std::span(&zero, 1));
                    RegisterPacket(reacquire, 0x79, 0x24b, std::span(&zero, 1));
                    append(reacquire, 0x2d, {3, 2});
                    querySubmit(reacquire);
                    finalSamples += Width * Height;
                    std::copy(depthBitsPixel.begin(), depthBitsPixel.end(), fragmentCode.begin());
                    fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(depthBitsPixel), 1);
                    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(0x710000));
                };
                auto replacement = ranges;
                replacement.push_back({0x1900000, unrelatedDepthStorage, false});
                AgcDriver::Metal::MetalDriver::Get().ReplaceBorrowedRanges(replacement, ++depthRangeGeneration);
                ranges = replacement;
                reacquireReadOnlyDepth();
                readDepth(false, "Public depth cache survives an unrelated borrowed mapping");
                const auto whole = std::find_if(replacement.begin(), replacement.end(), [&](const auto& range) {
                    return range.guestAddress == depthAddress;
                });
                Require(whole != replacement.end(), "Depth split fixture could not find its existing borrow");
                const auto tail = AgcDriver::NativeGuestMemory::BorrowedRange{
                    depthAddress + 32768, whole->host.subspan(32768), whole->writable, whole->identity};
                whole->host = whole->host.first(32768);
                replacement.push_back(tail);
                AgcDriver::Metal::MetalDriver::Get().ReplaceBorrowedRanges(replacement, ++depthRangeGeneration);
                ranges = replacement;
                reacquireReadOnlyDepth();
                readDepth(false, "Public depth cache survives an equivalent split borrow");
                for (auto& range : replacement)
                    if (range.guestAddress >= depthAddress && range.guestAddress < depthAddress + 65536) range.identity = 1;
                AgcDriver::Metal::MetalDriver::Get().ReplaceBorrowedRanges(replacement, ++depthRangeGeneration);
                ranges = replacement;
                reacquireReadOnlyDepth();
                readDepth(true, "Public depth identity replacement reacquires initial clear instead of stale written depth");
                for (const auto value : unrelatedDepthStorage)
                    Require(value == std::byte{0x6e}, "Depth cache replacement modified unrelated borrowed bytes");
                std::cout << "Public depth cache replacement: unrelated addition, equivalent split and same-host new identity passed\n";
            }
        }
    }
    AgcDriverShutdown_nid_postfix();
    std::cout << "Actual public AGC Submit: shader registration snapshots, immutable flattened IB, cross-queue compute WAIT/conditional draw, completed EOP and persistent registers passed\n";
    output.fill(0xdeadbeef);
    compute[compute.size() - 6] |= 1u << 24u;
    {
        std::array<std::uint32_t, 64 * 4> nextInput, nextOutput, rejectedInput, rejectedOutput;
        nextInput.fill(0xb5b5b5b5);
        rejectedInput.fill(0xc5c5c5c5);
        nextOutput.fill(0xdeadbeef);
        rejectedOutput.fill(0xdeadbeef);
        for (std::uint32_t i = 0; i < 64; ++i) {
            nextInput[i * 4] = i * 5 + 31;
            rejectedInput[i * 4] = i * 7 + 59;
        }
        const auto originalNextInput = nextInput, originalRejectedInput = rejectedInput;
        std::array<std::uint32_t, 4> addedStorage{0xcafef00d, 11, 22, 0xdeadbeef};
        const auto originalAddedStorage = addedStorage;
        auto replacement = ranges;
        const auto pointData = [&](auto& borrows, auto& source, auto& destination) {
            for (auto& range : borrows) {
                if (range.guestAddress == ComputeInputAddress) range.host = std::as_writable_bytes(std::span(source));
                if (range.guestAddress == ComputeOutputAddress) range.host = std::as_writable_bytes(std::span(destination));
            }
        };
        pointData(replacement, nextInput, nextOutput);
        std::array<std::uint32_t, 64 * 4>* activeInput = &input;
        std::array<std::uint32_t, 64 * 4>* activeOutput = &output;
        std::uint32_t increment = 1;
        std::atomic<std::uint32_t> rangeInterrupts{0};
        std::mutex rangeCallbackMutex;
        std::condition_variable rangeCallbackChanged;
        bool rangeCallbackEntered = false, rangeCallbackReleased = false, blockRangeCallback = true;
        AgcDriver::Metal::MetalDriver rangeDriver;
        const auto checkOutput = [&] {
            for (std::uint32_t i = 0; i < activeOutput->size(); ++i) {
                const auto expected = i % 4 == 0 ? (*activeInput)[i] + increment : 0xdeadbeefu;
                Require((*activeOutput)[i] == expected,
                    "Borrow replacement actual RDNA output or padding is incorrect at word " + std::to_string(i) +
                    ", actual=" + std::to_string((*activeOutput)[i]) + ", expected=" + std::to_string(expected));
            }
        };
        rangeDriver.Configure((__bridge void*)device, (__bridge void*)library, ranges,
            [&](std::uint32_t queue) {
                Require(queue == 0x20, "Borrow replacement EOP arrived from the wrong queue");
                checkOutput();
                rangeInterrupts.fetch_add(1);
                std::unique_lock lock(rangeCallbackMutex);
                if (blockRangeCallback) {
                    rangeCallbackEntered = true;
                    rangeCallbackChanged.notify_all();
                    rangeCallbackChanged.wait(lock, [&] { return rangeCallbackReleased; });
                }
            }, 0);
        rangeDriver.RegisterShader(reinterpret_cast<const Shader*>(0x720000));
        output.fill(0xdeadbeef);
        rangeDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
        {
            std::unique_lock lock(rangeCallbackMutex);
            if (!rangeCallbackChanged.wait_for(lock, std::chrono::seconds(10), [&] { return rangeCallbackEntered; })) {
                rangeCallbackReleased = true;
                rangeCallbackChanged.notify_all();
                throw std::runtime_error("Borrow replacement EOP did not reach the old mapping");
            }
        }
        std::promise<void> replacingStarted;
        auto startedReplacing = replacingStarted.get_future();
        auto replacing = std::async(std::launch::async, [&] {
            replacingStarted.set_value();
            rangeDriver.ReplaceBorrowedRanges(replacement, 1);
        });
        startedReplacing.wait();
        const bool drainedBeforeReplacing = replacing.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
        const auto completedOldOutput = output;
        {
            std::lock_guard lock(rangeCallbackMutex);
            rangeCallbackReleased = true;
            blockRangeCallback = false;
        }
        rangeCallbackChanged.notify_all();
        replacing.get();
        Require(drainedBeforeReplacing, "Borrow replacement returned while the EOP worker still retained old storage");
        Require(rangeInterrupts.load() == 1 && input == originalInput && output == completedOldOutput,
            "Borrow replacement changed old input/output before retiring its completed worker");
        activeInput = &nextInput;
        activeOutput = &nextOutput;
        const auto submitReplacement = [&] {
            activeOutput->fill(0xdeadbeef);
            const auto before = rangeInterrupts.load();
            rangeDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
            rangeDriver.WaitIdle();
            Require(rangeInterrupts.load() == before + 1, "Borrow replacement did not deliver exactly one completed EOP");
            checkOutput();
            Require(input == originalInput && output == completedOldOutput && nextInput == originalNextInput &&
                rejectedInput == originalRejectedInput && addedStorage == originalAddedStorage,
                "Borrow replacement modified old, read-only or unrelated storage");
        };
        submitReplacement();
        const auto rejectReplacement = [&](const auto& invalid, std::uint64_t generation) {
            bool refused = false;
            try { rangeDriver.ReplaceBorrowedRanges(invalid, generation); }
            catch (const std::exception&) { refused = true; }
            Require(refused, "Borrow replacement accepted invalid ranges or a nonmonotonic generation");
        };
        auto invalid = replacement;
        pointData(invalid, rejectedInput, rejectedOutput);
        invalid.push_back({ComputeInputAddress + 4, std::as_writable_bytes(std::span(addedStorage)), true});
        rejectReplacement(invalid, 2);
        invalid.pop_back();
        rejectReplacement(invalid, 1);
        submitReplacement();
        Require(std::all_of(rejectedOutput.begin(), rejectedOutput.end(), [](auto word) { return word == 0xdeadbeef; }),
            "Rejected borrow replacement partially published the future destination");
        computeStorage[7] = 0x4a080882;
        replacement.push_back({0xe30000, std::as_writable_bytes(std::span(addedStorage)), true});
        rangeDriver.ReplaceBorrowedRanges(replacement, 2);
        submitReplacement();
        for (auto& range : replacement) {
            if (range.guestAddress == ComputeCodeAddress || range.guestAddress == ComputeCodeAddress + 8 * sizeof(std::uint32_t)) {
                range.identity = 1;
            }
        }
        rangeDriver.ReplaceBorrowedRanges(replacement, 3);
        increment = 2;
        submitReplacement();
        for (auto& range : replacement) {
            if (range.guestAddress == ComputeOutputAddress) range.writable = false;
        }
        rangeDriver.ReplaceBorrowedRanges(replacement, 4);
        nextOutput.fill(0xdeadbeef);
        const auto interruptsBeforePermission = rangeInterrupts.load();
        bool permissionRefused = false;
        try {
            rangeDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
            rangeDriver.WaitIdle();
        } catch (const std::exception& error) {
            permissionRefused = std::string(error.what()).find("read-only") != std::string::npos;
            if (!permissionRefused) throw;
        }
        try { rangeDriver.Shutdown(); } catch (...) { if (!permissionRefused) throw; }
        Require(permissionRefused && rangeInterrupts.load() == interruptsBeforePermission &&
            std::all_of(nextOutput.begin(), nextOutput.end(), [](auto word) { return word == 0xdeadbeef; }) &&
            nextInput == originalNextInput && input == originalInput && output == completedOldOutput,
            "Read-only replacement did not reject the actual guest write atomically before EOP");
        computeStorage = originalComputeStorage;
        output.fill(0xdeadbeef);
        std::cout << "Actual PM4 borrow replacement: old EOP drain, same-VA new storage, invalid publication atomicity, add-only shader capture, allocation identity and read-only writes passed\n";
    }
    struct TransactionStorage {
        std::array<std::uint32_t, 64 * 4> input;
        std::array<std::uint32_t, 64 * 4> output;
        std::array<std::uint32_t, 8 + 64 + 9> code;
    };
    const auto transactionStorage = [&] {
        auto storage = std::make_shared<TransactionStorage>();
        storage->input = originalInput;
        storage->output.fill(0xdeadbeef);
        storage->code = originalComputeStorage;
        return storage;
    };
    const auto transactionRanges = [&](const auto& storage, std::uint64_t identity) {
        auto mapped = ranges;
        for (auto& range : mapped) {
            if (range.guestAddress == ComputeInputAddress) range.host = std::as_writable_bytes(std::span(storage->input));
            if (range.guestAddress == ComputeOutputAddress) range.host = std::as_writable_bytes(std::span(storage->output));
            if (range.guestAddress == ComputeCodeAddress) range.host = std::as_writable_bytes(std::span(storage->code).first(8));
            if (range.guestAddress == ComputeCodeAddress + 8 * sizeof(std::uint32_t))
                range.host = std::as_writable_bytes(std::span(storage->code).subspan(72));
            if (range.guestAddress == ComputeInputAddress || range.guestAddress == ComputeOutputAddress ||
                range.guestAddress == ComputeCodeAddress || range.guestAddress == ComputeCodeAddress + 8 * sizeof(std::uint32_t))
                range.identity = identity;
        }
        return mapped;
    };
    const auto checkTransactionOutput = [&](const TransactionStorage& storage, std::uint32_t increment) {
        for (std::uint32_t i = 0; i < storage.output.size(); ++i) {
            const auto expected = i % 4 == 0 ? storage.input[i] + increment : 0xdeadbeefu;
            Require(storage.output[i] == expected, "CPU mapping transaction produced incorrect RDNA output or padding at word " +
                std::to_string(i) + ", actual=" + std::to_string(storage.output[i]) + ", expected=" + std::to_string(expected));
        }
    };
    {
        auto previousOwner = transactionStorage(), nextOwner = transactionStorage();
        std::weak_ptr<TransactionStorage> previousWeak = previousOwner, nextWeak = nextOwner;
        const auto mapped = transactionRanges(previousOwner, 1);
        auto replacement = transactionRanges(nextOwner, 2);
        std::atomic<std::uint32_t> transactionInterrupts{0}, mutations{0};
        std::mutex transactionMutex;
        std::condition_variable transactionChanged;
        bool oldEopEntered = false, oldEopReleased = false, mutationEntered = false, mutationReleased = false;
        AgcDriver::Metal::MetalDriver transactionDriver;
        transactionDriver.Configure((__bridge void*)device, (__bridge void*)library, mapped,
            [&](std::uint32_t queue) {
                Require(queue == 0x20, "CPU mapping transaction EOP arrived from the wrong queue");
                const bool first = transactionInterrupts.load() == 0;
                const auto storage = first ? previousWeak.lock() : nextWeak.lock();
                Require(storage != nullptr, "CPU mapping transaction released active GPU storage before EOP");
                checkTransactionOutput(*storage, first ? 1 : 3);
                transactionInterrupts.fetch_add(1);
                if (first) {
                    std::unique_lock lock(transactionMutex);
                    oldEopEntered = true;
                    transactionChanged.notify_all();
                    transactionChanged.wait(lock, [&] { return oldEopReleased; });
                }
            }, 0);
        transactionDriver.RegisterShader(reinterpret_cast<const Shader*>(0x720000));
        const auto releaseTransaction = [&] {
            {
                std::lock_guard lock(transactionMutex);
                oldEopReleased = true;
                mutationReleased = true;
            }
            transactionChanged.notify_all();
        };
        std::future<void> transaction, submitting, registering, suspending;
        try {
            auto invalid = replacement;
            invalid.push_back({ComputeInputAddress + 4, std::as_writable_bytes(std::span(nextOwner->input).first(4)), false});
            for (const auto& preparation : {std::pair{&invalid, 1ull}, std::pair{&replacement, 0ull}}) {
                bool rejected = false;
                try {
                    transactionDriver.MutateBorrowedRanges(*preparation.first, preparation.second,
                        [&] { mutations.fetch_add(1); }, previousOwner, nextOwner);
                } catch (const std::exception&) { rejected = true; }
                Require(rejected && mutations.load() == 0, "Invalid CPU mapping preparation ran the mutation callback");
            }
            transactionDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
            {
                std::unique_lock lock(transactionMutex);
                Require(transactionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return oldEopEntered; }),
                    "CPU mapping transaction did not reach the old completed EOP");
            }
            std::promise<void> transactionStarted;
            auto started = transactionStarted.get_future();
            transaction = std::async(std::launch::async, [&] {
                transactionStarted.set_value();
                transactionDriver.MutateBorrowedRanges(replacement, 1, [&] {
                    mutations.fetch_add(1);
                    nextOwner->input.fill(0xb5b5b5b5);
                    for (std::uint32_t i = 0; i < 64; ++i) nextOwner->input[i * 4] = i * 5 + 47;
                    nextOwner->code[7] = 0x4a080883;
                    std::unique_lock lock(transactionMutex);
                    mutationEntered = true;
                    transactionChanged.notify_all();
                    transactionChanged.wait(lock, [&] { return mutationReleased; });
                }, previousOwner, nextOwner);
            });
            started.wait();
            const bool retainedOldEop = transaction.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
            {
                std::lock_guard lock(transactionMutex);
                Require(!mutationEntered && mutations.load() == 0, "CPU mutation ran while the old EOP still retained its mapping");
                oldEopReleased = true;
            }
            transactionChanged.notify_all();
            {
                std::unique_lock lock(transactionMutex);
                Require(transactionChanged.wait_for(lock, std::chrono::seconds(10), [&] { return mutationEntered; }),
                    "CPU mapping transaction did not enter its mutation callback after EOP drain");
            }
            Require(retainedOldEop, "CPU mapping transaction returned before old EOP drain");
            std::promise<void> submitStarted, registerStarted, suspendStarted;
            auto beganSubmit = submitStarted.get_future(), beganRegister = registerStarted.get_future(), beganSuspend = suspendStarted.get_future();
            submitting = std::async(std::launch::async, [&] {
                submitStarted.set_value();
                transactionDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
            });
            registering = std::async(std::launch::async, [&] {
                registerStarted.set_value();
                transactionDriver.RegisterShader(reinterpret_cast<const Shader*>(0x700000));
            });
            suspending = std::async(std::launch::async, [&] {
                suspendStarted.set_value();
                transactionDriver.SuspendPoint();
            });
            beganSubmit.wait();
            beganRegister.wait();
            beganSuspend.wait();
            const bool submitBlocked = submitting.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
            const bool registerBlocked = registering.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
            const bool suspendBlocked = suspending.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
            releaseTransaction();
            transaction.get();
            submitting.get();
            registering.get();
            suspending.get();
            transactionDriver.WaitIdle();
            Require(submitBlocked && registerBlocked && suspendBlocked,
                "Public GPU admission returned while the CPU mapping mutation was still active");
            Require(transactionInterrupts.load() == 2 && mutations.load() == 1,
                "CPU mapping transaction did not produce exactly one mutation and two completed EOPs");
            checkTransactionOutput(*previousOwner, 1);
            checkTransactionOutput(*nextOwner, 3);
            auto expectedInput = originalInput;
            expectedInput.fill(0xb5b5b5b5);
            for (std::uint32_t i = 0; i < 64; ++i) expectedInput[i * 4] = i * 5 + 47;
            auto expectedCode = originalComputeStorage;
            expectedCode[7] = 0x4a080883;
            Require(previousOwner->input == originalInput && previousOwner->code == originalComputeStorage &&
                nextOwner->input == expectedInput && nextOwner->code == expectedCode,
                "CPU mapping transaction GPU execution modified read-only input/code or storage guards");
            previousOwner.reset();
            nextOwner.reset();
            Require(previousWeak.expired() && !nextWeak.expired(),
                "Successful CPU mapping transaction retained the retired owner or dropped the published owner");
            transactionDriver.Shutdown();
            Require(nextWeak.expired(), "CPU mapping transaction retained its published host owner after shutdown");
        } catch (...) {
            const auto error = std::current_exception();
            releaseTransaction();
            for (auto* pending : {&transaction, &submitting, &registering, &suspending})
                if (pending->valid()) try { pending->get(); } catch (...) {}
            try { transactionDriver.Shutdown(); } catch (...) {}
            std::rethrow_exception(error);
        }
        std::cout << "Actual CPU mapping transaction: invalid preparation, EOP drain, admission closure, new RDNA storage and owning lease retirement passed\n";
    }
    {
        auto previousOwner = transactionStorage(), nextOwner = transactionStorage();
        std::weak_ptr<TransactionStorage> previousWeak = previousOwner, nextWeak = nextOwner;
        const auto mapped = transactionRanges(previousOwner, 1), replacement = transactionRanges(nextOwner, 2);
        std::atomic<std::uint32_t> failedInterrupts{0};
        std::mutex failedMutex;
        std::condition_variable failedChanged;
        bool failedMutationEntered = false, failedMutationReleased = false;
        const auto marker = std::make_exception_ptr(std::runtime_error("CPU mapping mutation partial failure"));
        AgcDriver::Metal::MetalDriver failedTransaction;
        failedTransaction.Configure((__bridge void*)device, (__bridge void*)library, mapped,
            [&](std::uint32_t) { failedInterrupts.fetch_add(1); });
        const auto releaseFailedMutation = [&] {
            {
                std::lock_guard lock(failedMutex);
                failedMutationReleased = true;
            }
            failedChanged.notify_all();
        };
        std::future<void> mutation, pendingSubmit, pendingMutation;
        try {
            mutation = std::async(std::launch::async, [&] {
                failedTransaction.MutateBorrowedRanges(replacement, 1, [&] {
                    nextOwner->input[0] = 97;
                    std::unique_lock lock(failedMutex);
                    failedMutationEntered = true;
                    failedChanged.notify_all();
                    failedChanged.wait(lock, [&] { return failedMutationReleased; });
                    std::rethrow_exception(marker);
                }, previousOwner, nextOwner);
            });
            {
                std::unique_lock lock(failedMutex);
                Require(failedChanged.wait_for(lock, std::chrono::seconds(10), [&] { return failedMutationEntered; }),
                    "Failing CPU mapping transaction did not enter its mutation callback");
            }
            std::promise<void> pendingStarted;
            auto started = pendingStarted.get_future();
            pendingSubmit = std::async(std::launch::async, [&] {
                pendingStarted.set_value();
                failedTransaction.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
            });
            started.wait();
            const bool admissionBlocked = pendingSubmit.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
            std::promise<void> nextMutationStarted;
            auto nextStarted = nextMutationStarted.get_future();
            pendingMutation = std::async(std::launch::async, [&] {
                nextMutationStarted.set_value();
                failedTransaction.MutateBorrowedRanges(replacement, 2, [&] {
                    nextOwner->input[4] = 123;
                }, previousOwner, nextOwner);
            });
            nextStarted.wait();
            const bool mutationBlocked = pendingMutation.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
            releaseFailedMutation();
            std::exception_ptr mutationError, admissionError, nextMutationError;
            try { mutation.get(); } catch (...) { mutationError = std::current_exception(); }
            try { pendingSubmit.get(); } catch (...) { admissionError = std::current_exception(); }
            try { pendingMutation.get(); } catch (...) { nextMutationError = std::current_exception(); }
            Require(admissionBlocked && mutationBlocked && mutationError == marker && admissionError == marker &&
                nextMutationError == marker && failedInterrupts.load() == 0,
                "CPU mutation failure did not wake blocked admission and mutation with the original terminal failure");
            for (const auto word : previousOwner->output) Require(word == 0xdeadbeef, "Failed transaction changed old output or guards");
            for (const auto word : nextOwner->output) Require(word == 0xdeadbeef, "Failed transaction changed new output or guards");
            auto partiallyMutatedInput = originalInput;
            partiallyMutatedInput[0] = 97;
            Require(previousOwner->input == originalInput && previousOwner->code == originalComputeStorage &&
                nextOwner->input == partiallyMutatedInput && nextOwner->code == originalComputeStorage,
                "Failed CPU transaction changed read-only storage beyond its partial CPU mutation");
            previousOwner.reset();
            nextOwner.reset();
            Require(!previousWeak.expired() && !nextWeak.expired(),
                "Failed CPU mutation released old or new actual host storage before shutdown");
            std::exception_ptr shutdownError;
            try { failedTransaction.Shutdown(); } catch (...) { shutdownError = std::current_exception(); }
            Require(shutdownError == marker && previousWeak.expired() && nextWeak.expired(),
                "Failed CPU mutation did not preserve its failure and release both host owners at shutdown");
        } catch (...) {
            const auto error = std::current_exception();
            releaseFailedMutation();
            for (auto* pending : {&mutation, &pendingSubmit, &pendingMutation})
                if (pending->valid()) try { pending->get(); } catch (...) {}
            try { failedTransaction.Shutdown(); } catch (...) {}
            std::rethrow_exception(error);
        }
        std::cout << "Actual CPU mutation failure: blocked admission wakes, no GPU/EOP writes and both actual host leases survive until shutdown passed\n";
    }
    {
        auto previousOwner = transactionStorage(), nextOwner = transactionStorage();
        std::weak_ptr<TransactionStorage> previousWeak = previousOwner, nextWeak = nextOwner;
        const auto mapped = transactionRanges(previousOwner, 1), replacement = transactionRanges(nextOwner, 2);
        const auto marker = std::make_exception_ptr(std::runtime_error("Same-thread CPU mapping mutation partial failure"));
        AgcDriver::Metal::MetalDriver sameThreadTransaction;
        sameThreadTransaction.Configure((__bridge void*)device, (__bridge void*)library, mapped);
        bool mutationEntered = false, callbackShutdownRejected = false;
        std::exception_ptr mutationError;
        try {
            sameThreadTransaction.MutateBorrowedRanges(replacement, 1, [&] {
                mutationEntered = true;
                nextOwner->input[0] = 97;
                try { sameThreadTransaction.Shutdown(); }
                catch (const std::runtime_error& error) {
                    callbackShutdownRejected = std::string(error.what()).find("mapping transaction callback") != std::string::npos;
                }
                std::rethrow_exception(marker);
            }, previousOwner, nextOwner);
        } catch (...) { mutationError = std::current_exception(); }
        Require(mutationEntered && callbackShutdownRejected && mutationError == marker,
            "Same-thread CPU mutation did not reject active-callback shutdown and preserve its original failure");
        for (const auto word : previousOwner->output) Require(word == 0xdeadbeef, "Same-thread failed mutation changed old output or guards");
        for (const auto word : nextOwner->output) Require(word == 0xdeadbeef, "Same-thread failed mutation changed new output or guards");
        auto partiallyMutatedInput = originalInput;
        partiallyMutatedInput[0] = 97;
        Require(previousOwner->input == originalInput && previousOwner->code == originalComputeStorage &&
            nextOwner->input == partiallyMutatedInput && nextOwner->code == originalComputeStorage,
            "Same-thread failed mutation changed storage beyond its partial CPU write");
        previousOwner.reset();
        nextOwner.reset();
        Require(!previousWeak.expired() && !nextWeak.expired(),
            "Same-thread failed mutation released actual host owners before shutdown");
        std::exception_ptr shutdownError;
        try { sameThreadTransaction.Shutdown(); } catch (...) { shutdownError = std::current_exception(); }
        Require(shutdownError == marker && previousWeak.expired() && nextWeak.expired(),
            "Same-thread shutdown after failed CPU mutation lost the original failure or retained actual host owners");
        std::cout << "Same-thread CPU mutation teardown: active callback rejects shutdown; unwound failure drains both actual host owners and preserves original error passed\n";
    }
    std::mutex callbackMutex;
    std::condition_variable callbackChanged;
    bool callbackEntered = false, callbackReleased = false;
    AgcDriver::Metal::MetalDriver failureDriver;
    failureDriver.WaitIdle();
    failureDriver.ReleaseWindow(nullptr);
    failureDriver.Configure((__bridge void*)device, (__bridge void*)library, ranges,
        [&](std::uint32_t queue) {
            Require(queue == 0x20, "Failure lifetime EOP arrived from the wrong native queue");
            for (std::uint32_t i = 0; i < output.size(); ++i) {
                const auto expected = i % 4 == 0 ? (i / 4) * 3 + 8 : 0xdeadbeefu;
                Require(output[i] == expected, "Failure lifetime EOP observed unfinished actual RDNA compute copyback");
            }
            std::unique_lock lock(callbackMutex);
            callbackEntered = true;
            callbackChanged.notify_all();
            callbackChanged.wait(lock, [&] { return callbackReleased; });
        });
    const auto releaseCallback = [&] {
        {
            std::lock_guard lock(callbackMutex);
            callbackReleased = true;
        }
        callbackChanged.notify_all();
    };
    try {
        std::copy(firstDump.begin(), firstDump.end(), indirectCommands.begin());
        packets[3].dw_num = static_cast<std::uint32_t>(firstDump.size());
        failureDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + 3 * sizeof(::Packet)), 0);
        failureDriver.WaitIdle();
        checkQueries(0, finalSamples, "Replacement native driver preserves process sample total");
        failureDriver.RegisterShader(reinterpret_cast<const Shader*>(0x720000));
        failureDriver.Submit(reinterpret_cast<const ::Packet*>(PacketAddress + sizeof(::Packet)), 0x20);
        bool entered;
        {
            std::unique_lock lock(callbackMutex);
            entered = callbackChanged.wait_for(lock, std::chrono::seconds(10), [&] { return callbackEntered; });
        }
        if (!entered) throw std::runtime_error("Failure lifetime actual RDNA EOP callback did not enter");
        const auto marker = std::make_exception_ptr(std::runtime_error("Native queue failure lifetime marker"));
        failureDriver.ReportFailure(marker);
        std::promise<void> waiterStarted;
        auto started = waiterStarted.get_future();
        auto waiter = std::async(std::launch::async, [&] {
            waiterStarted.set_value();
            try { failureDriver.WaitIdle(); }
            catch (...) { return std::current_exception(); }
            return std::exception_ptr{};
        });
        started.wait();
        const bool remainedPending = waiter.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout;
        releaseCallback();
        const auto reported = waiter.get();
        std::exception_ptr shutdownFailure;
        try { failureDriver.Shutdown(); }
        catch (...) { shutdownFailure = std::current_exception(); }
        Require(remainedPending, "Native WaitIdle returned failure while the EOP worker still held borrowed memory");
        Require(reported == marker, "Native WaitIdle did not preserve the original queue failure after draining its worker");
        Require(shutdownFailure == marker, "Native shutdown did not preserve the drained queue failure");
    } catch (...) {
        releaseCallback();
        try { failureDriver.Shutdown(); }
        catch (...) {}
        throw;
    }
    Require(vertices == originalVertices && input == originalInput && computeStorage == originalComputeStorage,
        "Failed queue draining modified borrowed input or raw shader storage guards");
    std::cout << "Actual native queue failure: WaitIdle retained borrowed memory until the completed EOP worker released it and preserved the original error passed\n";
}

}

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        try {
            Require(argc == 2, "Guest draw replay requires the compiled utility metallib path");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Guest draw replay requires a Metal 3 device");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
            Require(library != nil, std::string("Guest draw utility library load failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
            std::cout << "Actual RDNA native draw replay on " << device.name.UTF8String << '\n';
            DrawGuest(device, library, false, false);
            DrawGuest(device, library, true, false);
            DrawGuest(device, library, false, true);
            DrawGuest(device, library, false, true, true);
            DrawSubmittedGuest(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
