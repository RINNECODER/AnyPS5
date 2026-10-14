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
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Inputs = 8;
constexpr std::uint32_t Results = 64;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 260> Code{
    0x34020085, 0xe0381000, 0x80000201, 0xe0381010, 0x80000601, 0x34780084, 0xbf8c3f70, 0x4a7a7880,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb000002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x0a00003d, 0x4a7a78ff, 0x00000200, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb040002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x0c00003d, 0x4a7a78ff, 0x00000400,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb080002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x0e00003d, 0x4a7a78ff, 0x00000600, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb0c0002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x1000003d, 0x4a7a78ff, 0x00000800,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb100002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x1200003d, 0x4a7a78ff, 0x00000a00, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb140002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x1400003d, 0x4a7a78ff, 0x00000c00,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb180002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x1600003d, 0x4a7a78ff, 0x00000e00, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb1c0002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x1800003d, 0x4a7a78ff, 0x00001000,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb200002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x1a00003d, 0x4a7a78ff, 0x00001200, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb240002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x1c00003d, 0x4a7a78ff, 0x00001400,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb280002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x1e00003d, 0x4a7a78ff, 0x00001600, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb2c0002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x2000003d, 0x4a7a78ff, 0x00001800,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb340002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x2200003d, 0x4a7a78ff, 0x00001a00, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb480002, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x2400003d, 0x4a7a78ff, 0x00001c00,
    0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d, 0xbf8cc07f, 0xdb4c0002, 0x0000003d, 0xbf8cc07f,
    0xd9d80000, 0x2600003d, 0x4a7a78ff, 0x00001e00, 0xd8340000, 0x0000063d, 0xd8340004, 0x0000073d,
    0xbf8cc07f, 0xda480001, 0x0000003d, 0xbf8cc07f, 0xd8d80000, 0x2800003d, 0x4a7a78ff, 0x00002000,
    0xd8340000, 0x0000063d, 0xd8340004, 0x0000073d, 0xbf8cc07f, 0xda4c0001, 0x0000003d, 0xbf8cc07f,
    0xd8d80000, 0x2900003d, 0x4a7a78ff, 0x00002200, 0xd9340000, 0x0000023d, 0xd9340008, 0x0000043d,
    0xbf8cc07f, 0xdb340001, 0x0000003d, 0xbf8cc07f, 0xd9d80000, 0x2a00003d, 0x347c0085, 0x4a7a7cff,
    0x00002404, 0xd8340000, 0x0000023d, 0xd8340004, 0x0000033d, 0xd934000c, 0x0000043d, 0xbf8cc07f,
    0xdb340003, 0x0000003d, 0xbf8cc07f, 0xd8d80000, 0x2c00003d, 0xd8d80004, 0x2d00003d, 0x34020088,
    0xbf8cc07f, 0xe0781000, 0x80010a01, 0xe0781010, 0x80010e01, 0xe0781020, 0x80011201, 0xe0781030,
    0x80011601, 0xe0781040, 0x80011a01, 0xe0781050, 0x80011e01, 0xe0781060, 0x80012201, 0xe0781070,
    0x80012601, 0xe0781080, 0x80012a01, 0xbf810000,
};

constexpr std::array<std::uint64_t, 16> Wide{
    0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800fffffffffffffull, 0x3ff0000000000000ull, 0xbff0000000000000ull,
    0x7ff0000000000000ull, 0xfff0000000000000ull, 0x7ff8000000000000ull, 0xfff8000000000001ull, 0x7ff0000000000001ull, 0x7fefffffffffffffull,
    0x00000000ffffffffull, 0xffffffffffffffffull, 0x0000000100000000ull, 0x7fffffffffffffffull,
};

constexpr std::array<std::uint32_t, 8> Narrow{0x00000000u, 0x80000000u, 0x00000001u, 0x3f800000u, 0xc0200000u, 0x7fc00000u, 0x7f800001u, 0xff800000u};

std::uint64_t WideA(std::uint32_t tid) {
    return tid < Wide.size() ? Wide[tid] : (tid + 1u) * 0x9e3779b97f4a7c15ull;
}

std::uint64_t WideB(std::uint32_t tid) {
    if (tid < Wide.size()) return Wide[(tid * 5u + 3u) % Wide.size()];
    if (tid % 4u == 0u) return WideA(tid);
    if (tid % 4u == 1u) return WideA(tid) + 1u;
    return (tid + 7u) * 0xbf58476d1ce4e5b9ull;
}

void Fill(std::uint32_t tid, std::uint32_t* words) {
    const auto a = WideA(tid);
    const auto b = WideB(tid);
    words[0] = static_cast<std::uint32_t>(a);
    words[1] = static_cast<std::uint32_t>(a >> 32u);
    words[2] = static_cast<std::uint32_t>(b);
    words[3] = static_cast<std::uint32_t>(b >> 32u);
    words[4] = Narrow[tid % Narrow.size()];
    words[5] = Narrow[(tid / Narrow.size() + tid * 3u) % Narrow.size()];
    words[6] = 0u;
    words[7] = 0u;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}

