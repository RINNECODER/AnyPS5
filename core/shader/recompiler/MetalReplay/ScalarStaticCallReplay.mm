// Static guest programs adapted from boykopovar/AnyPS5 aa277b3818d8cf7bc900bdfee740f318469df349,
// ScalarStaticCall.cpp (#1535, Sanjaysrinivas); static SWAPPC model #1217 (ebosido444-ai).
#import <Foundation/Foundation.h>
#include "MetalShaderPipeline.hpp"
#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
using namespace AgcDriver::Metal;
constexpr std::uint32_t Guard = 16, Capacity = 64, Components = 4;
constexpr std::array<std::uint32_t, 21> SwappcForward{0x34020084u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xbe8a1f00u, 0x800aff0au, 0x00000014u, 0x820b800bu, 0xbe88210au, 0xbf820008u, 0x4a0808ffu, 0x13579bdfu, 0x3a0a0affu, 0xa5a5a5a5u, 0x4a0c0f06u, 0x3a0e0effu, 0x5a5a5a5au, 0xbe802008u, 0xe0781000u, 0x80010401u, 0xbf810000u};
constexpr std::array<std::uint32_t, 17> CallForward{0x34020084u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xbb080001u, 0xbf820008u, 0x4a0808ffu, 0x13579bdfu, 0x3a0a0affu, 0xa5a5a5a5u, 0x4a0c0f06u, 0x3a0e0effu, 0x5a5a5a5au, 0xbe802008u, 0xe0781000u, 0x80010401u, 0xbf810000u};
constexpr std::array<std::uint32_t, 18> CallBackward{0x34020084u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xbf820008u, 0x4a0808ffu, 0x13579bdfu, 0x3a0a0affu, 0xa5a5a5a5u, 0x4a0c0f06u, 0x3a0e0effu, 0x5a5a5a5au, 0xbe802008u, 0xbb08fff7u, 0xbf820000u, 0xe0781000u, 0x80010401u, 0xbf810000u};
constexpr std::array<std::uint32_t, 17> CallAfterEnd{0x34020084u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xbb080003u, 0xe0781000u, 0x80010401u, 0xbf810000u, 0x4a0808ffu, 0x13579bdfu, 0x3a0a0affu, 0xa5a5a5a5u, 0x4a0c0f06u, 0x3a0e0effu, 0x5a5a5a5au, 0xbe802008u, 0xbf810000u};
constexpr std::array<std::uint32_t, 24> MixedNested{0x34020084u, 0xe0381000u, 0x80000401u, 0xbf8c3f70u, 0xbe8a1f00u, 0x800aff0au, 0x00000014u, 0x820b800bu, 0xbe88210au, 0xbf82000bu, 0x4a0808ffu, 0x13579bdfu, 0xbb0c0001u, 0xbf820006u, 0x3a0a0affu, 0xa5a5a5a5u, 0x4a0c0f06u, 0x3a0e0effu, 0x5a5a5a5au, 0xbe80200cu, 0xbe802008u, 0xe0781000u, 0x80010401u, 0xbf810000u};
void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::uint32_t Input(std::uint32_t lane, std::uint32_t component) {
    constexpr std::array<std::uint32_t, 8> selectors{0, 1, 2, 3, 4, 7, 0x80000000, 0xffffffff};
    if (component == 0) return selectors[lane % selectors.size()];
    if (component == 1) return 0x9e3779b9u * (lane + 1);
    if (component == 2) return 0xffffffffu - lane;
    return 17u * lane + 1;
}
std::uint32_t Expected(std::uint32_t lane, std::uint32_t component) {
    switch (component) {
        case 0: return Input(lane, 0) + 0x13579bdfu;
        case 1: return Input(lane, 1) ^ 0xa5a5a5a5u;
        case 2: return Input(lane, 2) + Input(lane, 3);
        default: return Input(lane, 3) ^ 0x5a5a5a5au;
    }
}
MetalBackend::Result Compile(id<MTLDevice> device, std::span<const std::uint32_t> code,
    std::span<const std::uint32_t> userData, std::uint32_t wave) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{"SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = device.maxThreadsPerThreadgroup;
    const SpirvTarget target{0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(limit.height), static_cast<std::uint32_t>(limit.depth)},
        static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<MemoryRegion, 1> regions{{{address, std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{wave, 1, 1}, 0, {false, false, false}, false, 1};
    RecompileRequest request{{ShaderStage::Compute, address, code, 0, {}},
        {wave, 0, userData, compute, {}, {}, regions}, target, {0, 0, 0, 128}};
    request.useCache = false;
    auto guest = Recompile(request);
    Require(!guest.spirv.empty(), "static-call frontend produced no SPIR-V");
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true; options.supportsGpuAddresses = true; options.supportsSimdGroups = true;
    return MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
}
void NativeCase(id<MTLDevice> device, id<MTLCommandQueue> queue, const char* name,
    std::span<const std::uint32_t> program, std::uint32_t wave) {
    const std::string label = std::string(name) + " wave" + std::to_string(wave);
    std::vector<std::uint32_t> input(Capacity * Components + Guard * 2);
    std::vector<std::uint32_t> output(input.size(), 0xdeadbeefu);
    for (std::size_t word = 0; word < input.size(); ++word) input[word] = 0xa6b5c4d3u ^ static_cast<std::uint32_t>(word * 31);
    for (std::uint32_t lane = 0; lane < Capacity; ++lane)
        for (std::uint32_t component = 0; component < Components; ++component)
            input[Guard + lane * Components + component] = Input(lane, component);
    const auto initialOutput = output;
    auto inBuffer = [device newBufferWithBytes:input.data() length:input.size() * 4 options:MTLResourceStorageModeShared];
    auto outBuffer = [device newBufferWithBytes:output.data() length:output.size() * 4 options:MTLResourceStorageModeShared];
    Require(inBuffer && outBuffer, label + " buffer allocation failed");
    const std::array<std::uint32_t, 4> inDescriptor{0x100000, 0, Capacity * Components * 4, 0x31016fac};
    const std::array<std::uint32_t, 4> outDescriptor{0x200000, 0, Capacity * Components * 4, 0x31016fac};
    std::array<std::uint32_t, 8> userData{};
    std::copy(inDescriptor.begin(), inDescriptor.end(), userData.begin());
    std::copy(outDescriptor.begin(), outDescriptor.end(), userData.begin() + 4);
    std::vector<std::uint32_t> guardedCode(Guard * 2 + program.size(), 0xffffffff);
    std::copy(program.begin(), program.end(), guardedCode.begin() + Guard);
    const auto originalCode = guardedCode;
    const auto code = std::span<const std::uint32_t>(guardedCode).subspan(Guard, program.size());
    auto shader = Compile(device, code, userData, wave);
    MetalComputePipeline pipeline(device, shader);
    std::vector<MetalShaderResourceBinding> bindings;
    bool boundInput = false, boundOutput = false;
    for (const auto& binding : shader.guest.bindings) {
        Require(binding.role == DescriptorRole::GuestBuffers && binding.guestDescriptor.size() == binding.count * 4u,
            label + " unexpected reflected descriptor");
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        for (std::uint32_t index = 0; index < binding.count; ++index) {
            const auto descriptor = binding.guestDescriptor.begin() + index * 4;
            const bool isInput = std::equal(inDescriptor.begin(), inDescriptor.end(), descriptor);
            const bool isOutput = std::equal(outDescriptor.begin(), outDescriptor.end(), descriptor);
            Require(isInput != isOutput, label + " unknown descriptor");
            native.buffers.push_back({isInput ? inBuffer : outBuffer, Guard * 4, Capacity * Components * 4});
            boundInput |= isInput; boundOutput |= isOutput;
        }
        bindings.push_back(std::move(native));
    }
    Require(boundInput && boundOutput, label + " missing native input/output bindings");
    auto command = [queue commandBuffer]; Require(command != nil, label + " command allocation failed");
    pipeline.Encode(command, bindings, MTLSizeMake(wave, 1, 1));
    [command commit]; [command waitUntilCompleted];
    Require(command.status == MTLCommandBufferStatusCompleted,
        label + " Metal command failed: " + (command.error.localizedDescription.UTF8String ?: "unknown"));
    Require(std::memcmp(inBuffer.contents, input.data(), input.size() * 4) == 0, label + " input/guard modified");
    Require(guardedCode == originalCode, label + " code/guard modified");
    std::memcpy(output.data(), outBuffer.contents, output.size() * 4);
    for (std::size_t word = 0; word < output.size(); ++word) {
        const bool active = word >= Guard && word < Guard + wave * Components;
        const auto expected = active ? Expected(static_cast<std::uint32_t>((word - Guard) / Components),
            static_cast<std::uint32_t>((word - Guard) % Components)) : initialOutput[word];
        Require(output[word] == expected, label + " output word=" + std::to_string(word) +
            " actual=" + std::to_string(output[word]) + " expected=" + std::to_string(expected));
    }
    std::cout << label << " PASS: Completed; arithmetic, inactive lanes, input/output/code guards\n";
}
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const std::string mode = argc > 1 ? argv[1] : "all";
            Require(mode == "all" || mode == "call" || mode == "swappc", "unknown static-call mode");
            auto device = MTLCreateSystemDefaultDevice(); Require(device != nil, "No Metal device available");
            auto queue = [device newCommandQueue]; Require(queue != nil, "No Metal command queue available");
            for (const auto wave : {32u, 64u}) {
                if (mode != "swappc") {
                    NativeCase(device, queue, "CALL forward", CallForward, wave);
                    NativeCase(device, queue, "CALL backward", CallBackward, wave);
                    NativeCase(device, queue, "CALL after END", CallAfterEnd, wave);
                }
                if (mode != "call") {
                    NativeCase(device, queue, "SWAPPC forward", SwappcForward, wave);
                    NativeCase(device, queue, "mixed nested", MixedNested, wave);
                }
            }
            std::cout << "native static calls PASS on " << device.name.UTF8String
                << "; guest wave32/64, native subgroup32. PS5 ISA parity and sampler interaction unqualified.\n";
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
