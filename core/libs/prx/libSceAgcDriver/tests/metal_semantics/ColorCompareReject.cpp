// The emulated color-texture comparison must refuse TRUNC_COORD with point filtering (upstream PR #907):
// the emulation does not reproduce its coordinate rounding. Needs only the recompile, so it builds a
// request like tests/execution/EmulatedColorCompare.cpp does and checks the diagnostic.
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP
#include "MetalTestDevice.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Side = 2;
constexpr std::uint32_t ClampEdge = 2;
constexpr std::uint32_t FilterPoint = 0;
constexpr std::uint32_t FilterBilinear = 1;
alignas(256) std::array<float, Threads * 3> Input{};
alignas(256) std::array<float, Threads * 4> Output{};
alignas(256) std::array<std::uint32_t, Threads * 3> Extra{};
alignas(4096) std::array<std::uint8_t, 4096> Texels{};

alignas(256) constexpr std::array<std::uint32_t, 13> Code{
    0x1614008c, 0xe03c1000, 0x8000010a, 0xbf8c3f70, 0xf0bc0108, 0x00820401, 0xbf8c3f70,
    0x34160082, 0xe0701000, 0x8001040b, 0xbf810000, 0xbf810000, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(std::uint32_t format) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Texels.data()));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::uint32_t format, std::uint32_t filter, std::uint32_t function, bool truncCoord) {
    std::vector<std::uint32_t> userData(24, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(sizeof(Input)));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(sizeof(Output)));
    const auto texture = TextureDescriptor(format);
    const std::array<std::uint32_t, 4> sampler{
        ClampEdge | (ClampEdge << 3u) | (ClampEdge << 6u) | (function << 12u) | (truncCoord ? 1u << 27u : 0u),
        0u,
        (filter << 20u) | (filter << 22u),
        0u,
    };
    const auto extra = BufferDescriptor(Extra.data(), static_cast<std::uint32_t>(sizeof(Extra)));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    std::copy(texture.begin(), texture.end(), userData.begin() + 8);
    std::copy(sampler.begin(), sampler.end(), userData.begin() + 16);
    std::copy(extra.begin(), extra.end(), userData.begin() + 20);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void Accepted(AgcDriver::VulkanDevice& device, std::uint32_t filter, bool truncCoord, const char* what) {
    try {
        static_cast<void>(Compile(device, Format8888UNorm, filter, 3u, truncCoord));
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string(what) + " was refused: " + error.what());
    }
}

void Rejected(AgcDriver::VulkanDevice& device, std::uint32_t function, std::string_view reason) {
    try {
        static_cast<void>(Compile(device, Format8888UNorm, FilterPoint, function, true));
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(reason) == std::string_view::npos) throw std::runtime_error(std::string("unexpected rejection: ") + error.what());
        return;
    }
    throw std::runtime_error("expected rejection: " + std::string(reason));
}
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        for (const std::uint32_t function : {1u, 3u, 5u, 6u}) Rejected(*device, function, "TRUNC_COORD for point sampling");
        Accepted(*device, FilterPoint, false, "point sampling without TRUNC_COORD");
        Accepted(*device, FilterBilinear, true, "bilinear sampling with TRUNC_COORD");
        std::puts("color compare TRUNC_COORD rejection tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
