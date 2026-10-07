// Native Metal adaptation of boykopovar/AnyPS5 shader corrections, 2026-10-07.
// Upstream authors: Adria Franch and DotDebian. GPL-2.0; see repository LICENSE.
// Provenance: b8e1936, 5864b20, f08e691, cb1f456 (full identities in lane packet).
#import <Foundation/Foundation.h>
#include "MetalShaderPipeline.hpp"
#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ShaderRecompiler;
using namespace AgcDriver::Metal;
constexpr std::uint32_t Threads = 32, Sentinel = 0xcafebeefu, Guard = 0x91a5d33du;
using Row = std::array<std::uint32_t, 4>;
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::string Hex(std::uint32_t value) { char text[16]; std::snprintf(text, sizeof(text), "0x%08x", value); return text; }
std::array<std::uint32_t, 4> Descriptor(std::uint32_t address, std::uint32_t bytes) {
    return {address, 0, bytes, 0x01016facu};
}
MetalBackend::Result Compile(id<MTLDevice> device, std::span<const std::uint32_t> code,
                             std::span<const std::uint32_t> data, std::uint32_t lds = 0) {
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
    const ShaderComputeStageInfo compute{{Threads, 1, 1}, lds, {false, false, false}, false, 1};
    RecompileRequest request{{ShaderStage::Compute, address, code, 0, {}},
        {32, 0, data, compute, {}, {}, regions}, target, {0, 0, 0, 128}};
    request.useCache = false;
    const auto guest = Recompile(request);
    Require(!guest.spirv.empty(), "Shader correctness frontend produced no SPIR-V");
    MetalBackend::TargetOptions options;
    options.supportsInt64 = true;
    options.supportsGpuAddresses = true;
    options.supportsSimdGroups = true;
    return MetalBackend::ConvertToMetal(guest, ShaderStage::Compute, options);
}

