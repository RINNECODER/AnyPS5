#import <Foundation/Foundation.h>
#include "MetalComputeDispatch.hpp"
#include "MetalDraw.hpp"
#include "Recompiler.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/DrawDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
constexpr std::uint32_t Width = 64, Height = 32, Sentinel = 0x7b7b7b7b;
constexpr std::uint64_t VertexAddress = 0x100000, ColorAllocation = 0x200000,
    ColorAddress = ColorAllocation + 256, OutputAllocation = 0x300000,
    OutputAddress = OutputAllocation + 256, VertexCodeAddress = 0x500000,
    FragmentCodeAddress = 0x600000, ComputeCodeAddress = 0x700000;
constexpr std::array<std::uint32_t, 7> VertexCode{
    0xe0382000, 0x80020005, 0xbf8c3f70, 0x0600000c,
    0xf80008cf, 0x03020100, 0xbf810000};
constexpr std::array<std::uint32_t, 7> FragmentCode{
    0x7e000200, 0x7e020201, 0x7e040202, 0x7e060203,
    0xf800180f, 0x03020100, 0xbf810000};
constexpr std::array<std::uint32_t, 8> ComputeCode{
    0x34000084, 0x7e020204, 0x7e040205, 0x7e060206, 0x7e080207,
    0xe0781000, 0x80000100, 0xbf810000};
constexpr std::array<std::uint32_t, 5> Dispatch{0xc0031500, 1, 1, 1, 0x8041};
constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1, -1, 0.5f, 1}, {3, -1, 0.5f, 1}, {-1, 3, 0.5f, 1}}};

void Require(bool condition, const std::string& reason) {
    if (!condition) throw std::runtime_error(reason);
}

template<class F> void Reject(F&& operation, const std::string& diagnostic, const std::string& name) {
    bool rejected = false;
    try { operation(); }
    catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find(diagnostic) != std::string::npos,
            name + " rejected for the wrong reason: " + error.what());
        rejected = true;
    }
    Require(rejected, name + " silently accepted required missing state");
}

AgcDriver::QueueState ComputeQueue(bool complete) {
    AgcDriver::QueueState queue;
    queue.shader = {{0x207, 32}, {0x208, 1}, {0x209, 1},
        {0x20c, ComputeCodeAddress >> 8u}, {0x20d, 0}, {0x213, 16},
        {0x240, OutputAddress}, {0x241, 16u << 16u}, {0x242, 32}, {0x243, 0x11016fac},
        {0x245, 0x12345678}, {0x246, 0}};
    if (complete) { queue.shader[0x244] = 0; queue.shader[0x247] = 0; }
    return queue;
}

SpirvTarget Target(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto maximum = device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height),
         static_cast<std::uint32_t>(maximum.depth)}, static_cast<std::uint32_t>(maximum.width),
        static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
}

AgcDriver::QueueState DrawQueue() {
    AgcDriver::QueueState queue;
    queue.userConfig[0x242] = 4;
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
        {0x318, ColorAddress >> 8u}, {0x390, 0},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-16.0f)}, {0x112, std::bit_cast<std::uint32_t>(16.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0}, {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x1b3, 0}, {0x1b4, 0}};
    for (const auto& [offset, value] : context) queue.context.insert_or_assign(offset, value);
    queue.shader = {{0xc8, VertexCodeAddress >> 8u}, {0xc9, 0}, {0x8b, 10},
        {0x8c, VertexAddress}, {0x8d, 16u << 16u}, {0x8e, 3}, {0x8f, 0x01016fac},
        {0x90, 0}, {0x008, FragmentCodeAddress >> 8u}, {0x009, 0}, {0x00b, 8},
        {0x00c, 0}, {0x00d, std::bit_cast<std::uint32_t>(1.0f)}, {0x00e, 0}, {0x00f, 0}};
    return queue;
}

AgcDriver::DriverDetail::ShaderRegistry Registry() {
    AgcDriver::DriverDetail::ShaderRegistry registry;
    const auto append = [&](std::uint64_t address, std::uint8_t type, auto code) {
        auto snapshot = std::make_shared<AgcDriver::DriverDetail::ShaderSnapshot>();
        snapshot->codeAddress = address;
        snapshot->headerAddress = 0;
        snapshot->type = type;
        snapshot->code.assign(code.begin(), code.end());
        registry.emplace(address, std::move(snapshot));
    };
    append(VertexCodeAddress, 2, VertexCode);
    append(FragmentCodeAddress, 1, FragmentCode);
    return registry;
}

