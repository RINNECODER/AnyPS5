// A T# read with s_buffer_load from a table record that user data selects (upstream PR #5188), and a
// record whose T# has TYPE=0 and a nonzero base, which the hardware treats as null (upstream PR #5473).
// The upstream tests stop at the captured snapshot; this one samples through the Metal driver's image
// binding: the selected record's texture is read, and the TYPE=0 record binds no texture and reads zero.
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP
#include "MetalDriverTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "Recompiler.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Size = 4;

struct alignas(256) Texture {
    std::array<std::uint8_t, 16384> bytes{};
};
Texture Textures[2];

struct alignas(4096) GuestTables {
    std::array<std::array<std::uint32_t, 12>, 3> table{};
    std::array<std::uint32_t, 4> output{};
    std::array<std::uint32_t, 16> srt{};
};
GuestTables Guest;

// s_load_dwordx4 s[4:7] (table V#), s[8:11] (S#), s[28:31] (output V#) from the SRT; s_mul_i32 s16, s2, 48;
// s_buffer_load_dwordx8 s[20:27] at record s2 + 16 bytes; image_sample_lz v[0:3], v[0:1], s[20:27], s[8:11];
// buffer_store_dwordx4 v[0:3], s[28:31]. This is the upstream ShaderMemory test's program, which only
// captures and stores one dword, widened to all four components.
alignas(256) constexpr std::array<std::uint32_t, 14> Code{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000030u, 0x9310b002u, 0xf42c0502u,
    0x20000010u, 0xf09c0f08u, 0x00450000u, 0xe0780000u, 0x80070000u, 0xbf810000u,
};

std::array<std::uint32_t, 8> TextureDescriptor(const Texture& texture, std::uint32_t type) {
    const auto base = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texture.bytes.data()));
    return {static_cast<std::uint32_t>(base >> 8u), static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format8888UNorm << 20u) | (((Size - 1u) & 3u) << 30u),
        ((Size - 1u) >> 2u) | ((Size - 1u) << 14u), 0xfacu | (type << 28u), 0u, 0u, 0u, 0u};
}

void Fill(Texture& texture, std::array<std::uint8_t, 4> color) {
    const auto geometry = AgcDriver::Graphics::DescribeSurface(AgcDriver::Graphics::DecodeTextureResource(TextureDescriptor(texture, Type2D)));
    Require(geometry.guestBytes <= texture.bytes.size(), "image table record: the texture does not fit its storage");
    for (std::size_t offset = 0; offset + 4u <= texture.bytes.size(); offset += 4u) std::copy(color.begin(), color.end(), texture.bytes.begin() + offset);
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* base, std::uint32_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t key, std::array<std::uint8_t, 4> expected, const std::string& name) {
    Guest.output.fill(0xdeadbeefu);
    const auto srtAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Guest.srt.data()));
    const std::array<std::uint32_t, 3> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u), key};
    const std::span<const std::uint32_t> code(Code);
    // The SRT and the table are read while the shader is recompiled, as the driver's capture reads guest memory.
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)},
        {reinterpret_cast<std::uintptr_t>(&Guest), std::as_bytes(std::span(&Guest, 1))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    for (std::uint32_t component = 0; component < 4u; ++component) {
        const float value = std::bit_cast<float>(Guest.output[component]) * 255.0f;
        Require(std::lround(value) == static_cast<long>(expected[component]), name + ": component " + std::to_string(component) + " is " +
            std::to_string(value) + ", expected " + std::to_string(expected[component]));
    }
    std::printf("%s passed\n", name.c_str());
}
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        constexpr std::array<std::uint8_t, 4> first{10u, 20u, 30u, 255u};
        constexpr std::array<std::uint8_t, 4> second{200u, 150u, 100u, 50u};
        Fill(Textures[0], first);
        Fill(Textures[1], second);
        const auto typeZero = TextureDescriptor(Textures[0], 0u);
        const auto firstWords = TextureDescriptor(Textures[0], Type2D);
        const auto secondWords = TextureDescriptor(Textures[1], Type2D);
        std::copy(typeZero.begin(), typeZero.end(), Guest.table[0].begin() + 4);
        std::copy(firstWords.begin(), firstWords.end(), Guest.table[1].begin() + 4);
        std::copy(secondWords.begin(), secondWords.end(), Guest.table[2].begin() + 4);
        const auto table = BufferDescriptor(Guest.table.data(), static_cast<std::uint32_t>(sizeof(Guest.table)));
        const auto output = BufferDescriptor(Guest.output.data(), 16u);
        std::copy(table.begin(), table.end(), Guest.srt.begin());
        std::copy(output.begin(), output.end(), Guest.srt.begin() + 12);
        Run(*device, 2u, second, "T# of table record 2");
        Run(*device, 1u, first, "T# of table record 1");
        Run(*device, 0u, {0u, 0u, 0u, 0u}, "TYPE=0 T# with a nonzero base in table record 0");
        std::puts("image table record tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
