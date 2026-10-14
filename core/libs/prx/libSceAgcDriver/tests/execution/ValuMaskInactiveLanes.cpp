#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Inputs = 12;
constexpr std::uint32_t Results = 48;
constexpr std::uint32_t Writers = 10;
constexpr std::uint64_t Exec = 0x00ff00018000f00full;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 222> MaskCode{
    0x1602008c, 0x160600b0, 0xe0302000, 0x80000401, 0xe0302004, 0x80000501, 0xe0302008, 0x80000601,
    0xe030200c, 0x80000701, 0xe0302010, 0x80000801, 0xe0302014, 0x80000901, 0xe0302018, 0x80000a01,
    0xe030201c, 0x80000b01, 0xe0302020, 0x80000c01, 0xe0302024, 0x80000d01, 0xe0302028, 0x80000e01,
    0xe030202c, 0x80001101, 0xbf8c3f70, 0x7e200280, 0xbeaa047e, 0xbea803ff, 0x8000f00f, 0xbea903ff,
    0x00ff0001, 0xd5010012, 0x00a100c1, 0x7e260281, 0xbe8804c1, 0xbe8a04c1, 0xbe8c04c1, 0xbe8e04c1,
    0xbe9004c1, 0xbe9204c1, 0x87fe282a, 0xd70f0814, 0x00020b04, 0xd7100a15, 0x00020b04, 0xd7190c16,
    0x00020b04, 0xbeea0480, 0x502e0b04, 0xbe94046a, 0xbeea0480, 0x52300b04, 0xbe96046a, 0xbeea0480,
    0x54320b04, 0xbe98046a, 0xd5760e1a, 0x04220f06, 0xd577101c, 0x0432170a, 0xbeea04c1, 0xd56d6a1e,
    0x0439e4f2, 0xbe9a046a, 0xd56e1220, 0x0441e4f2, 0xbeea0480, 0x50442712, 0xbe9c0380, 0xbf870001,
    0xbe9c0381, 0xbefe042a, 0x7e500208, 0x7e520209, 0xbe9e1008, 0x7e54021e, 0xd501002b, 0x00210280,
    0x7e58020a, 0x7e5a020b, 0xbe9e100a, 0x7e5c021e, 0xd501002f, 0x00290280, 0x7e60020c, 0x7e62020d,
    0xbe9e100c, 0x7e64021e, 0xd5010033, 0x00310280, 0x7e680214, 0x7e6a0215, 0xbe9e1014, 0x7e6c021e,
    0xd5010037, 0x00510280, 0x7e700216, 0x7e720217, 0xbe9e1016, 0x7e74021e, 0xd501003b, 0x00590280,
    0x7e780218, 0x7e7a0219, 0xbe9e1018, 0x7e7c021e, 0xd501003f, 0x00610280, 0x7e80020e, 0x7e82020f,
    0xbe9e100e, 0x7e84021e, 0xd5010043, 0x00390280, 0x7e880210, 0x7e8a0211, 0xbe9e1010, 0x7e8c021e,
    0xd5010047, 0x00410280, 0x7e90021a, 0x7e92021b, 0xbe9e101a, 0x7e94021e, 0xd501004b, 0x00690280,
    0x7e980212, 0x7e9a0213, 0xbe9e1012, 0x7e9c021e, 0xd501004f, 0x00490280, 0xd5282c50, 0x00210080,
    0x7ea2021c, 0xe0702000, 0x80012803, 0xe0702004, 0x80012903, 0xe0702008, 0x80012a03, 0xe070200c,
    0x80012b03, 0xe0702010, 0x80012c03, 0xe0702014, 0x80012d03, 0xe0702018, 0x80012e03, 0xe070201c,
    0x80012f03, 0xe0702020, 0x80013003, 0xe0702024, 0x80013103, 0xe0702028, 0x80013203, 0xe070202c,
    0x80013303, 0xe0702030, 0x80013403, 0xe0702034, 0x80013503, 0xe0702038, 0x80013603, 0xe070203c,
    0x80013703, 0xe0702040, 0x80013803, 0xe0702044, 0x80013903, 0xe0702048, 0x80013a03, 0xe070204c,
    0x80013b03, 0xe0702050, 0x80013c03, 0xe0702054, 0x80013d03, 0xe0702058, 0x80013e03, 0xe070205c,
    0x80013f03, 0xe0702060, 0x80014003, 0xe0702064, 0x80014103, 0xe0702068, 0x80014203, 0xe070206c,
    0x80014303, 0xe0702070, 0x80014403, 0xe0702074, 0x80014503, 0xe0702078, 0x80014603, 0xe070207c,
    0x80014703, 0xe0702080, 0x80014803, 0xe0702084, 0x80014903, 0xe0702088, 0x80014a03, 0xe070208c,
    0x80014b03, 0xe0702090, 0x80014c03, 0xe0702094, 0x80014d03, 0xe0702098, 0x80014e03, 0xe070209c,
    0x80014f03, 0xe07020a0, 0x80015003, 0xe07020a4, 0x80015103, 0xbf810000,
};

constexpr std::array<const char*, Writers> Names{
    "v_add_co_u32 into an sgpr pair", "v_sub_co_u32 into an sgpr pair", "v_subrev_co_u32 into an sgpr pair", "v_add_co_ci_u32 into vcc",
    "v_sub_co_ci_u32 into vcc", "v_subrev_co_ci_u32 into vcc", "v_mad_u64_u32 into an sgpr pair", "v_mad_i64_i32 into an sgpr pair",
    "v_div_scale_f32 into vcc", "v_div_scale_f64 into an sgpr pair",
};