void ComputeReplay(id<MTLDevice> device) {
    auto queue = ComputeQueue(false);
    const auto state = AgcDriver::DecodeComputeDispatch(queue, Dispatch);
    const std::array<std::uint32_t, 4> values{0, 0x12345678, 0, 0};
    Require(state.userData.size() == 8 && std::equal(values.begin(), values.end(), state.userData.begin() + 4),
        "compute decoder failed optional zero/written preservation contract");
    std::vector<std::uint32_t> output(64 + 32 * 4 + 64, Sentinel);
    auto expected = output;
    for (std::uint32_t lane = 0; lane < 32; ++lane)
        std::copy(values.begin(), values.end(), expected.begin() + 64 + lane * 4);
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 1> ranges{{
        {OutputAllocation, std::as_writable_bytes(std::span(output)), true}}};
    AgcDriver::Metal::MetalComputeDispatch adapter(device);
    const auto fault = adapter.DispatchSynchronously(state, ComputeCode, ranges);
    Require(fault.state == BdaAbi::FaultState::Empty, "optional SGPR compute published a GPU fault");
    Require(output == expected, "native optional SGPR compute readback differs from independent raw-word expectation");
    queue.shader[0x244] = 0x2468ace0;
    queue.shader[0x247] = 0xfedcba98;
    const auto rebound = AgcDriver::DecodeComputeDispatch(queue, Dispatch);
    output.assign(output.size(), Sentinel);
    const std::array<std::uint32_t, 4> written{0x2468ace0, 0x12345678, 0, 0xfedcba98};
    for (std::uint32_t lane = 0; lane < 32; ++lane)
        std::copy(written.begin(), written.end(), expected.begin() + 64 + lane * 4);
    const auto second = adapter.DispatchSynchronously(rebound, ComputeCode, ranges);
    Require(second.state == BdaAbi::FaultState::Empty && output == expected,
        "native compute failed formerly unwritten SGPR rebound or guarded buffer preservation");
    std::cout << "compute: 32 native Metal lanes, absent/written/rebound SGPR words and 128 guard words passed\n";
}

void DrawReplay(id<MTLDevice> device, id<MTLLibrary> library, bool vertexOptional) {
    auto queue = DrawQueue();
    if (vertexOptional) queue.shader.erase(0x90);
    else { queue.shader.erase(0x00c); queue.shader.erase(0x00f); }
    const auto registry = Registry();
    auto vertices = Triangle;
    const auto originalVertices = vertices;
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 2> ranges{{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false}, {ColorAllocation, pixels, true}}};
    AgcDriver::NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    AgcDriver::Metal::MetalDraw adapter(device, library);
    for (std::uint32_t pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            if (vertexOptional) queue.shader[0x90] = std::bit_cast<std::uint32_t>(4.0f);
            else { queue.shader[0x00c] = std::bit_cast<std::uint32_t>(1.0f); queue.shader[0x00f] = std::bit_cast<std::uint32_t>(1.0f); }
        }
        const auto decoded = AgcDriver::DecodeDrawDispatch(queue, registry);
        Require(decoded.programs.size() == 2, "optional SGPR draw decoded wrong shader-stage count");
        const auto& vertex = decoded.programs[0];
        const auto& fragment = decoded.programs[1];
        Require(vertex.userData.size() == 5 && vertex.userData[4] ==
            (vertexOptional && pass ? std::bit_cast<std::uint32_t>(4.0f) : 0u),
            "vertex decoder failed optional zero/written preservation contract");
        const auto fragmentWritten = !vertexOptional && pass == 1;
        const std::array<std::uint32_t, 4> expectedUsers{
            fragmentWritten ? std::bit_cast<std::uint32_t>(1.0f) : 0u,
            std::bit_cast<std::uint32_t>(1.0f), 0,
            fragmentWritten ? std::bit_cast<std::uint32_t>(1.0f) : 0u};
        Require(fragment.userData == std::vector<std::uint32_t>(expectedUsers.begin(), expectedUsers.end()),
            "fragment decoder failed optional zero/written preservation contract");
        AgcDriver::Pm4::DrawParameters draw{};
        draw.indexCount = 3;
        draw.instanceCount = 1;
        draw.indexed = false;
        std::vector<RecompileResult> compiled;
        std::vector<AgcDriver::Graphics::CompiledShader> shaders;
        compiled.reserve(decoded.programs.size());
        shaders.reserve(decoded.programs.size());
        std::uint32_t pushOffset = 0;
        for (const auto& program : decoded.programs) {
            const std::optional<ShaderVertexStageInfo> vertexInfo = program.binary.stage == ShaderStage::Vertex ?
                std::optional(ShaderVertexStageInfo{}) : std::nullopt;
            auto request = AgcDriver::BuildDrawRecompileRequest(program.binary, program.firstUserSgpr, program.userData,
                decoded.state, decoded.pixel, vertexInfo, Target(device), pushOffset, draw,
                std::span(program.memory).first(1), {});
            request.useCache = false;
            compiled.push_back(Recompile(request));
            shaders.push_back({program.binary.stage, &compiled.back(), pushOffset});
            pushOffset += static_cast<std::uint32_t>(compiled.back().pushConstants.size());
        }
        std::fill(pixels.begin(), pixels.end(), std::byte{0x7b});
        std::fill_n(pixels.begin() + 256, Width * Height * 4, std::byte{0x40});
        const auto fault = adapter.DrawSynchronously(decoded.state, draw, shaders, ranges);
        Require(fault.state == BdaAbi::FaultState::Empty, "optional SGPR native draw published a GPU fault");
        for (std::size_t offset = 0; offset < pixels.size(); ++offset) {
            std::byte expected{0x7b};
            if (offset >= 256 && offset < 256 + Width * Height * 4) {
                const auto channel = (offset - 256) % 4;
                expected = vertexOptional && pass ? std::byte{0x40} :
                    std::byte{static_cast<unsigned char>(channel == 1 || (fragmentWritten && channel != 2) ? 255 : 0)};
            }
            Require(pixels[offset] == expected, "native optional SGPR draw pixel/guard mismatch at byte " + std::to_string(offset));
        }
        Require(vertices == originalVertices, "optional SGPR draw modified borrowed vertex input");
    }
    std::cout << (vertexOptional ? "vertex" : "fragment")
        << ": native decoder-to-Metal missing-zero and written-value rendered pixels plus 512 guard bytes passed\n";
}

