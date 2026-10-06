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
    auto fragmentCode = MaskedPixelCode;
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
    auto fragmentHeader = makeHeader(0x710000, 0x600000, sizeof(fragmentCode), 1);
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
    std::uint64_t finalSamples = 384;
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
    fragmentCode = MaskedPixelCode;
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
    AgcDriverShutdown_nid_postfix();
    std::cout << "Actual public AGC Submit: shader registration snapshots, immutable flattened IB, cross-queue compute WAIT/conditional draw, completed EOP and persistent registers passed\n";
    output.fill(0xdeadbeef);
    compute[compute.size() - 6] |= 1u << 24u;
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