// Strongest boundary: guest RDNA -> production IR/SPIR-V -> MSL -> GPU readback.
// Credible regressions: rejection/dropped clamp, integer truncation and M0-based LDS address.
// GuestShaderReplay covers LDS64 and general ALU, but no cases for these contracts.
// No test-only production seam. Expected words below come from arithmetic/ISA contracts.
std::vector<std::uint32_t> Dispatch(id<MTLDevice> device, id<MTLCommandQueue> queue,
    std::span<const std::uint32_t> code, std::span<const std::uint32_t> input,
    std::uint32_t wordsPerLane, std::uint32_t lds = 0) {
    constexpr std::uint32_t guardWords = 16, guardBytes = guardWords * 4;
    std::vector<std::uint32_t> in(input.size() + guardWords * 2, Guard);
    std::copy(input.begin(), input.end(), in.begin() + guardWords);
    std::vector<std::uint32_t> out(Threads * wordsPerLane + guardWords * 2, Guard);
    std::fill(out.begin() + guardWords, out.end() - guardWords, Sentinel);
    auto inBuffer = [device newBufferWithBytes:in.data() length:in.size() * 4 options:MTLResourceStorageModeShared];
    auto outBuffer = [device newBufferWithBytes:out.data() length:out.size() * 4 options:MTLResourceStorageModeShared];
    Require(inBuffer != nil && outBuffer != nil, "Shader correctness buffer allocation failed");
    const auto inDescriptor = Descriptor(0x100000, static_cast<std::uint32_t>(input.size() * 4));
    const auto outDescriptor = Descriptor(0x200000, Threads * wordsPerLane * 4);
    std::array<std::uint32_t, 8> userData{};
    // The LDS fixture uses s[0:3] for its only output descriptor.
    if (input.empty()) std::copy(outDescriptor.begin(), outDescriptor.end(), userData.begin());
    else {
        std::copy(inDescriptor.begin(), inDescriptor.end(), userData.begin());
        std::copy(outDescriptor.begin(), outDescriptor.end(), userData.begin() + 4);
    }
    const auto shader = Compile(device, code, userData, lds);
    MetalComputePipeline pipeline(device, shader);
    std::vector<MetalShaderResourceBinding> bindings;
    bool boundOutput = false;
    for (const auto& binding : shader.guest.bindings) {
        Require(binding.role == DescriptorRole::GuestBuffers, "Unexpected shader correctness descriptor role");
        Require(binding.guestDescriptor.size() == binding.count * 4u, "Incomplete descriptor reflection");
        MetalShaderResourceBinding native{binding.descriptorSet, binding.binding};
        for (std::uint32_t i = 0; i < binding.count; ++i) {
            const auto descriptor = binding.guestDescriptor.begin() + i * 4u;
            const bool isInput = std::equal(inDescriptor.begin(), inDescriptor.end(), descriptor);
            const bool isOutput = std::equal(outDescriptor.begin(), outDescriptor.end(), descriptor);
            Require(isInput != isOutput, "Unknown shader correctness guest descriptor");
            if (isInput) native.buffers.push_back({inBuffer, guardBytes, input.size() * 4});
            else { native.buffers.push_back({outBuffer, guardBytes, Threads * wordsPerLane * 4}); boundOutput = true; }
        }
        bindings.push_back(std::move(native));
    }
    Require(boundOutput, "Shader correctness fixture did not bind output");
    auto commands = [queue commandBuffer];
    pipeline.Encode(commands, bindings, MTLSizeMake(Threads, 1, 1));
    [commands commit]; [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted,
        std::string("Shader correctness Metal dispatch failed: ") + (commands.error.localizedDescription.UTF8String ?: "unknown"));
    Require(std::memcmp(inBuffer.contents, in.data(), in.size() * 4) == 0, "Guest shader modified input or input guards");
    std::memcpy(out.data(), outBuffer.contents, out.size() * 4);
    for (std::uint32_t i = 0; i < guardWords; ++i) {
        Require(out[i] == Guard && out[out.size() - guardWords + i] == Guard, "Guest shader wrote outside output allocation");
    }
    return {out.begin() + guardWords, out.end() - guardWords};
}
std::vector<std::uint32_t> AluCode(std::uint32_t word0, std::uint32_t word1) {
    // Byte offsets: lane * 16; load v[4:7], initialize v[10:13] from v7, ALU -> v10, store four words.
    return {0x34020084, 0x34060084, 0xe0381000, 0x80000401, 0xbf8c3f70,
        0x7e140307, 0x7e160307, 0x7e180307, 0x7e1a0307, word0, word1,
        0xe0781000, 0x80010a03, 0xbf810000};
}
void AluCase(id<MTLDevice> device, id<MTLCommandQueue> queue, const char* name,
    std::uint32_t word0, std::uint32_t word1, const std::array<Row, 8>& rows,
    const std::array<std::uint32_t, 8>& expected) {
    std::vector<std::uint32_t> input;
    for (std::uint32_t lane = 0; lane < Threads; ++lane) input.insert(input.end(), rows[lane % 8].begin(), rows[lane % 8].end());
    const auto result = Dispatch(device, queue, AluCode(word0, word1), input, 4);
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        Require(result[lane * 4] == expected[lane % 8], std::string(name) + " lane " + std::to_string(lane) +
            " got " + Hex(result[lane * 4]) + " expected " + Hex(expected[lane % 8]));
        for (std::uint32_t j = 1; j < 4; ++j) Require(result[lane * 4 + j] == rows[lane % 8][3], std::string(name) + " changed neighboring result register");
    }
    std::cout << name << ": 32 native lanes and allocation/input guards passed\n";
}
void Packed(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    constexpr std::array<Row, 8> rows{{
        {0, 65535, 0, Sentinel}, {65536, 0xffffffff, 0, Sentinel},
        {32767, 32768, 0, Sentinel}, {0xffff8000, 0xffff7fff, 0, Sentinel},
        {0x80000000, 0x7fffffff, 0, Sentinel}, {1, 0xffffffff, 0, Sentinel},
        {65534, 65536, 0, Sentinel}, {0x12345678, 0xfedcba98, 0, Sentinel}}};
    constexpr std::array<std::uint32_t, 8> unsignedExpected{
        0xffff0000, 0xffffffff, 0x80007fff, 0xffffffff, 0xffffffff, 0xffff0001, 0xfffffffe, 0xffffffff};
    constexpr std::array<std::uint32_t, 8> signedExpected{
        0x7fff0000, 0xffff7fff, 0x7fff7fff, 0x80008000, 0x7fff8000, 0xffff0001, 0x7fff7fff, 0x80007fff};
    AluCase(device, queue, "packed unsigned saturation", 0xd76a000a, 0x02020b04, rows, unsignedExpected);
    AluCase(device, queue, "packed signed saturation", 0xd76b000a, 0x02020b04, rows, signedExpected);
}
void Half(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    constexpr std::array<std::uint32_t, 8> values{0, 0x8000, 0x3800, 0x3c00, 0x4000, 0xb800, 0x7c00, 0x7e01};
    constexpr std::array<std::uint32_t, 8> clamp{0, 0, 0x3800, 0x3c00, 0x3c00, 0, 0x3c00, 0};
    const auto rowsFor = [&](std::uint32_t second, std::uint32_t third = 0) {
        std::array<Row, 8> rows{};
        for (std::uint32_t i = 0; i < 8; ++i) rows[i] = {values[i], second, third, Sentinel};
        return rows;
    };
    const auto high = [](std::array<std::uint32_t, 8> low) {
        for (auto& value : low) value |= Sentinel & 0xffff0000u;
        return low;
    };
    AluCase(device, queue, "f16 add clamp", 0xd532800a, 0x02020b04, rowsFor(0), high(clamp));
    AluCase(device, queue, "f16 sub clamp", 0xd533800a, 0x02020b04, rowsFor(0), high(clamp));
    AluCase(device, queue, "f16 subrev clamp", 0xd534800a, 0x02020b04, rowsFor(0), high({0, 0, 0, 0, 0, 0x3800, 0, 0}));
    for (std::uint32_t omod = 0; omod < 4; ++omod)
        AluCase(device, queue, "f16 mul clamp/omod with denormals", 0xd535800a, 0x02020b04 | (omod << 27u), rowsFor(0x3c00), high(clamp));
    AluCase(device, queue, "f16 min clamp", 0xd53a800a, 0x02020b04, rowsFor(0x3c00), high({0, 0, 0x3800, 0x3c00, 0x3c00, 0, 0x3c00, 0x3c00}));
    AluCase(device, queue, "f16 max clamp", 0xd539800a, 0x02020b04, rowsFor(0), high(clamp));
    AluCase(device, queue, "f16 ldexp clamp", 0xd53b800a, 0x02020b04, rowsFor(0), high(clamp));
    AluCase(device, queue, "f16 frexp mantissa clamp", 0xd5d9800a, 0x02010104, rowsFor(0), high({0, 0, 0x3800, 0x3800, 0x3800, 0, 0x3c00, 0}));
    AluCase(device, queue, "f16 normalized signed conversion modifiers", 0xd5e3800a, 0x02010104, rowsFor(0), high({0, 0, 0x4000, 0x7fff, 0x7fff, 0xc000, 0x7fff, 0}));
    AluCase(device, queue, "f16 normalized unsigned conversion modifiers", 0xd5e4800a, 0x02010104, rowsFor(0), high({0, 0, 0x8000, 0xffff, 0xffff, 0, 0xffff, 0}));
    // Div fixup applies the numerator/denominator sign to magnitude(v4).
    AluCase(device, queue, "f16 division fixup clamp", 0xd75f800a, 0x041a0b04, rowsFor(0x3c00, 0x3c00), high({0, 0, 0x3800, 0x3c00, 0x3c00, 0x3800, 0x3c00, 0x3c00}));
    // Keep finite, unequal operands to give ternary clamp a deterministic numerical oracle.
    const std::array<Row, 8> ternary{{
        {0x4000, 0x4200, 0x4400, Sentinel}, {0xbc00, 0xc000, 0xc200, Sentinel},
        {0x3400, 0x3800, 0x3a00, Sentinel}, {0x4000, 0x4200, 0x4400, Sentinel},
        {0xbc00, 0xc000, 0xc200, Sentinel}, {0x3400, 0x3800, 0x3a00, Sentinel},
        {0x4000, 0x4200, 0x4400, Sentinel}, {0xbc00, 0xc000, 0xc200, Sentinel}}};
    AluCase(device, queue, "f16 max3 clamp", 0xd754800a, 0x041a0b04, ternary, high({0x3c00, 0, 0x3a00, 0x3c00, 0, 0x3a00, 0x3c00, 0}));
    AluCase(device, queue, "f16 min3 clamp", 0xd751800a, 0x041a0b04, ternary, high({0x3c00, 0, 0x3400, 0x3c00, 0, 0x3400, 0x3c00, 0}));
    AluCase(device, queue, "f16 med3 clamp", 0xd757800a, 0x041a0b04, ternary, high({0x3c00, 0, 0x3800, 0x3c00, 0, 0x3800, 0x3c00, 0}));
}
void Sdwa(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    // WORD_1 input (.5) and WORD_1 exponent (1), WORD_1 output with low-half preservation.
    std::array<Row, 8> rows{}; rows.fill({0x38001234, 0x00015678, 0, Sentinel});
    std::array<std::uint32_t, 8> expected{}; expected.fill(0x3c00beef);
    AluCase(device, queue, "SDWA ldexp word selectors/preserve", 0x76140af9, 0x05051504, rows, expected);
    expected.fill(0x3c00beef);
    AluCase(device, queue, "SDWA ldexp word selectors clamp/omod", 0x76140af9, 0x0505f504, rows, expected);
    // BYTE_3 gives half bit-pattern 0x38, multiplied by 2^0, stored BYTE_1, untouched bits preserved.
    rows.fill({0x38123456, 0, 0, Sentinel}); expected.fill(0xcafe38ef);
    AluCase(device, queue, "SDWA ldexp byte selectors/preserve", 0x76140af9, 0x06031104, rows, expected);
}
// Upstream machine-code fixture adapted for raw byte-addressed descriptors:
// v3 = lane << 6 gives each lane its 64-byte output row. Upstream used lane << 4
// with a four-byte descriptor stride; all twelve stores now use OFFEN, not IDXEN.
// M0.base 64 and nonzero/zero M0.size;
// 32 active lanes then 16 active lanes; counters at offsets 16/20/24/28 only.
constexpr std::uint32_t AppendStart = 1000, ConsumeStart = 2000, MaskedAppendStart = 3000,
    MaskedConsumeStart = 4000, BaseShiftedStart = 5000, UntouchedAppend = 0x5eed0001,
    UntouchedConsume = 0x5eed0002;