void RequiredRegisters() {
    const auto compute = ComputeQueue(true);
    for (const auto offset : {0x20cu, 0x20du, 0x213u, 0x207u, 0x208u, 0x209u}) {
        auto missing = compute;
        missing.shader.erase(offset);
        const auto diagnostic = offset >= 0x20c ? "required shader register" : "missing register at DWORD";
        Reject([&] { static_cast<void>(AgcDriver::DecodeComputeDispatch(missing, Dispatch)); }, diagnostic,
            "compute required register " + std::to_string(offset));
    }
    const auto draw = DrawQueue();
    const auto registry = Registry();
    auto vertices = Triangle;
    std::vector<std::byte> pixels(256 + Width * Height * 4 + 256, std::byte{0x7b});
    const std::array<AgcDriver::NativeGuestMemory::BorrowedRange, 2> ranges{{
        {VertexAddress, std::as_writable_bytes(std::span(vertices)), false}, {ColorAllocation, pixels, true}}};
    AgcDriver::NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    // Unwritten graphics program address, resource and user-data registers read as zero (the shared
    // driver's draw decoding): a missing low address word leaves no registered program to run,
    // while the high address and resource words decode as their zero values.
    for (const auto offset : {0xc8u, 0x008u}) {
        auto missing = draw;
        missing.shader.erase(offset);
        Reject([&] { static_cast<void>(AgcDriver::DecodeDrawDispatch(missing, registry)); },
            "does not belong to a registered shader", "draw unwritten program address " + std::to_string(offset));
    }
    for (const auto offset : {0xc9u, 0x8bu, 0x009u, 0x00bu}) {
        auto missing = draw;
        missing.shader.erase(offset);
        static_cast<void>(AgcDriver::DecodeDrawDispatch(missing, registry));
    }
    auto missing = draw;
    missing.context.erase(0x206);
    Reject([&] { static_cast<void>(AgcDriver::DecodeDrawDispatch(missing, registry)); },
        "missing register in context bank at DWORD 0x206", "draw required viewport register");
    std::cout << "required: six compute shader/dimension controls, two unwritten graphics program addresses, four zero-read graphics registers and the viewport control passed\n";
}
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            Require(argc >= 2, "optional SGPR replay requires compute, draw-vertex, draw-fragment, or required mode");
            const std::string mode = argv[1];
            if (mode == "required") { RequiredRegisters(); return 0; }
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "optional SGPR replay requires Metal 3");
            if (mode == "compute") { ComputeReplay(device); return 0; }
            Require((mode == "draw-vertex" || mode == "draw-fragment") && argc == 3,
                "draw replay requires its utility Metal library path");
            NSError* error = nil;
            const auto url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]];
            id<MTLLibrary> library = [device newLibraryWithURL:url error:&error];
            Require(library != nil, error ? error.localizedDescription.UTF8String : "optional SGPR utility library load failed");
            DrawReplay(device, library, mode == "draw-vertex");
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