struct FloatFormat {
    std::uint64_t sign;
    std::uint64_t exponent;
    std::uint64_t mantissa;
    std::uint64_t quiet;
};

constexpr FloatFormat F32{0x80000000ull, 0x7f800000ull, 0x007fffffull, 0x00400000ull};
constexpr FloatFormat F64{0x8000000000000000ull, 0x7ff0000000000000ull, 0x000fffffffffffffull, 0x0008000000000000ull};

double Value(std::uint64_t bits, const FloatFormat& format) {
    return format.sign == F32.sign ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(bits))) : std::bit_cast<double>(bits);
}

std::uint64_t MinMax(std::uint64_t old, std::uint64_t data, bool minimum, bool flush, const FloatFormat& format) {
    const auto flushed = [&](std::uint64_t bits) { return flush && (bits & format.exponent) == 0u && (bits & format.mantissa) != 0u ? bits & format.sign : bits; };
    const auto nan = [&](std::uint64_t bits) { return (bits & format.exponent) == format.exponent && (bits & format.mantissa) != 0u; };
    const auto signalling = [&](std::uint64_t bits) { return nan(bits) && (bits & format.quiet) == 0u; };
    old = flushed(old);
    data = flushed(data);
    if (signalling(data)) return data | format.quiet;
    if (signalling(old)) return old | format.quiet;
    if (nan(old)) return data;
    if (nan(data)) return old;
    const double o = Value(old, format);
    const double d = Value(data, format);
    const bool below = d < o || (d == 0.0 && o == 0.0 && (data & format.sign) != 0u && (old & format.sign) == 0u);
    return minimum ? (below ? data : old) : (below ? old : data);
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device) {
    std::vector<std::uint32_t> userData(8, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(Code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 10240u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(Code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

void Check() {
    constexpr std::array<const char*, 15> names{
        "ds_add_src2_u64", "ds_sub_src2_u64", "ds_rsub_src2_u64", "ds_inc_src2_u64", "ds_dec_src2_u64", "ds_min_src2_i64", "ds_max_src2_i64", "ds_min_src2_u64",
        "ds_max_src2_u64", "ds_and_src2_b64", "ds_or_src2_b64", "ds_xor_src2_b64", "ds_write_src2_b64", "ds_min_src2_f64", "ds_max_src2_f64",
    };
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto a = WideA(tid);
        const auto b = WideB(tid);
        const auto sa = static_cast<std::int64_t>(a);
        const auto sb = static_cast<std::int64_t>(b);
        const std::array<std::uint64_t, 15> expected{
            a + b, a - b, b - a, a >= b ? 0u : a + 1u, (a == 0u || a > b) ? b : a - 1u,
            static_cast<std::uint64_t>(std::min(sa, sb)), static_cast<std::uint64_t>(std::max(sa, sb)), std::min(a, b), std::max(a, b),
            a & b, a | b, a ^ b, b, MinMax(a, b, true, false, F64), MinMax(a, b, false, false, F64),
        };
        const std::uint32_t* out = &Output[tid * Results];
        const auto wide = [&](std::uint32_t index) { return out[index] | (static_cast<std::uint64_t>(out[index + 1u]) << 32u); };
        for (std::uint32_t op = 0; op < expected.size(); ++op) {
            Require(wide(op * 2u) == expected[op], "lds src2 b64: lane " + std::to_string(tid) + " " + names[op] + " is " + Hex(wide(op * 2u)) + ", expected " + Hex(expected[op]));
        }
        const std::uint32_t* in = &Input[tid * Inputs];
        const std::array<std::uint64_t, 2> narrow{MinMax(in[4], in[5], true, true, F32), MinMax(in[4], in[5], false, true, F32)};
        Require(out[30] == narrow[0], "lds src2: lane " + std::to_string(tid) + " ds_min_src2_f32 is " + Hex(out[30]) + ", expected " + Hex(narrow[0]));
        Require(out[31] == narrow[1], "lds src2: lane " + std::to_string(tid) + " ds_max_src2_f32 is " + Hex(out[31]) + ", expected " + Hex(narrow[1]));
        Require(wide(32u) == 0u, "lds src2 b64: lane " + std::to_string(tid) + " a B that is not 8-byte aligned did not read as 0: " + Hex(wide(32u)));
        Require(wide(34u) == a, "lds src2 b64: lane " + std::to_string(tid) + " an A that is not 8-byte aligned was written: " + Hex(wide(34u)));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (std::uint32_t tid = 0; tid < Threads; ++tid) Fill(tid, &Input[tid * Inputs]);
        Output.fill(0xdeadbeefu);
        const auto result = Compile(*device);
        device->Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(Code.data()));
        device->WaitIdle();
        Check();
        std::puts("lds src2 b64 and f32 tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
