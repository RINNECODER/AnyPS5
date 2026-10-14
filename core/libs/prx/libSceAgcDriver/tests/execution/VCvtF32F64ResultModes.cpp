#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderFloatMode;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 4;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 14> Code{
    0x34020084, 0xe0381000, 0x80000401, 0xbf8c3f70, 0x7e141f04, 0xd58f000b, 0x0a010104, 0xd58f000c,
    0x12010104, 0xd58f000d, 0x1a010104, 0xe0781000, 0x80010a01, 0xbf810000,
};

constexpr std::uint64_t Sources[32] = {
    0x3ff0000000000000ull, 0xc004000000000000ull, 0x3fb999999999999aull, 0xbfd5555555555555ull,
    0x3ff0000010000000ull, 0x3ff0000010000001ull, 0x3ff0000030000000ull, 0x4153959fc5f99476ull,
    0xbc30000000000000ull, 0x400921fb54442d18ull, 0xc202a05f20000000ull, 0x40effc0000000000ull,
    0x4630000000000000ull, 0xb9b0000000000000ull, 0x3870000000000000ull, 0x3810000000000000ull,
    0x47efffffe0000000ull, 0x47e0000000000000ull, 0x7e37e43c8800759cull, 0x7ff0000000000000ull,
    0xfff0000000000000ull, 0x7ff8000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull,
    0x37d0000000000000ull, 0xb7d0000000000000ull, 0x36a0000000000000ull, 0xb6a0000000000000ull,
    0x37e8000000000000ull, 0xb734000000000000ull, 0x0000000000000001ull, 0x8000000000000001ull
};
constexpr std::uint32_t Expected[32][4] = {
    {0x3f800000u, 0x40000000u, 0x40800000u, 0x3f000000u},
    {0xc0200000u, 0xc0a00000u, 0xc1200000u, 0xbfa00000u},
    {0x3dcccccdu, 0x3e4ccccdu, 0x3ecccccdu, 0x3d4ccccdu},
    {0xbeaaaaabu, 0xbf2aaaabu, 0xbfaaaaabu, 0xbe2aaaabu},
    {0x3f800000u, 0x40000000u, 0x40800000u, 0x3f000000u},
    {0x3f800001u, 0x40000001u, 0x40800001u, 0x3f000001u},
    {0x3f800002u, 0x40000002u, 0x40800002u, 0x3f000002u},
    {0x4a9cacfeu, 0x4b1cacfeu, 0x4b9cacfeu, 0x4a1cacfeu},
    {0xa1800000u, 0xa2000000u, 0xa2800000u, 0xa1000000u},
    {0x40490fdbu, 0x40c90fdbu, 0x41490fdbu, 0x3fc90fdbu},
    {0xd01502f9u, 0xd09502f9u, 0xd11502f9u, 0xcf9502f9u},
    {0x477fe000u, 0x47ffe000u, 0x487fe000u, 0x46ffe000u},
    {0x71800000u, 0x72000000u, 0x72800000u, 0x71000000u},
    {0x8d800000u, 0x8e000000u, 0x8e800000u, 0x8d000000u},
    {0x03800000u, 0x04000000u, 0x04800000u, 0x03000000u},
    {0x00800000u, 0x01000000u, 0x01800000u, 0x00000000u},
    {0x7f7fffffu, 0x7f800000u, 0x7f800000u, 0x7effffffu},
    {0x7f000000u, 0x7f800000u, 0x7f800000u, 0x7e800000u},
    {0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u},
    {0x7f800000u, 0x7f800000u, 0x7f800000u, 0x7f800000u},
    {0xff800000u, 0xff800000u, 0xff800000u, 0xff800000u},
    {0x7fc00000u, 0x7fc00000u, 0x7fc00000u, 0x7fc00000u},
    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
    {0x80000000u, 0x00000000u, 0x00000000u, 0x00000000u}
};
constexpr std::uint32_t Kept[32] = {
    0x3f800000u, 0xc0200000u, 0x3dcccccdu, 0xbeaaaaabu, 0x3f800000u, 0x3f800001u, 0x3f800002u, 0x4a9cacfeu,
    0xa1800000u, 0x40490fdbu, 0xd01502f9u, 0x477fe000u, 0x71800000u, 0x8d800000u, 0x03800000u, 0x00800000u,
    0x7f7fffffu, 0x7f000000u, 0x7f800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00000000u, 0x80000000u,
    0x00080000u, 0x80080000u, 0x00000001u, 0x80000001u, 0x00180000u, 0x80000280u, 0x00000000u, 0x80000000u
};
constexpr const char* Names[Results] = {"v_cvt_f32_f64", "v_cvt_f32_f64 mul:2", "v_cvt_f32_f64 mul:4", "v_cvt_f32_f64 div:2"};

