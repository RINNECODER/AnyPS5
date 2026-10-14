#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 128;
constexpr std::uint32_t Lanes = 4096;
constexpr std::uint32_t Batches = 16;
constexpr std::uint32_t Offset = 0x74;
alignas(256) std::array<std::uint32_t, Lanes * 4> Input{};
alignas(256) std::array<std::uint32_t, Lanes> Output{};

alignas(256) constexpr std::array<std::uint32_t, 20> Wave32Code{
    0x34020084, 0x8f098b08, 0x4a020209, 0x34060082, 0x8f0a8908, 0x4a06060a, 0xe0381000, 0x80000401,
    0xbf8c3f70, 0x7e1002ff, 0xdeaddead, 0x7d8a0c80, 0xbe943c6a, 0xdac80000 | Offset, 0x08000504, 0xbefe0314,
    0xbf8cc07f, 0xe0701000, 0x80010803, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 20> Wave64Code{
    0x34020084, 0x8f098b08, 0x4a020209, 0x34060082, 0x8f0a8908, 0x4a06060a, 0xe0381000, 0x80000401,
    0xbf8c3f70, 0x7e1002ff, 0xdeaddead, 0x7d8a0c80, 0xbe94246a, 0xdac80000 | Offset, 0x08000504, 0xbefe0414,
    0xbf8cc07f, 0xe0701000, 0x80010803, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void Fill(std::mt19937_64& random, std::uint32_t batch) {
    const std::uint32_t mode = batch % 4u;
    for (std::uint32_t group = 0; group < Lanes / 32u; ++group) {
        std::array<std::uint32_t, 32> order{};
        std::iota(order.begin(), order.end(), 0u);
        std::shuffle(order.begin(), order.end(), random);
        for (std::uint32_t lane = 0; lane < 32u; ++lane) {
            std::uint32_t* row = &Input[(group * 32u + lane) * 4u];
            std::uint32_t target = static_cast<std::uint32_t>(random()) & 31u;
            if (mode == 1u) target &= 3u;
            if (mode == 2u) target = order[lane];
            const std::uint32_t address = static_cast<std::uint32_t>(random()) + Offset;
            row[0] = ((address & ~0x7cu) | (target << 2u)) - Offset;
            row[1] = static_cast<std::uint32_t>(random());
            row[2] = mode == 3u || random() % 4u != 0u ? 1u : 0u;
            row[3] = 0u;
        }
    }
}

std::uint32_t Expected(std::uint32_t lane) {
    const std::uint32_t base = lane & ~31u;
    if (Input[lane * 4u + 2u] == 0u) return 0xdeaddeadu;
    std::uint32_t result = 0u;
    for (std::uint32_t source = base; source < base + 32u; ++source) {
        const std::uint32_t* row = &Input[source * 4u];
        if (row[2] != 0u && (((row[0] + Offset) >> 2u) & 31u) == (lane & 31u)) result = row[1];
    }
    return result;
}

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::uint32_t waveSize, const char* name) {
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    std::mt19937_64 random{0x7065726d75u + waveSize};
    for (std::uint32_t batch = 0; batch < Batches; ++batch) {
        Fill(random, batch);
        Output.fill(0xdeadbeefu);
        device.Dispatch(result, Lanes / Threads, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
            const std::uint32_t* row = &Input[lane * 4u];
            const std::uint32_t expected = Expected(lane);
            Require(Output[lane] == expected, std::string(name) + " batch " + std::to_string(batch) + ": lane " + std::to_string(lane) + " (address " + Hex(row[0]) + ", data " + Hex(row[1]) + ", exec " + std::to_string(row[2]) + ") is " + Hex(Output[lane]) + ", expected " + Hex(expected));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32u) {
            std::printf("skipped, the device's subgroups are narrower than a wave (%u lanes)\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        Run(*device, Wave32Code, 32u, "wave32");
        Run(*device, Wave64Code, 64u, "wave64");
        std::printf("ds permute lane tests passed (%u results)\n", 2u * Batches * Lanes);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
