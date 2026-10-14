#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t MaxThreads = 64;
constexpr std::uint32_t Inputs = 8;
constexpr std::uint32_t WordsPerLane = 16;
constexpr std::uint32_t AndSaveexecB32 = 0xbe943c6au;
constexpr std::uint32_t AndSaveexecB64 = 0xbe94246au;
constexpr std::uint32_t MovExecLo = 0xbefe0314u;
constexpr std::uint32_t MovExec = 0xbefe0414u;
alignas(256) std::array<std::uint32_t, MaxThreads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, MaxThreads * WordsPerLane> Output{};

alignas(256) constexpr std::array<std::uint32_t, 181> Code{
    0x34020085, 0xe0381000, 0x80000201, 0xe0381010, 0x80000601, 0x34500084, 0x34520086, 0xbf8c3f70,
    0x4a545080, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340000, 0x00002b29,
    0x4a545081, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340004, 0x00002b29,
    0x4a545082, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340008, 0x00002b29,
    0x4a545083, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd834000c, 0x00002b29,
    0x4a545084, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340010, 0x00002b29,
    0x4a545085, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340014, 0x00002b29,
    0x4a545086, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340018, 0x00002b29,
    0x4a545087, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd834001c, 0x00002b29,
    0x4a545088, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340020, 0x00002b29,
    0x4a545089, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340024, 0x00002b29,
    0x4a54508a, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340028, 0x00002b29,
    0x4a54508b, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd834002c, 0x00002b29,
    0x4a54508c, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340030, 0x00002b29,
    0x4a54508d, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340034, 0x00002b29,
    0x4a54508e, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd8340038, 0x00002b29,
    0x4a54508f, 0xd569002b, 0x0001ff2a, 0x9e3779b1, 0x3a5656ff, 0x00012345, 0xd834003c, 0x00002b29,
    0xbf8cc07f, 0xbf8a0000, 0x7d8a0680, 0xbe943c6a, 0xda007fff, 0x00000002, 0xbf8cc07f, 0xbefe0314,
    0x7d8a0a80, 0xbe943c6a, 0xda000001, 0x00000004, 0xbf8cc07f, 0xbefe0314, 0x7d8a0e80, 0xbe943c6a,
    0xdb007ffe, 0x00000006, 0xbf8cc07f, 0xbefe0314, 0x7d8a1280, 0xbe943c6a, 0xda347fff, 0x00000008,
    0xbf8cc07f, 0xbefe0314, 0xbf8a0000, 0xdbfc0000, 0x0a000029, 0xdbfc0010, 0x0e000029, 0xdbfc0020,
    0x12000029, 0xdbfc0030, 0x16000029, 0xbf8cc07f, 0xe0781000, 0x80010a29, 0xe0781010, 0x80010e29,
    0xe0781020, 0x80011229, 0xe0781030, 0x80011629, 0xbf810000,
};

alignas(256) std::array<std::uint32_t, Code.size()> Code64{};

enum Field : std::uint32_t { ChainAddress, ChainActive, NextAddress, NextActive, WideAddress, WideActive, ReversedAddress, ReversedActive };

void Fill(std::uint32_t threads, std::uint32_t first) {
    Input.fill(0u);
    for (std::uint32_t lane = first; lane < threads; ++lane) {
        std::uint32_t* words = &Input[lane * Inputs];
        const auto slot = lane - first;
        if (slot < 8u) {
            words[ChainAddress] = 4u * (64u + slot);
            words[ChainActive] = 1u;
            words[WideAddress] = 4u * (400u + 2u * slot);
            words[WideActive] = 1u;
        } else if (slot < 16u) {
            words[ReversedAddress] = 4u * (207u - slot);
            words[ReversedActive] = 1u;
        } else if (slot < 24u) {
            words[NextAddress] = 4u * (320u + slot - 16u);
            words[NextActive] = 1u;
        } else if (slot < 28u) {
            words[NextAddress] = 4u * 340u;
            words[NextActive] = 1u;
        }
    }
}

std::vector<std::uint32_t> Expected(std::uint32_t threads) {
    std::vector<std::uint32_t> lds(threads * WordsPerLane);
    for (std::uint32_t word = 0; word < lds.size(); ++word) lds[word] = (word * 0x9e3779b1u) ^ 0x12345u;
    const auto field = [&](std::uint32_t lane, Field name) { return Input[lane * Inputs + name]; };
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        if (field(lane, ChainActive) == 0u) continue;
        const auto a = field(lane, ChainAddress) / 4u;
        lds[a] += lds[a - 1u];
    }
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        if (field(lane, NextActive) == 0u) continue;
        const auto a = field(lane, NextAddress) / 4u;
        lds[a] += lds[a + 1u];
    }
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        if (field(lane, WideActive) == 0u) continue;
        const auto a = field(lane, WideAddress) / 4u;
        const auto sum = (lds[a] | (static_cast<std::uint64_t>(lds[a + 1u]) << 32u)) + (lds[a - 2u] | (static_cast<std::uint64_t>(lds[a - 1u]) << 32u));
        lds[a] = static_cast<std::uint32_t>(sum);
        lds[a + 1u] = static_cast<std::uint32_t>(sum >> 32u);
    }
    for (std::uint32_t lane = 0; lane < threads; ++lane) {
        if (field(lane, ReversedActive) == 0u) continue;
        const auto a = field(lane, ReversedAddress) / 4u;
        lds[a] = lds[a - 1u];
    }
    return lds;
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

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t threads, std::uint32_t hostSubgroup) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{threads, 1, 1}, threads * WordsPerLane * 4u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {threads, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    if (hostSubgroup != 0u) request.target.subgroupSize = hostSubgroup;
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    Require(hostSubgroup == 0u || result.hostSubgroupSize == hostSubgroup, "lds src2 lane order: the program was not laid out for a " + std::to_string(hostSubgroup) + "-lane host subgroup");
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t threads, std::uint32_t first, std::uint32_t hostSubgroup) {
    Fill(threads, first);
    Run(device, code, threads, hostSubgroup);
    const auto expected = Expected(threads);
    for (std::uint32_t word = 0; word < expected.size(); ++word) {
        Require(Output[word] == expected[word], "lds src2 lane order: wave" + std::to_string(threads) + " on a " + (hostSubgroup == 0u ? std::string("default") : std::to_string(hostSubgroup) + "-lane") + " host subgroup, LDS word " + std::to_string(word) + " is " + Hex(Output[word]) + ", expected " + Hex(expected[word]));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::transform(Code.begin(), Code.end(), Code64.begin(), [](std::uint32_t word) { return word == AndSaveexecB32 ? AndSaveexecB64 : word == MovExecLo ? MovExec : word; });
        Check(*device, Code, 32u, 0u, 0u);
        Check(*device, Code64, 64u, 28u, 0u);
        if (device->ComputeTarget(32u).subgroupSize == 32u) Check(*device, Code64, 64u, 28u, 32u);
        else std::puts("lds src2 lane order: wave64 on a 32-lane host subgroup skipped, the device cannot require 32-lane subgroups");
        std::puts("lds src2 lane order tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
