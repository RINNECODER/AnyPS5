#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Results = 4;
constexpr std::uint32_t Loaded = 3;
constexpr std::uint32_t RegionBytes = 64;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint32_t DataBytes = 1024;
constexpr std::uint32_t Fill = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, 4> Descriptor{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 19> Code{
    0xe0380000, 0x80000100, 0xbf8c3f70, 0x7e100501, 0x7e120502, 0x7e140503, 0x7e160504, 0xf4200304,
    0xfa000000, 0xf4240384, 0xfa000004, 0xbf8cc07f, 0x34000084, 0x7e0a020c, 0x7e0c020e, 0x7e0e020f,
    0xe07c1000, 0x80010500, 0xbf810000,
};

struct Case {
    const char* name;
    std::uint32_t baseOffset;
    std::uint32_t word3;
};

constexpr std::array<Case, 9> Cases{{
    {"aligned base", 0u, 0x31016facu},
    {"base +1", 1u, 0x31016facu},
    {"base +2", 2u, 0x31016facu},
    {"base +3", 3u, 0x31016facu},
    {"type 1", 0u, 0x71016facu},
    {"type 2", 0u, 0x91b00f2eu},
    {"type 3", 0u, 0xc0000000u},
    {"word 3 all ones", 0u, 0xffffffffu},
    {"base +3 with type 2", 3u, 0x91b00f2eu},
}};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "scalar buffer gpu descriptor: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, false, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes, std::uint32_t word3) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, word3};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::string Load(std::uint32_t dword) {
    return dword == 0u ? "s_buffer_load_dword" : "s_buffer_load_dwordx2 dword " + std::to_string(dword - 1u);
}

void Run(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize) {
    std::vector<std::uint32_t> userData(8, 0u);
    const auto descriptor = BufferDescriptor(Descriptor.data(), sizeof(Descriptor), 0x31016facu);
    const auto output = BufferDescriptor(Output.data(), sizeof(Output), 0x31016facu);
    std::copy(descriptor.begin(), descriptor.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    for (std::uint32_t index = 0; index < Cases.size(); ++index) {
        const auto& test = Cases[index];
        const auto* region = guest.Data() + index * RegionBytes;
        Descriptor = BufferDescriptor(region + test.baseOffset, RegionBytes, test.word3);
        Output.fill(Fill);
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            for (std::uint32_t dword = 0; dword < Loaded; ++dword) {
                std::uint32_t expected = 0;
                std::memcpy(&expected, region + dword * 4u, sizeof(expected));
                const auto actual = Output[tid * Results + dword];
                Require(actual == expected, "scalar buffer gpu descriptor, wave" + std::to_string(waveSize) + ", " + test.name + " (word 3 " + Hex(test.word3) + "): thread " + std::to_string(tid) + " " + Load(dword) + " is " + Hex(actual) + ", expected " + Hex(expected));
            }
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest;
        std::memset(guest.Data(), 0, BlockBytes);
        for (std::uint32_t index = 0; index < DataBytes; ++index) guest.Data()[index] = static_cast<std::uint8_t>(index * 41u + 3u + (index >> 3u));
        for (const std::uint32_t waveSize : {32u, 64u}) Run(*device, guest, waveSize);
        std::puts("scalar buffer gpu descriptor tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
