// COMPUTE_TG_SIZE carries the wave index of the thread in bits 11:6 (upstream PR #5214). The upstream
// execution test stores through a global address; this variant stores the extracted field through a
// buffer descriptor so it runs on the Metal bridge. Runs on the shimmed Metal device.
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_VULKANTESTDEVICE_HPP
#include "MetalTestDevice.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {
using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 256;
alignas(256) std::array<std::uint32_t, 64> Dummy{};
alignas(256) std::array<std::uint32_t, Threads> Output{};

// s_bfe_u32 s10, s9, 0x00060006 ; v_mov_b32 v10, s10 ; buffer_store_dword v10, v0 idxen, s[4:7]
alignas(256) constexpr std::array<std::uint32_t, 6> Code{
    0x938aff09, 0x00060006, 0x7e14020a, 0xe0702000, 0x80010a00, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x11016facu};
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto dummy = BufferDescriptor(Dummy.data(), static_cast<std::uint32_t>(Dummy.size()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(dummy.begin(), dummy.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {true, false, false}, true, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    for (std::uint32_t thread = 0; thread < Threads; ++thread) {
        Require(Output[thread] == thread / waveSize, "wave" + std::to_string(waveSize) + " thread " + std::to_string(thread) + " read wave " + std::to_string(Output[thread]) +
            " from TG_SIZE[11:6], expected " + std::to_string(thread / waveSize));
    }
    std::printf("wave%u: TG_SIZE[11:6] holds the wave index for all %u threads\n", waveSize, Threads);
}
}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        Run(*device, 64);
        Run(*device, 32);
        std::puts("TG_SIZE wave index tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
