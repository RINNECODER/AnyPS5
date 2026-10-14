#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "SpirvBackend/SpirvBufferFormat.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::IrBufferFormat;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t LaneDwords = 16;
constexpr std::uint32_t Fill = 0xaaaaaaaau;
alignas(256) std::array<std::uint32_t, Threads * LaneDwords> Input{};
alignas(256) std::array<std::uint32_t, Threads * LaneDwords> Output{};
alignas(256) std::array<std::uint32_t, Threads * LaneDwords> Scratch{};

alignas(256) constexpr std::array<std::uint32_t, 41> Code{
    0x34020086, 0xe0381000, 0x80000401, 0xe0341010, 0x80000801, 0xe0381020, 0x80000c01, 0xe0381030,
    0x80001001, 0xbf8c3f70, 0xe01c1000, 0x80020401, 0xe21c1004, 0x80020801, 0xe0101008, 0x80020401,
    0xe01c1010, 0x80031001, 0xe01c1018, 0x80040c01, 0xe010101c, 0x80040c01, 0xe01c1020, 0x80050c01,
    0xe01c1024, 0x80061001, 0xbbfd0000, 0xe00c1000, 0x80021401, 0xe00c1010, 0x80031801, 0xe00c1018,
    0x80041c01, 0xbf8c3f70, 0xe0781000, 0x80011401, 0xe0781010, 0x80011801, 0xe0781020, 0x80011c01,
    0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 6> StoreCode{
    0x34020086, 0xe01c1000, 0x80020401, 0xe01c1020, 0x80050401, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 5> GpuSelectedStoreCode{
    0x34020086, 0x7e160501, 0xe01c1000, 0x80020401, 0xbf810000,
};

constexpr std::uint32_t Unorm8888ZyxwWord3 = 0x31038f2eu;
constexpr std::uint32_t Uint16x4ZyxwWord3 = 0x31045f2eu;
constexpr std::uint32_t Uint8AlphaWord3 = 0x31005800u;
constexpr std::uint32_t Uint32RedWord3 = 0x31014204u;
constexpr std::uint32_t Uint16x4IdentityWord3 = 0x31045facu;
constexpr std::size_t FourComponentWord3 = 11;
constexpr std::size_t OneComponentWord3 = 23;

constexpr std::uint32_t IdentityDstSel = 0xfacu;
constexpr std::uint32_t ZyxwDstSel = 0xf2eu;
constexpr std::uint32_t AlphaDstSel = 0x800u;
constexpr std::uint32_t RedDstSel = 0x204u;
constexpr std::uint32_t RedReservedDstSel = 0x404u;
constexpr std::array<IrBufferFormat, 2> ZyxwFormats{IrBufferFormat::Format8_8_8_8UNorm, IrBufferFormat::Format16_16_16_16UInt};
constexpr std::array<IrBufferFormat, 2> AlphaFormats{IrBufferFormat::Format8UNorm, IrBufferFormat::Format8UInt};

constexpr std::array<std::uint32_t, 4> ZyxwChannelSlots{2, 1, 0, 3};
constexpr std::array<std::uint32_t, 4> ShortBases{0x1234u, 0x5678u, 0x9abcu, 0xdef0u};
constexpr std::array<std::uint32_t, 2> Gfx1036Uint16x4ZyxwLane0{0x56789abcu, 0xdef01234u};
constexpr std::array<std::uint32_t, 2> Gfx1036Uint16x4IdentityLane0{0x56781234u, 0xdef09abcu};

constexpr std::array<std::uint32_t, 8> UnormInputs{0x00000000u, 0x3f800000u, 0x3e800000u, 0x3f000000u, 0x3f400000u, 0x40000000u, 0xbf800000u, 0x3e4ccccdu};
constexpr std::array<std::uint32_t, 8> HalfInputs{0x0000u, 0x3c00u, 0x3400u, 0x3800u, 0x3a00u, 0x4000u, 0xbc00u, 0x3266u};
constexpr std::array<std::uint32_t, 8> UnormBytes{0x00u, 0xffu, 0x40u, 0x80u, 0xbfu, 0xffu, 0x00u, 0x33u};
constexpr std::array<std::uint32_t, 8> UnormLoads{0x00000000u, 0x3f800000u, 0x3e808081u, 0x3f008081u, 0x3f3fbfc0u, 0x3f800000u, 0x00000000u, 0x3e4ccccdu};

std::uint32_t UnormIndex(std::uint32_t lane, std::uint32_t slot) {
    return (lane + slot * 3u + 1u) % 8u;
}

std::uint32_t HalfIndex(std::uint32_t lane, std::uint32_t slot) {
    return (lane + slot * 3u + 6u) % 8u;
}

std::uint32_t Byte(std::uint32_t lane, std::uint32_t slot) {
    return (lane * 4u + slot * 0x41u + 0x11u) & 0xffu;
}

std::uint32_t Short(std::uint32_t lane, std::uint32_t slot) {
    return (ShortBases[slot] + lane * 0x0101u) & 0xffffu;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes, std::uint32_t word3) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, word3};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::vector<std::uint32_t> UserData() {
    std::vector<std::uint32_t> userData(28u, 0u);
    const std::array<std::array<std::uint32_t, 4>, 7> descriptors{
        BufferDescriptor(Input.data(), sizeof(Input), 0x31016facu),
        BufferDescriptor(Output.data(), sizeof(Output), 0x31016facu),
        BufferDescriptor(Scratch.data(), sizeof(Scratch), Unorm8888ZyxwWord3),
        BufferDescriptor(Scratch.data(), sizeof(Scratch), Uint16x4ZyxwWord3),
        BufferDescriptor(Scratch.data(), sizeof(Scratch), Uint8AlphaWord3),
        BufferDescriptor(Scratch.data(), sizeof(Scratch), Uint32RedWord3),
        BufferDescriptor(Scratch.data(), sizeof(Scratch), Uint16x4IdentityWord3),
    };
    for (std::size_t index = 0; index < descriptors.size(); ++index) std::copy(descriptors[index].begin(), descriptors[index].end(), userData.begin() + 4 * index);
    return userData;
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t waveSize, std::span<const std::uint32_t> userData) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize) {
    Input.fill(0u);
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        auto* row = &Input[lane * LaneDwords];
        for (std::uint32_t slot = 0; slot < 4u; ++slot) {
            row[slot] = UnormInputs[UnormIndex(lane, slot)];
            row[4u + slot / 2u] |= HalfInputs[HalfIndex(lane, slot)] << (slot % 2u * 16u);
            row[8u + slot] = Byte(lane, slot);
            row[12u + slot] = Short(lane, slot);
        }
    }
    Output.fill(0xdeadbeefu);
    Scratch.fill(Fill);
    const auto userData = UserData();
    const auto result = Compile(device, Code, waveSize, userData);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
    device.WaitIdle();
}