void Fill() {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint64_t source = Sources[tid % std::size(Sources)];
        std::uint32_t* words = &Input[tid * Inputs];
        words[0] = static_cast<std::uint32_t>(source);
        words[1] = static_cast<std::uint32_t>(source >> 32u);
        words[2] = 0u;
        words[3] = 0u;
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

bool IsDenormal(std::uint32_t value) {
    return (value & 0x7f800000u) == 0u && (value & 0x7fffffffu) != 0u;
}

void Run(AgcDriver::VulkanDevice& device, const std::optional<ShaderFloatMode>& floatMode, std::uint32_t waveSize) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    request.context.floatMode = floatMode;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(const std::string& mode, bool modifierApplied, bool denormalsKept) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const std::uint32_t row = tid % std::size(Sources);
        for (std::uint32_t i = 0; i < Results; ++i) {
            const std::uint32_t expected = denormalsKept ? Kept[row] : Expected[row][modifierApplied ? i : 0u];
            if (IsDenormal(expected)) continue;
            const std::uint32_t actual = Output[tid * Results + i];
            Require(actual == expected, "v_cvt_f32_f64 result modes: " + mode + " lane " + std::to_string(tid) + " " + Names[i] + " is " + Hex(actual) + ", expected " + Hex(expected));
        }
    }
}

void RequireRefused(AgcDriver::VulkanDevice& device, std::uint32_t floatMode, const char* reason) {
    bool refused = false;
    try {
        Run(device, ShaderFloatMode{floatMode, true, false, false}, 32u);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find(reason) != std::string::npos;
    }
    Require(refused, "v_cvt_f32_f64 result modes: FLOAT_MODE " + Hex(floatMode) + " was not refused");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (!TargetHasCapability(device->Target(), spv::CapabilityFloat64)) {
            std::puts("skipped, the device has no shaderFloat64");
            return VulkanTestSkipped;
        }
        Fill();
        for (const std::uint32_t waveSize : {32u, 64u}) {
            const std::string wave = "wave" + std::to_string(waveSize) + " ";
            Run(*device, std::nullopt, waveSize);
            Check(wave + "no float mode", true, false);
            Run(*device, ShaderFloatMode{0xc0u, true, false, false}, waveSize);
            Check(wave + "IEEE=0 f32 denormals flushed", true, false);
            Run(*device, ShaderFloatMode{0x00u, true, false, false}, waveSize);
            Check(wave + "IEEE=0 all denormals flushed", true, false);
            Run(*device, ShaderFloatMode{0xc0u, true, true, false}, waveSize);
            Check(wave + "IEEE=1 f32 denormals flushed", false, false);
            Run(*device, ShaderFloatMode{0xf0u, true, false, false}, waveSize);
            Check(wave + "IEEE=0 f32 denormals kept", false, true);
            Run(*device, ShaderFloatMode{0xf0u, true, true, false}, waveSize);
            Check(wave + "IEEE=1 f32 denormals kept", false, true);
        }
        RequireRefused(*device, 0x30u, "output modifier");
        RequireRefused(*device, 0xd0u, "f32 denormal mode");
        RequireRefused(*device, 0xe0u, "f32 denormal mode");
        std::puts("v_cvt_f32_f64 result mode tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
