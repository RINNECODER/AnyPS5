#import <Foundation/Foundation.h>
#include "MetalShaderPipeline.hpp"
#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
using namespace AgcDriver::Metal;
constexpr std::uint32_t Threads = 32, Guard = 0xa6b5c4d3, Sentinel = 0xfedcba98;
struct Format {
    const char* name;
    std::uint32_t id, count, bytes;
    std::array<std::uint32_t, 4> widths;
    bool signedNorm;
};
constexpr std::array<Format, 22> Formats{{
    {"8_UNORM", 1, 1, 1, {8}, false}, {"8_SNORM", 2, 1, 1, {8}, true},
    {"16_UNORM", 7, 1, 2, {16}, false}, {"16_SNORM", 8, 1, 2, {16}, true},
    {"8_8_UNORM", 14, 2, 2, {8,8}, false}, {"8_8_SNORM", 15, 2, 2, {8,8}, true},
    {"16_16_UNORM", 23, 2, 4, {16,16}, false}, {"16_16_SNORM", 24, 2, 4, {16,16}, true},
    {"11_11_10_UNORM", 30, 3, 4, {11,11,10}, false}, {"11_11_10_SNORM", 31, 3, 4, {11,11,10}, true},
    {"10_11_11_UNORM", 37, 3, 4, {10,11,11}, false}, {"10_11_11_SNORM", 38, 3, 4, {10,11,11}, true},
    {"2_10_10_10_UNORM", 44, 4, 4, {2,10,10,10}, false}, {"2_10_10_10_SNORM", 45, 4, 4, {2,10,10,10}, true},
    {"10_10_10_2_UNORM", 50, 4, 4, {10,10,10,2}, false}, {"10_10_10_2_SNORM", 51, 4, 4, {10,10,10,2}, true},
    {"8_8_8_8_UNORM", 56, 4, 4, {8,8,8,8}, false}, {"8_8_8_8_SNORM", 57, 4, 4, {8,8,8,8}, true},
    {"16_16_16_16_UNORM", 65, 4, 8, {16,16,16,16}, false}, {"16_16_16_16_SNORM", 66, 4, 8, {16,16,16,16}, true},
    {"16_16_16_16_UNORM_swizzle_constants", 65, 4, 8, {16,16,16,16}, false},
    {"8_8_8_8_SNORM_swizzle_constants", 57, 4, 4, {8,8,8,8}, true},
}};
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Hex(std::uint32_t value) { char text[16]; std::snprintf(text, sizeof(text), "0x%08x", value); return text; }
std::uint32_t RationalF32(std::uint32_t numerator, std::uint32_t denominator) {
    if (numerator == 0) return 0;
    Require(numerator <= denominator && denominator != 0, "Invalid rational reference input");
    int exponent = 0;
    std::uint64_t scaled = numerator;
    while (scaled < denominator) { scaled *= 2; --exponent; }
    scaled = static_cast<std::uint64_t>(numerator) << (23 - exponent);
    auto significand = scaled / denominator;
    const auto remainder = scaled % denominator;
    if (remainder * 2 > denominator || (remainder * 2 == denominator && (significand & 1))) ++significand;
    if (significand == (1u << 24)) { significand >>= 1; ++exponent; }
    return (static_cast<std::uint32_t>(exponent + 127) << 23) | static_cast<std::uint32_t>(significand - (1u << 23));
}
std::uint32_t Reference(std::uint32_t raw, std::uint32_t width, bool signedNorm) {
    const auto mask = (1u << width) - 1;
    if (!signedNorm) return RationalF32(raw, mask);
    const auto signBit = 1u << (width - 1);
    const auto signedValue = (raw & signBit) ? static_cast<std::int32_t>(raw) - static_cast<std::int32_t>(1u << width) : static_cast<std::int32_t>(raw);
    const auto magnitude = static_cast<std::uint32_t>(signedValue < 0 ? -signedValue : signedValue);
    return RationalF32(std::min(magnitude, signBit - 1), signBit - 1) | (signedValue < 0 ? 0x80000000u : 0u);
}
std::uint32_t Raw(std::uint32_t lane, std::uint32_t component, std::uint32_t width) {
    return (lane + 97u * component) & ((1u << width) - 1);
}
void CheckReferenceAnchors() {
    constexpr std::array<std::array<std::uint32_t, 4>, 7> claims{{
        {8, 1, 0, 0x3b808081}, {8, 0x38, 0, 0x3e60e0e1}, {8, 0xa5, 0, 0x3f25a5a6},
        {16, 1, 0, 0x37800080}, {16, 0x4de4, 0, 0x3e9bc89c},
        {8, 0xff, 1, 0xbc010204}, {16, 0xffff, 1, 0xb8000100},
    }};
    for (const auto& claim : claims) Require(Reference(claim[1], claim[0], claim[2] != 0) == claim[3], "Independent rational reference disagrees with cited upstream RDNA claim");
    Require(RationalF32(1, 3) == 0x3eaaaaab && RationalF32(2, 3) == 0x3f2aaaab, "Rational reference fixed anchors failed");
    Require(Reference(0x80, 8, true) == 0xbf800000 && Reference(0x81, 8, true) == 0xbf800000 && Reference(0, 8, true) == 0,
        "SNORM saturation/zero fixed anchors failed");
    std::cout << "Independent exact rational f32 reference agrees with 7 pinned upstream claimed-RDNA anchors; these are source claims, not local RDNA measurements\n";
}
std::array<std::uint32_t, 4> Descriptor(std::uint32_t address, std::uint32_t bytes, std::uint32_t format, std::uint32_t swizzle = 0xfac) {
    return {address, 0, bytes, 0x31000000u | (format << 12) | swizzle};
}
MetalBackend::Result Compile(id<MTLDevice> device, std::span<const std::uint32_t> code, std::span<const std::uint32_t> data) {
    static constexpr std::array<std::uint32_t, 3> capabilities{
        spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 2> extensions{
        "SPV_KHR_physical_storage_buffer", "SPV_KHR_8bit_storage"};
    const auto limit = device.maxThreadsPerThreadgroup;
    SpirvTarget target{0x00401000, 0x00010300, 32, BdaAbi::Version, capabilities, extensions, false,
        {static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(limit.height), static_cast<std::uint32_t>(limit.depth)},
        static_cast<std::uint32_t>(limit.width), static_cast<std::uint32_t>(device.maxThreadgroupMemoryLength), {}, {}};
    const auto address = reinterpret_cast<std::uintptr_t>(code.data());
    const std::array<MemoryRegion, 1> regions{{{address, std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {true, false, false}, false, 1};
    RecompileRequest request{{ShaderStage::Compute, address, code, 0, {}}, {32, 0, data, compute, {}, {}, regions}, target, {0, 0, 0, 128}};
    request.useCache = false;
    const auto guest = Recompile(request);
    Require(!guest.spirv.empty(), "Normalized load frontend produced no SPIR-V");
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true; options.supportsGpuAddresses = true; options.supportsSimdGroups = true;
    return MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
}
void Run(id<MTLDevice> device, id<MTLCommandQueue> queue, const Format& format, bool typed) {
    const bool constantSwizzle = std::strstr(format.name, "swizzle") != nullptr;
    const auto lanes = 1u << *std::max_element(format.widths.begin(), format.widths.end());
    constexpr std::uint32_t guardWords = 16, guardBytes = guardWords * 4, wordsPerLane = 4;
    std::vector<std::uint32_t> input(lanes * wordsPerLane + guardWords * 2, Guard);
    std::vector<std::uint32_t> output(input.size(), Guard);
    std::fill(output.begin() + guardWords, output.end() - guardWords, Sentinel);
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
        std::uint64_t packed = 0;
        std::uint32_t offset = 0;
        for (std::uint32_t component = 0; component < format.count; ++component) {
            packed |= static_cast<std::uint64_t>(Raw(lane, component, format.widths[component])) << offset;
            offset += format.widths[component];
        }
        std::memcpy(input.data() + guardWords + lane * wordsPerLane, &packed, format.bytes);
    }
    auto inBuffer = [device newBufferWithBytes:input.data() length:input.size() * 4 options:MTLResourceStorageModeShared];
    auto outBuffer = [device newBufferWithBytes:output.data() length:output.size() * 4 options:MTLResourceStorageModeShared];
    Require(inBuffer && outBuffer, "Normalized load buffer allocation failed");
    const auto inDescriptor = Descriptor(0x100000, lanes * 16, format.id, constantSwizzle ? 0x908 : 0xfac);
    const auto outDescriptor = Descriptor(0x200000, lanes * 16, 22);
    std::array<std::uint32_t, 8> userData{};
    std::copy(inDescriptor.begin(), inDescriptor.end(), userData.begin());
    std::copy(outDescriptor.begin(), outDescriptor.end(), userData.begin() + 4);
    const std::uint32_t load = typed ? 0xe8001000u | ((format.count - 1) << 16) | ((format.id & 15) << 19) | ((format.id >> 4) << 23) : 0xe00c1000u;
    const std::array<std::uint32_t, 19> code{
        0x7e020208, 0x34020285, 0x4a000101, 0x34040084, 0x34060084,
        0x7e0802ff, Sentinel, 0x7e0a02ff, Sentinel, 0x7e0c02ff, Sentinel, 0x7e0e02ff, Sentinel,
        load, 0x80000402, 0xbf8c3f70, 0xe0781000, 0x80010403, 0xbf810000};
    const auto shader = Compile(device, code, userData);
    MetalComputePipeline pipeline(device, shader);
    std::vector<MetalShaderResourceBinding> bindings;
    bool boundInput = false, boundOutput = false;
    for (const auto& binding : shader.guest.bindings) {
        Require(binding.role == DescriptorRole::GuestBuffers && binding.guestDescriptor.size() == binding.count * 4u,
            "Normalized load fixture reflected an unexpected descriptor");
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        for (std::uint32_t i = 0; i < binding.count; ++i) {
            const auto descriptor = binding.guestDescriptor.begin() + i * 4u;
            const bool isInput = std::equal(inDescriptor.begin(), inDescriptor.end(), descriptor);
            const bool isOutput = std::equal(outDescriptor.begin(), outDescriptor.end(), descriptor);
            Require(isInput != isOutput, "Unknown normalized load descriptor");
            native.buffers.push_back({isInput ? inBuffer : outBuffer, guardBytes, lanes * 16});
            boundInput |= isInput; boundOutput |= isOutput;
        }
        bindings.push_back(std::move(native));
    }
    Require(boundInput && boundOutput, "Normalized load fixture missing native input/output binding");
    auto commands = [queue commandBuffer];
    pipeline.Encode(commands, bindings, MTLSizeMake(lanes, 1, 1));
    [commands commit]; [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted,
        std::string("Normalized load Metal dispatch failed: ") + (commands.error.localizedDescription.UTF8String ?: "unknown"));
    Require(std::memcmp(inBuffer.contents, input.data(), input.size() * 4) == 0, "Normalized load shader modified input/guards");
    std::memcpy(output.data(), outBuffer.contents, output.size() * 4);
    for (std::uint32_t i = 0; i < guardWords; ++i) Require(output[i] == Guard && output[output.size() - guardWords + i] == Guard, "Normalized load shader corrupted output guards");
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
        for (std::uint32_t word = 0; word < wordsPerLane; ++word) {
            const auto component = word % format.count;
            std::uint32_t expected = typed && word >= format.count ? Sentinel : Reference(Raw(lane, component, format.widths[component]), format.widths[component], format.signedNorm);
            if (constantSwizzle) {
                constexpr std::array<std::uint32_t, 4> selectors{0, 1, 4, 4};
                const auto selector = selectors[word];
                expected = selector == 0 ? 0 : selector == 1 ? 0x3f800000 : Reference(Raw(lane, 0, format.widths[0]), format.widths[0], format.signedNorm);
            }
            const auto actual = output[guardWords + lane * wordsPerLane + word];
            Require(actual == expected, std::string(typed ? "typed " : "descriptor ") + format.name + " lane " + std::to_string(lane) + " component " + std::to_string(word) + " got " + Hex(actual) + " expected " + Hex(expected));
        }
    }
    std::cout << (typed ? "typed " : "descriptor ") << format.name << ": " << lanes << " native lanes, exhaustive component raw values, exact f32 and allocation guards passed\n";
}
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            CheckReferenceAnchors();
            const std::string mode = argc > 1 ? argv[1] : "all";
            Require(mode == "all" || mode == "descriptor" || mode == "typed" || mode == "oracle", "Unknown normalized load mode");
            if (mode == "oracle") return 0;
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "No Metal device available");
            auto queue = [device newCommandQueue]; Require(queue != nil, "No Metal command queue available");
            for (const auto& format : Formats) {
                if (mode == "all" || mode == "descriptor") Run(device, queue, format, false);
                if ((mode == "all" || mode == "typed") && std::strstr(format.name, "swizzle") == nullptr) Run(device, queue, format, true);
            }
            std::cout << "Normalized buffer loads " << mode << " passed on " << device.name.UTF8String << '\n';
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    }
}