void Expect(std::uint32_t waveSize, std::uint32_t lane, std::uint32_t actual, std::uint32_t expected, const std::string& name) {
    Require(actual == expected, "buffer store format dst_sel: wave" + std::to_string(waveSize) + " lane " + std::to_string(lane) + " " + name + " is " + Hex(actual) + ", expected " + Hex(expected));
}

void Check(std::uint32_t waveSize) {
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        const auto* scratch = &Scratch[lane * LaneDwords];
        const auto* output = &Output[lane * LaneDwords];
        std::uint32_t unorm = 0;
        std::uint32_t half = 0;
        for (std::uint32_t channel = 0; channel < 4u; ++channel) {
            unorm |= UnormBytes[UnormIndex(lane, ZyxwChannelSlots[channel])] << (channel * 8u);
            half |= UnormBytes[HalfIndex(lane, ZyxwChannelSlots[channel])] << (channel * 8u);
        }
        Expect(waveSize, lane, scratch[0], unorm, "buffer_store_format_xyzw 8_8_8_8_UNORM Z,Y,X,W");
        Expect(waveSize, lane, scratch[1], half, "buffer_store_format_d16_xyzw 8_8_8_8_UNORM Z,Y,X,W");
        Expect(waveSize, lane, scratch[2], UnormBytes[UnormIndex(lane, 0u)] << 16u, "buffer_store_format_x 8_8_8_8_UNORM Z,Y,X,W");
        Expect(waveSize, lane, scratch[3], Fill, "scratch dword 3");
        Expect(waveSize, lane, scratch[4], Short(lane, ZyxwChannelSlots[0]) | Short(lane, ZyxwChannelSlots[1]) << 16u, "buffer_store_format_xyzw 16_16_16_16_UINT Z,Y,X,W low");
        Expect(waveSize, lane, scratch[5], Short(lane, ZyxwChannelSlots[2]) | Short(lane, ZyxwChannelSlots[3]) << 16u, "buffer_store_format_xyzw 16_16_16_16_UINT Z,Y,X,W high");
        Expect(waveSize, lane, scratch[6], (Fill & ~0xffu) | Byte(lane, 3u), "buffer_store_format_xyzw 8_UINT 0,0,0,X");
        Expect(waveSize, lane, scratch[7], Fill & ~0xffu, "buffer_store_format_x 8_UINT 0,0,0,X");
        Expect(waveSize, lane, scratch[8], Byte(lane, 0u), "buffer_store_format_xyzw 32_UINT X,0,0,1");
        Expect(waveSize, lane, scratch[9], Short(lane, 0u) | Short(lane, 1u) << 16u, "buffer_store_format_xyzw 16_16_16_16_UINT X,Y,Z,W low");
        Expect(waveSize, lane, scratch[10], Short(lane, 2u) | Short(lane, 3u) << 16u, "buffer_store_format_xyzw 16_16_16_16_UINT X,Y,Z,W high");
        if (lane == 0u) {
            Expect(waveSize, lane, scratch[4], Gfx1036Uint16x4ZyxwLane0[0], "buffer_store_format_xyzw 16_16_16_16_UINT Z,Y,X,W low as measured on gfx1036");
            Expect(waveSize, lane, scratch[5], Gfx1036Uint16x4ZyxwLane0[1], "buffer_store_format_xyzw 16_16_16_16_UINT Z,Y,X,W high as measured on gfx1036");
            Expect(waveSize, lane, scratch[9], Gfx1036Uint16x4IdentityLane0[0], "buffer_store_format_xyzw 16_16_16_16_UINT X,Y,Z,W low as measured on gfx1036");
            Expect(waveSize, lane, scratch[10], Gfx1036Uint16x4IdentityLane0[1], "buffer_store_format_xyzw 16_16_16_16_UINT X,Y,Z,W high as measured on gfx1036");
        }
        for (std::uint32_t dword = 11; dword < LaneDwords; ++dword) Expect(waveSize, lane, scratch[dword], Fill, "scratch dword " + std::to_string(dword));
        for (std::uint32_t slot = 0; slot < 4u; ++slot) {
            const auto component = std::string(1, "xyzw"[slot]);
            Expect(waveSize, lane, output[slot], UnormLoads[UnormIndex(lane, slot)], "buffer_load_format_xyzw 8_8_8_8_UNORM Z,Y,X,W " + component);
            Expect(waveSize, lane, output[4u + slot], Short(lane, slot), "buffer_load_format_xyzw 16_16_16_16_UINT Z,Y,X,W " + component);
            Expect(waveSize, lane, output[8u + slot], slot == 3u ? Byte(lane, 3u) : 0u, "buffer_load_format_xyzw 8_UINT 0,0,0,X " + component);
        }
    }
}

