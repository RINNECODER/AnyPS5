#include "BdaAbi.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <set>
#include <span>
#include <spirv/unified1/spirv.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if ANYPS5_ENABLE_SPIRV_TOOLS
#include <spirv-tools/libspirv.hpp>
#endif

namespace {

using namespace ShaderRecompiler;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Record = 16;
constexpr std::uint32_t Forms = 4;

void Check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::array<std::uint32_t, 82> MakeCode() {
    std::array<std::uint32_t, 82> code{};
    std::size_t at = 0;
    code[at++] = 0x34020086u;
    for (std::uint32_t dword = 0; dword < Record; ++dword) {
        code[at++] = 0xe0301000u | (dword * 4u);
        code[at++] = 0x80000001u | ((4u + dword) << 8u);
    }
    code[at++] = 0xbf8c3f70u;
    code[at++] = 0xf19c9f01u;
    code[at++] = 0x00031405u;
    code[at++] = 0xf1989f07u;
    code[at++] = 0x00021804u;
    code[at++] = 0x0a090807u;
    code[at++] = 0x0e0d0c0bu;
    code[at++] = 0x0000100fu;
    code[at++] = 0xf19c9f05u;
    code[at++] = 0x40031c05u;
    code[at++] = 0x09080706u;
    code[at++] = 0x1312110au;
    code[at++] = 0xf1989f05u;
    code[at++] = 0x40022004u;
    code[at++] = 0x0a090807u;
    code[at++] = 0x00131211u;
    for (std::uint32_t dword = 0; dword < Record; ++dword) {
        code[at++] = 0xe0701000u | (dword * 4u);
        code[at++] = 0x80010001u | ((20u + dword) << 8u);
    }
    code[at++] = 0xbf810000u;
    return code;
}

alignas(256) constexpr std::array<std::uint32_t, 82> Code = MakeCode();
alignas(256) std::array<std::uint32_t, Threads * Record> Input{};
alignas(256) std::array<std::uint32_t, Threads * Record> Output{};

SpirvTarget Target(std::uint32_t subgroupSize) {
    static constexpr std::array<std::uint32_t, 3> capabilities{spv::CapabilityInt64, spv::CapabilityPhysicalStorageBufferAddresses, spv::CapabilityStorageBuffer8BitAccess};
    static constexpr std::array<std::string_view, 4> extensions{"SPV_EXT_descriptor_indexing", "SPV_KHR_8bit_storage", "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer"};
    return {0x00401000u, 0x00010300u, subgroupSize, BdaAbi::Version, capabilities, extensions, false, {1024, 1024, 64}, 1024, 32768, {}, {}};
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

struct Module {
    std::size_t words = 0;
    std::set<std::uint32_t> functions;
    std::size_t calls = 0;
    bool dontInline = true;
};

std::string Name(const std::vector<std::uint32_t>& words, std::size_t offset, std::uint32_t count) {
    std::string name;
    for (std::uint32_t i = 2; i < count; ++i) {
        for (std::uint32_t byte = 0; byte < 4u; ++byte) {
            const char c = static_cast<char>((words[offset + i] >> (byte * 8u)) & 0xffu);
            if (c == '\0') return name;
            name.push_back(c);
        }
    }
    return name;
}

Module Inspect(const RecompileResult& result, const std::string& label) {
    const auto& words = result.spirv.Words();
    Check(words.size() >= 5 && words[0] == spv::MagicNumber, label + ": missing SPIR-V module");
    Module module;
    module.words = words.size();
    std::set<std::uint32_t> named;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> spv::WordCountShift;
        const auto opcode = words[offset] & spv::OpCodeMask;
        Check(count != 0 && count <= words.size() - offset, label + ": malformed SPIR-V instruction");
        if (opcode == spv::OpName && count >= 3 && Name(words, offset, count) == "image_bvh_intersect_ray") named.insert(words[offset + 1]);
        if (opcode == spv::OpFunction && count == 5 && named.contains(words[offset + 2])) {
            module.functions.insert(words[offset + 2]);
            module.dontInline = module.dontInline && (words[offset + 3] & spv::FunctionControlDontInlineMask) != 0u;
        }
        if (opcode == spv::OpFunctionCall && count >= 4 && named.contains(words[offset + 3])) ++module.calls;
        offset += count;
    }
#if ANYPS5_ENABLE_SPIRV_TOOLS
    std::string diagnostics;
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    validator.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t&, const char* message) {
        if (message != nullptr) diagnostics += std::string(message) + '\n';
    });
    Check(validator.Validate(words), label + ": emitted invalid SPIR-V:\n" + diagnostics);
#endif
    return module;
}

Module Compile(std::uint32_t waveSize, std::uint32_t subgroupSize) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    const std::array<std::uint32_t, 4> bvh{0x100u, 0x80000000u, 6u, 0u};
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    std::copy(bvh.begin(), bvh.end(), userData.begin() + 8);
    std::copy(bvh.begin(), bvh.end(), userData.begin() + 12);
    const std::span<const std::uint32_t> code(Code);
    const std::array<MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1, {}};
    RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory, std::nullopt},
        Target(subgroupSize), {0, 0, 0, 128}, std::nullopt, false,
    };
    return Inspect(Recompile(request), "wave" + std::to_string(waveSize) + " on " + std::to_string(subgroupSize) + " lanes");
}

void SharedFunctions() {
    const auto wave32 = Compile(32, 32);
    Check(wave32.functions.size() == Forms, "wave32: expected one image_bvh_intersect_ray function per variant, found " + std::to_string(wave32.functions.size()));
    Check(wave32.dontInline, "wave32: an image_bvh_intersect_ray function may be inlined");
    Check(wave32.calls == Forms, "wave32: expected one call per instruction, found " + std::to_string(wave32.calls));
    const auto wave64 = Compile(64, 32);
    Check(wave64.functions.size() == Forms, "wave64 on 32 lanes: expected one image_bvh_intersect_ray function per variant, found " + std::to_string(wave64.functions.size()));
    Check(wave64.dontInline, "wave64 on 32 lanes: an image_bvh_intersect_ray function may be inlined");
    Check(wave64.calls == 2u * Forms, "wave64 on 32 lanes: expected one call per instruction and lane half, found " + std::to_string(wave64.calls));
    Check(wave64.words < wave32.words + wave32.words / 2u, "wave64 on 32 lanes: the module is " + std::to_string(wave64.words) + " words against " + std::to_string(wave32.words) + " for wave32, so the query is still emitted per lane half");
    std::printf("wave32 %zu words, wave64 on 32 lanes %zu words\n", wave32.words, wave64.words);
}

}

int main() {
    try {
        if (RayTracingStrict() || RayTracingMiss()) {
            std::puts("skipped, APS5_RAYTRACING replaces the node test");
            return 0;
        }
        if (std::getenv("APS5_BDA_BYTE_READS") != nullptr) {
            std::puts("skipped, APS5_BDA_BYTE_READS keeps the query inline");
            return 0;
        }
        SharedFunctions();
        std::puts("image_bvh_intersect_ray shared function tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