constexpr auto LdsCode = std::to_array<std::uint32_t>({
    0x34060086, 0x7e100280,
    0x7e0802ff, AppendStart, 0x7e0a02ff, ConsumeStart, 0x7e0c02ff, MaskedAppendStart, 0x7e0e02ff, MaskedConsumeStart,
    0x7e1c02ff, BaseShiftedStart, 0x7e1e02ff, BaseShiftedStart + 1u, 0x7e2002ff, BaseShiftedStart + 2u, 0x7e2202ff, BaseShiftedStart + 3u,
    0x7e1802ff, UntouchedAppend, 0x7e1a02ff, UntouchedConsume,
    0x7da40080,
    0xd8340010, 0x00000408, 0xd8340014, 0x00000508, 0xd8340018, 0x00000608, 0xd834001c, 0x00000708,
    0xd8340050, 0x00000e08, 0xd8340054, 0x00000f08, 0xd8340058, 0x00001008, 0xd834005c, 0x00001108,
    0xbefe03c1,
    0xbf8cc07f,
    0xbefc03ff, 0x0040ffff,
    0xd8f80010, 0x0a000000,
    0xd8f40014, 0x0b000000,
    0xbefc03ff, 0x00400000,
    0x7da80090,
    0xd8f80018, 0x0c000000,
    0xd8f4001c, 0x0d000000,
    0xbefe03c1,
    0xbf8cc07f,
    0xd8d80010, 0x14000008, 0xd8d80014, 0x15000008, 0xd8d80018, 0x16000008, 0xd8d8001c, 0x17000008,
    0xd8d80050, 0x18000008, 0xd8d80054, 0x19000008, 0xd8d80058, 0x1a000008, 0xd8d8005c, 0x1b000008,
    0xbf8cc07f,
    0xe0701000, 0x80000a03, 0xe0701004, 0x80000b03, 0xe0701008, 0x80000c03, 0xe070100c, 0x80000d03,
    0xe0701010, 0x80001403, 0xe0701014, 0x80001503, 0xe0701018, 0x80001603, 0xe070101c, 0x80001703,
    0xe0701020, 0x80001803, 0xe0701024, 0x80001903, 0xe0701028, 0x80001a03, 0xe070102c, 0x80001b03,
    0xbf810000,
});