std::string DstSelName(std::uint32_t dstSel) {
    std::string name;
    for (std::uint32_t slot = 0; slot < 4u; ++slot) name += std::string(slot == 0u ? "" : ",") + "01??XYZW"[(dstSel >> (slot * 3u)) & 7u];
    return name;
}

void ExpectSettled(IrBufferFormat format, std::uint32_t dstSel, bool expected) {
    const auto settled = ShaderRecompiler::IsSettledFormattedStoreDstSel(format, 0x31000000u | dstSel);
    Require(settled == expected, "buffer store format dst_sel: DST_SEL " + DstSelName(dstSel) + " on FORMAT " + std::to_string(static_cast<std::uint32_t>(format)) + (settled ? " is accepted" : " is refused"));
}

void CheckSettledSet() {
    std::array<std::uint32_t, 4> components{0, 1, 2, 3};
    do {
        std::uint32_t dstSel = 0;
        for (std::uint32_t slot = 0; slot < 4u; ++slot) dstSel |= (4u + components[slot]) << (slot * 3u);
        ExpectSettled(IrBufferFormat::Format16_16_16_16UInt, dstSel, dstSel == IdentityDstSel || dstSel == ZyxwDstSel);
    } while (std::next_permutation(components.begin(), components.end()));
    for (std::uint32_t value = 1; value <= static_cast<std::uint32_t>(IrBufferFormat::Format32_32_32_32Float); ++value) {
        const auto format = static_cast<IrBufferFormat>(value);
        const bool single = ShaderRecompiler::GetFormatInfo(format).componentCount == 1u;
        ExpectSettled(format, IdentityDstSel, true);
        ExpectSettled(format, ZyxwDstSel, std::find(ZyxwFormats.begin(), ZyxwFormats.end(), format) != ZyxwFormats.end());
        ExpectSettled(format, AlphaDstSel, std::find(AlphaFormats.begin(), AlphaFormats.end(), format) != AlphaFormats.end());
        ExpectSettled(format, RedDstSel, single);
        ExpectSettled(format, RedReservedDstSel, single);
    }
    ExpectSettled(IrBufferFormat::Format8_8_8_8UNorm, 0xfa4u, false);
    ExpectSettled(IrBufferFormat::Format8_8_8_8UNorm, 0x32eu, false);
    ExpectSettled(IrBufferFormat::Format32UInt, 0x801u, false);
}

