#import <Foundation/Foundation.h>
#include "MetalShaderPipeline.hpp"
#include "MetalGuestMemory.hpp"
#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace ShaderRecompiler;
using namespace AgcDriver::Metal;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::array<std::uint32_t, 17> Vop1ControlCode{
    0x34020082, 0xe0302000, 0x80000401, 0xbf8c3f70, 0x7e000000, 0x7e003600, 0x7e008200, 0x4a080881,
    0xd5800000, 0x00000000, 0xd59b0000, 0x00000000, 0xd5c10000, 0x00000000, 0xe0702000, 0x80010401, 0xbf810000,
};
constexpr std::array<std::uint32_t, 22> VccBaseCode{
    0xbeea0400, 0x34020082, 0x34040084, 0x340c0085, 0x4a0c0cff, 0x00001000, 0x4ad404ff, 0x00000048,
    0xdc308010, 0x046a0001, 0xdcc98700, 0x0c6a0201, 0xdc3887b8, 0x006a006a, 0xbf8c3f70, 0xdc788000,
    0x006a0006, 0xdc708010, 0x006a0406, 0xdc708014, 0x006a0c06, 0xbf810000,
};
constexpr std::array<std::uint32_t, 22> WaveUniformCode{
    0xf4240200, 0xfa000000, 0xbf8cc07f, 0xbe8a0380, 0xbe8b0380, 0x800a080a, 0x930c0b0a, 0x800a0c0a,
    0x800b810b, 0xbf0a090b, 0xbf85fffa, 0x7d880094, 0xbe8d106a, 0x4a060087, 0x7e1c0503, 0xd5430001,
    0x00281b00, 0x4a02020e, 0x34040082, 0xe0701000, 0x80010102, 0xbf810000,
};