void Lds(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    const auto out = Dispatch(device, queue, LdsCode, {}, 16, 256);
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        const bool active = lane < 16;
        const std::array<std::uint32_t, 16> expected{
            1000, 2000, active ? 3000u : UntouchedAppend, active ? 4000u : UntouchedConsume,
            1032, 1968, 3016, 3984, 5000, 5001, 5002, 5003, Sentinel, Sentinel, Sentinel, Sentinel};
        for (std::uint32_t word = 0; word < expected.size(); ++word)
            Require(out[lane * 16 + word] == expected[word], "LDS append/consume lane " + std::to_string(lane) +
                " word " + std::to_string(word) + " got " + Hex(out[lane * 16 + word]) + " expected " + Hex(expected[word]));
    }
    std::cout << "LDS append/consume: offset-only counters, M0 independence, EXEC masking and 32-lane GPU readback passed\n";
}
}
int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "No Metal device available");
            auto queue = [device newCommandQueue];
            Require(queue != nil, "No Metal command queue available");
            const std::string mode = argc > 1 ? argv[1] : "all";
            Require(mode == "all" || mode == "packed" || mode == "half" || mode == "sdwa" || mode == "lds", "Unknown shader correctness mode");
            if (mode == "all" || mode == "packed") Packed(device, queue);
            if (mode == "all" || mode == "half") Half(device, queue);
            if (mode == "all" || mode == "sdwa") Sdwa(device, queue);
            if (mode == "all" || mode == "lds") Lds(device, queue);
            std::cout << "Native shader correctness " << mode << " passed on " << device.name.UTF8String << '\n';
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