std::string CompileError(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> userData) {
    try {
        static_cast<void>(Compile(device, StoreCode, 32u, userData));
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

struct Case {
    std::size_t word3;
    std::uint32_t value;
    const char* name;
};

void CheckThrows(AgcDriver::VulkanDevice& device) {
    constexpr std::array<Case, 7> refused{{
        {FourComponentWord3, 0x31045f35u, "Y,Z,X,W on 16_16_16_16_UINT"},
        {FourComponentWord3, 0x31045fa5u, "Y,X,Z,W on 16_16_16_16_UINT"},
        {FourComponentWord3, 0x31045bbcu, "X,W,Z,Y on 16_16_16_16_UINT"},
        {FourComponentWord3, 0x31047f2eu, "Z,Y,X,W on 16_16_16_16_FLOAT"},
        {FourComponentWord3, 0x3103cf2eu, "Z,Y,X,W on 8_8_8_8_UINT"},
        {FourComponentWord3, 0x31032f2eu, "Z,Y,X,W on 10_10_10_2_UNORM (FORMAT 50)"},
        {OneComponentWord3, 0x31014800u, "0,0,0,X on 32_UINT"},
    }};
    for (const auto& test : refused) {
        auto userData = UserData();
        userData[test.word3] = test.value;
        const auto error = CompileError(device, userData);
        Require(!error.empty(), std::string("buffer store format dst_sel: a store through DST_SEL ") + test.name + " compiled");
        Require(error.find("formatted buffer store through a V# whose DST_SEL") != std::string::npos, std::string("buffer store format dst_sel: ") + test.name + " failed with an unrelated error: " + error);
    }
    for (const auto word3 : {FourComponentWord3, OneComponentWord3}) {
        auto userData = UserData();
        userData[word3] = word3 == FourComponentWord3 ? 0x31045f35u : 0x31014800u;
        userData[word3 - 1u] = 0u;
        const auto error = CompileError(device, userData);
        Require(error.empty(), "buffer store format dst_sel: a refused DST_SEL with NUM_RECORDS 0 failed: " + error);
    }
    constexpr std::array<Case, 2> accepted{{
        {OneComponentWord3, 0x31001800u, "0,0,0,X on 8_UNORM"},
        {OneComponentWord3, 0x31014404u, "X,0,0 and the reserved selector 2 on 32_UINT"},
    }};
    for (const auto& test : accepted) {
        auto userData = UserData();
        userData[test.word3] = test.value;
        const auto error = CompileError(device, userData);
        Require(error.empty(), std::string("buffer store format dst_sel: a store through DST_SEL ") + test.name + " was refused: " + error);
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (const std::uint32_t waveSize : {32u, 64u}) {
            Run(*device, waveSize);
            Check(waveSize);
        }
        CheckSettledSet();
        CheckThrows(*device);
        static_cast<void>(Compile(*device, GpuSelectedStoreCode, 32u, UserData()));
        std::puts("buffer store format dst_sel tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