constexpr std::array<std::uint64_t, Writers> Before{~0ull, ~0ull, ~0ull, 0ull, 0ull, 0ull, ~0ull, ~0ull, ~0ull, ~0ull};

void FillInput() {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        auto* words = &Input[tid * Inputs];
        const bool raises = tid % 3u != 0u;
        words[0] = raises ? 1u + (tid & 7u) : 0x10u + tid;
        words[1] = raises ? 0xffffffffu - (tid & 3u) : tid;
        words[2] = raises ? 0xffffffffu : tid + 1u;
        words[3] = raises ? 0xffffffffu : 3u;
        words[4] = raises ? 0xffffffffu : tid;
        words[5] = raises ? 0xffffffffu : 0u;
        words[6] = raises ? 0x80000000u : tid;
        words[7] = raises ? 0x7fffffffu : 5u;
        words[8] = raises ? 0u : 7u;
        words[9] = raises ? 0x80000000u : 0u;
        words[10] = raises ? 0x71800000u : 0x40000000u;
        words[11] = raises ? 0x71f00000u : 0x40000000u;
    }
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (4u << 16u), count, 0x11016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t waveSize, std::uint32_t threads) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size()));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(MaskCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{threads, 1, 1}, 0, {false, false, false}, false, 1};
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
}

bool MadI64Carry(std::uint32_t lhs, std::uint32_t rhs, std::uint64_t addend) {
    const std::int64_t product = static_cast<std::int64_t>(static_cast<std::int32_t>(lhs)) * static_cast<std::int32_t>(rhs);
    const auto sum = static_cast<std::int64_t>(static_cast<std::uint64_t>(product) + addend);
    const bool overflow = (product < 0) == (static_cast<std::int64_t>(addend) < 0) && (sum < 0) != (product < 0);
    return overflow ? product < 0 : sum < 0;
}

bool Raised(std::uint32_t writer, std::uint32_t tid) {
    const auto* words = &Input[tid * Inputs];
    const std::uint32_t a = words[0];
    const std::uint32_t b = words[1];
    switch (writer) {
        case 0:
        case 3: return static_cast<std::uint64_t>(a) + b > 0xffffffffu;
        case 1:
        case 4: return b > a;
        case 2:
        case 5: return a > b;
        case 6: {
            const std::uint64_t product = static_cast<std::uint64_t>(words[2]) * words[3];
            return product + (static_cast<std::uint64_t>(words[5]) << 32u | words[4]) < product;
        }
        case 7: return MadI64Carry(words[6], words[7], static_cast<std::uint64_t>(words[9]) << 32u | words[8]);
        case 8: return (words[10] >> 23u & 0xffu) >= 127u + 96u;
        default: return (words[11] >> 20u & 0x7ffu) >= 1023u + 768u;
    }
}

void Check(std::uint32_t waveSize, std::uint32_t threads) {
    constexpr std::array<const char*, 4> reads{"low word", "high word", "s_bcnt1_i32_b64", "v_cndmask_b32 lane bit"};
    for (std::uint32_t base = 0; base < threads; base += waveSize) {
        const std::uint32_t lanes = std::min(waveSize, threads - base);
        const std::uint64_t exec = lanes == 64u ? Exec : Exec & ((1ull << lanes) - 1u);
        std::array<std::uint64_t, Writers> masks{};
        for (std::uint32_t writer = 0; writer < Writers; ++writer) {
            std::uint64_t written = 0;
            for (std::uint32_t lane = 0; lane < lanes; ++lane) {
                if ((exec >> lane & 1u) != 0u && Raised(writer, base + lane)) written |= 1ull << lane;
            }
            masks[writer] = waveSize == 64u ? written : (Before[writer] & 0xffffffff00000000ull) | written;
        }
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            const std::uint32_t tid = base + lane;
            const std::string thread = "wave" + std::to_string(waveSize) + " thread " + std::to_string(tid);
            const auto* values = &Output[tid * Results];
            for (std::uint32_t writer = 0; writer < Writers; ++writer) {
                const std::uint64_t mask = masks[writer];
                const std::array<std::uint32_t, 4> expected{
                    static_cast<std::uint32_t>(mask), static_cast<std::uint32_t>(mask >> 32u), static_cast<std::uint32_t>(std::popcount(mask)), static_cast<std::uint32_t>(mask >> lane & 1u),
                };
                for (std::uint32_t read = 0; read < expected.size(); ++read) {
                    const std::uint32_t value = values[writer * 4u + read];
                    Require(value == expected[read], thread + " " + Names[writer] + " " + reads[read] + " is " + Hex(value) + ", expected " + Hex(expected[read]));
                }
            }
            const std::uint32_t carryIn = values[Writers * 4u];
            const auto expectedCarryIn = static_cast<std::uint32_t>(masks[0] >> lane & 1u);
            Require(carryIn == expectedCarryIn, thread + " v_add_co_ci_u32 carry-in from the v_add_co_u32 mask is " + Hex(carryIn) + ", expected " + Hex(expectedCarryIn));
            const std::uint32_t vccz = values[Writers * 4u + 1u];
            Require(vccz == 1u, thread + " vccz after a carry only in inactive lanes is " + Hex(vccz) + ", expected 0x00000001");
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::uint32_t subgroupSize = device->Target().subgroupSize;
        const std::uint32_t threads = subgroupSize >= 32u ? Threads : subgroupSize;
        if (threads < Threads) std::printf("running %u threads, one subgroup of size %u\n", threads, subgroupSize);
        FillInput();
        for (const std::uint32_t waveSize : {32u, 64u}) {
            Run(*device, waveSize, threads);
            Check(waveSize, threads);
        }
        std::puts("valu mask inactive lanes tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