SpirvTarget Target(id<MTLDevice> device) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto size = device.maxThreadsPerThreadgroup;
    return {0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
            {static_cast<std::uint32_t>(size.width), static_cast<std::uint32_t>(size.height), static_cast<std::uint32_t>(size.depth)},
            static_cast<std::uint32_t>(size.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
}

MetalBackend::Result Compile(id<MTLDevice> device, std::span<const std::uint32_t> code,
                             std::span<const std::uint32_t> userData, std::uint32_t pushOffset,
                             std::uint32_t waveSize = 32, std::uint32_t hostSubgroupSize = 32) {
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<MemoryRegion, 1> regions{{{address, std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{64, 1, 1}, 0, {false, false, false}, false, 1};
    auto target = Target(device);
    target.subgroupSize = hostSubgroupSize;
    RecompileRequest request{{ShaderStage::Compute, address, code, 0, {}},
        {waveSize, 0, userData, compute, {}, {}, regions}, target, {0, 0, pushOffset, 128u - pushOffset}};
    request.useCache = false;
    const auto guest = Recompile(request);
    Require(!guest.spirv.empty(), "Existing guest frontend emitted no SPIR-V");
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true;
    options.supportsGpuAddresses = true;
    options.supportsSimdGroups = true;
    options.pushConstantOffsetBytes = pushOffset;
    return MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
}

void Wait(id<MTLCommandBuffer> commands) {
    [commands commit];
    [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted,
            std::string("Guest Metal dispatch failed: ") + (commands.error.localizedDescription.UTF8String ?: "unknown error"));
}

void DescriptorReplay(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    constexpr std::uint32_t sentinel = 0xdeadbeef;
    std::array<std::uint32_t, 64 * 4> input{}, output;
    output.fill(sentinel);
    for (std::uint32_t lane = 0; lane < 64; ++lane) input[lane * 4] = lane * 0x01010101u + 7u;
    const auto descriptor = [](std::uint64_t address) {
        return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(address),
            static_cast<std::uint32_t>(address >> 32u) | (4u << 16u), 256u, 0x01016facu};
    };
    const auto in = descriptor(0x100000), out = descriptor(0x200000);
    std::array<std::uint32_t, 8> userData;
    std::copy(in.begin(), in.end(), userData.begin());
    std::copy(out.begin(), out.end(), userData.begin() + 4);
    const auto shader = Compile(device, Vop1ControlCode, userData, 16);
    MetalComputePipeline pipeline(device, shader);
    auto inBuffer = [device newBufferWithBytes:input.data() length:sizeof(input) options:MTLResourceStorageModeShared];
    auto outBuffer = [device newBufferWithBytes:output.data() length:sizeof(output) options:MTLResourceStorageModeShared];
    Require(inBuffer != nil && outBuffer != nil, "Descriptor replay buffer allocation failed");
    std::vector<MetalShaderResourceBinding> bindings;
    for (const auto& binding : shader.guest.bindings) {
        if (binding.role != DescriptorRole::GuestBuffers) throw std::runtime_error("Unexpected VOP1 descriptor role");
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        Require(binding.guestDescriptor.size() == binding.count * 4u, "Guest buffer descriptor metadata is incomplete");
        for (std::uint32_t i = 0; i < binding.count; ++i) {
            const auto low = binding.guestDescriptor[i * 4];
            Require(low == 0x100000 || low == 0x200000, "Unexpected guest descriptor address");
            auto buffer = low == 0x100000 ? inBuffer : outBuffer;
            native.buffers.push_back({buffer, 0, buffer.length});
        }
        bindings.push_back(std::move(native));
    }
    auto commands = [queue commandBuffer];
    pipeline.Encode(commands, bindings, MTLSizeMake(64, 1, 1));
    Wait(commands);
    std::memcpy(output.data(), outBuffer.contents, sizeof(output));
    for (std::uint32_t lane = 0; lane < 64; ++lane) {
        Require(output[lane * 4] == input[lane * 4] + 1u, "RDNA VOP1 replay produced wrong value at lane " + std::to_string(lane));
        for (std::uint32_t word = 1; word < 4; ++word)
            Require(output[lane * 4 + word] == sentinel, "RDNA VOP1 replay wrote outside the guest store");
    }
    Require(std::memcmp(inBuffer.contents, input.data(), sizeof(input)) == 0, "RDNA VOP1 replay modified read-only input");
    std::cout << "Existing RDNA VOP1 fixture: 64 lanes, descriptor load/ALU/store, push offset 16 passed\n";
}

void BdaReplay(id<MTLDevice> device, id<MTLCommandQueue> queue, bool writable) {
    constexpr std::uint64_t guestAddress = 0x400000;
    std::vector<std::uint32_t> memory(16384, 0xcdcdcdcdu);
    for (std::uint32_t word = 0; word < 768; ++word) memory[word] = 0x51000000u + word * 0x00010203u;
    const auto initial = memory;
    auto expected = initial;
    for (std::uint32_t lane = 0; lane < 64; ++lane) {
        for (std::uint32_t component = 0; component < 4; ++component)
            expected[1024 + lane * 8 + component] = initial[512 + lane * 4 + component];
        expected[1024 + lane * 8 + 4] = initial[4 + lane];
        expected[1024 + lane * 8 + 5] = initial[448 + lane];
        expected[448 + lane] = initial[448 + lane] + lane * 16u;
    }
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(guestAddress), 0};
    auto shader = Compile(device, VccBaseCode, userData, 0);
    Require(shader.requiresGpuAddresses && shader.guest.bdaAbiVersion == BdaAbi::Version,
            "Global fixture did not emit the existing BDA path");
    MetalComputePipeline pipeline(device, shader);
    MetalGuestMemory guest(device);
    guest.RegisterBorrowedHostSpanUntilSnapshotsComplete(guestAddress, std::as_writable_bytes(std::span(memory)), writable);
    auto snapshot = guest.CaptureAfterPriorSnapshotsComplete();
    std::vector<MetalShaderResourceBinding> bindings;
    for (const auto& binding : shader.guest.bindings) {
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        Require(binding.count == 1, "Unexpected BDA descriptor array");
        id<MTLBuffer> buffer = nil;
        if (binding.role == DescriptorRole::BdaPagetable) buffer = snapshot.Table();
        else if (binding.role == DescriptorRole::FaultBuffer) buffer = snapshot.FaultBuffer();
        else throw std::runtime_error("Unexpected global BDA descriptor role");
        native.buffers.push_back({buffer, 0, buffer.length});
        bindings.push_back(std::move(native));
    }
    std::vector<id<MTLResource>> resident;
    for (id<MTLBuffer> buffer in snapshot.Buffers()) resident.push_back(buffer);
    auto commands = [queue commandBuffer];
    pipeline.Encode(commands, bindings, MTLSizeMake(64, 1, 1), {}, resident);
    Wait(commands);
    const auto fault = snapshot.CompleteAndCopyDirtyPagesToBorrowedHost(commands);
    if (writable) {
        Require(fault.state == BdaAbi::FaultState::Empty, "RDNA global BDA replay published a fault");
        Require(memory == expected, "RDNA global BDA replay changed memory differently from expected loads/stores/atomics");
    } else {
        Require(fault.state == BdaAbi::FaultState::Ready && fault.reason == BdaAbi::FaultReason::Permission,
                "RDNA global BDA replay failed to publish the permission fault");
        Require(fault.address >= guestAddress && fault.address < guestAddress + memory.size() * 4u,
                "RDNA BDA fault address is outside the guest allocation");
        Require(fault.bytes == 4 && fault.stage == static_cast<std::uint32_t>(IrShaderStage::Compute), "RDNA BDA fault access size or stage is incorrect: bytes=" +
                std::to_string(fault.bytes) + " stage=" + std::to_string(fault.stage));
        Require(memory == initial, "RDNA global BDA permission fault changed borrowed read-only memory");
    }
    std::cout << "Existing RDNA global fixture: " << (writable ? "64 lanes, BDA load/store/atomic and guarded copy-back" : "read-only BDA permission fault") << " passed\n";
}

void RejectSubgroupMismatch(id<MTLDevice> device) {
    const std::array<std::uint32_t, 8> userData{
        0x100000, 0, 16, 0x01016fac, 0x200000, 0, 256, 0x01016fac};
    const auto shader = Compile(device, WaveUniformCode, userData, 0, 64, 64);
    Require(shader.requiresSimdGroups && shader.guest.hostSubgroupSize == 64,
            "Genuine SIMD fixture did not retain its host subgroup compilation contract");
    bool rejected = false;
    try {
        MetalComputePipeline pipeline(device, shader);
    } catch (const std::invalid_argument& error) {
        rejected = std::string(error.what()).find("subgroup width differs") != std::string::npos;
        if (!rejected) throw;
    }
    Require(rejected, "Native Metal accepted an actual guest SIMD shader compiled for mismatched host subgroup width 64");
    std::cout << "Existing RDNA SIMD fixture: incompatible host subgroup width rejected before dispatch\n";
}
}

int main() {
    @autoreleasepool {
        try {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            Require(device != nil && [device supportsFamily:MTLGPUFamilyMetal3], "Guest shader replay requires a Metal 3 device");
            id<MTLCommandQueue> queue = [device newCommandQueue];
            Require(queue != nil, "Guest shader replay could not create the native queue");
            std::cout << "Native Metal guest replay on " << device.name.UTF8String << '\n';
            DescriptorReplay(device, queue);
            BdaReplay(device, queue, true);
            BdaReplay(device, queue, false);
            RejectSubgroupMismatch(device);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
