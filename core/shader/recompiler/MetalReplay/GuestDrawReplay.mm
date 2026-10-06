#import <Foundation/Foundation.h>
#include "MetalDraw.hpp"
#include "Recompiler.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include <spirv/unified1/spirv.hpp>
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

void RegisterPacket(AgcDriver::QueueState& queue, std::uint32_t opcode, std::uint32_t offset,
                    std::span<const std::uint32_t> words) {
    std::vector<std::uint32_t> packet{0xc0000000u | (static_cast<std::uint32_t>(words.size()) << 16u) | (opcode << 8u), offset};
    packet.insert(packet.end(), words.begin(), words.end());
    AgcDriver::Pm4::Validate(packet, 0);
    AgcDriver::Pm4::Execute(packet, queue);
}

AgcDriver::QueueState Queue() {
    AgcDriver::QueueState queue;
    const std::array<std::uint32_t, 1> primitive{4};
    RegisterPacket(queue, 0x79, 0x242, primitive);
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
    for (const auto& [offset, value] : context) RegisterPacket(queue, 0x69, offset, std::span(&value, 1));
    const std::array<std::uint32_t, 2> vertexProgram{0x500000u >> 8u, 0}, fragmentProgram{0x600000u >> 8u, 0};
    const std::array<std::uint32_t, 1> vertexResources{8}, fragmentResources{0};
    const std::array<std::uint32_t, 4> userData{static_cast<std::uint32_t>(VertexAddress), 16u << 16u, 3, 0x01016fac};
    RegisterPacket(queue, 0x76, 0xc8, vertexProgram);
    RegisterPacket(queue, 0x76, 0x008, fragmentProgram);
    RegisterPacket(queue, 0x76, 0x08b, vertexResources);
    RegisterPacket(queue, 0x76, 0x00b, fragmentResources);
    RegisterPacket(queue, 0x76, 0x08c, userData);
    return queue;
}

void DrawDecodedGuest(id<MTLDevice> device, id<MTLLibrary> library) {
    auto vertices = Triangle;
    const auto originalVertices = vertices;
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 2> ranges{{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false}, {ColorAllocation, pixels, true}}};
    auto vertex = std::make_shared<AgcDriver::DriverDetail::ShaderSnapshot>();
    vertex->codeAddress = 0x500000;
    vertex->headerAddress = 0x700000;
    vertex->type = 2;
    vertex->code.assign(VertexCode.begin(), VertexCode.end());
    vertex->code[1] = 0x80020005;
    vertex->header.resize(sizeof(Shader) + sizeof(ShaderUserData));
    Shader header{};
    header.user_data = reinterpret_cast<ShaderUserData*>(vertex->headerAddress + sizeof(Shader));
    header.code = reinterpret_cast<void*>(vertex->codeAddress);
    header.header_size = static_cast<std::uint32_t>(vertex->header.size());
    header.shader_size = static_cast<std::uint32_t>(vertex->code.size() * sizeof(std::uint32_t));
    header.type = 2;
    std::memcpy(vertex->header.data(), &header, sizeof(header));
    auto fragment = std::make_shared<AgcDriver::DriverDetail::ShaderSnapshot>();
    fragment->codeAddress = 0x600000;
    fragment->headerAddress = 0x710000;
    fragment->type = 1;
    fragment->code.assign(MaskedPixelCode.begin(), MaskedPixelCode.end());
    fragment->header.resize(sizeof(Shader) + sizeof(ShaderUserData));
    header.user_data = reinterpret_cast<ShaderUserData*>(fragment->headerAddress + sizeof(Shader));
    header.code = reinterpret_cast<void*>(fragment->codeAddress);
    header.header_size = static_cast<std::uint32_t>(fragment->header.size());
    header.shader_size = static_cast<std::uint32_t>(fragment->code.size() * sizeof(std::uint32_t));
    header.type = 1;
    std::memcpy(fragment->header.data(), &header, sizeof(header));
    const AgcDriver::DriverDetail::ShaderRegistry registry{{vertex->codeAddress, vertex}, {fragment->codeAddress, fragment}};
    const AgcDriver::NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    auto queue = Queue();
    const std::array<std::uint32_t, 3> drawPacket{0xc0012d00, 3, 2};
    AgcDriver::Pm4::Validate(drawPacket, 0);
    auto draw = AgcDriver::Pm4::ResolveDraw(drawPacket, queue);
    const auto decoded = AgcDriver::DecodeDrawDispatch(queue, registry);
    const auto vertexInfo = AgcDriver::Graphics::DecodeVertexStageInfo(
        decoded.programs[0].binary.header, decoded.programs[0].binary.headerAddress, decoded.programs[0].userData);
    std::array<RecompileResult, 2> programs;
    std::array<AgcDriver::Graphics::CompiledShader, 2> shaders;
    std::vector<LinkedProgram> linked;
    for (std::size_t i = 0; i < decoded.programs.size(); ++i) {
        const auto& program = decoded.programs[i];
        linked.push_back({decoded.roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
    }
    std::uint32_t pushOffset = 0;
    for (std::uint32_t i = 0; i < 2; ++i) {
        const auto& program = decoded.programs[i];
        auto request = AgcDriver::BuildDrawRecompileRequest(program.binary, program.firstUserSgpr,
            program.userData, decoded.state, decoded.pixel, i == 0 ? std::optional(vertexInfo) : std::nullopt,
            Target(device), pushOffset, draw, program.memory, linked);
        request.useCache = false;
        programs[i] = Recompile(request);
        shaders[i] = {program.binary.stage, &programs[i], pushOffset};
        pushOffset += static_cast<std::uint32_t>(programs[i].pushConstants.size());
    }
    AgcDriver::FoldDrawOffsets(programs[0], decoded.programs[0].firstUserSgpr, decoded.programs[0].userData, draw);
    AgcDriver::Metal::MetalDraw adapter(device, library);
    const auto fault = adapter.DrawSynchronously(decoded.state, draw, shaders, ranges);
    Require(fault.state == BdaAbi::FaultState::Empty, "Shared decoded actual guest draw published a GPU fault");
    for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
        auto expected = std::byte{0x7b};
        if (offset >= 256 && offset < 256 + Width * Height * 4) {
            const auto x = ((offset - 256) / 4) % Width;
            expected = x % 2 == 0 ? std::byte{255} : std::byte{0x40};
        }
        Require(pixels[offset] == expected, "Shared PM4 draw decode and production SGPR ABI produced wrong target byte " + std::to_string(offset));
    }
    Require(vertices == originalVertices, "Shared decoded actual guest draw modified the vertex input");
    std::cout << "Actual PM4 draw: shared state/program/request decode, production SGPR8 descriptor fetch and masked fragment exports passed\n";
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
            DrawDecodedGuest(device, library);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
