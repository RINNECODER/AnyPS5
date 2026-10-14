// Per-invocation (pixel and vertex) LDS must update without atomics or workgroup-scope barriers.
// Extracted from verifyFunctionLdsUpdates in tests/ShaderMemory.cpp (upstream PR #5160), which only
// builds with the Vulkan driver tests; this copy runs against the recompiler the Metal replay links.
#include "Recompiler.hpp"
#include <spirv/unified1/spirv.hpp>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void verifyFunctionLdsUpdates() {
    using namespace ShaderRecompiler;
    static constexpr std::array<std::uint32_t, 5> capabilities{spv::CapabilityShader, spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle, spv::CapabilityClipDistance};
    const auto compile = [](ShaderStage stage, std::uint32_t waveSize, std::span<const std::uint32_t> body) {
        std::vector<std::uint32_t> code{0x7e000280u, 0x7e020292u, 0xbefc03ffu, 0x0000ffffu};
        code.insert(code.end(), body.begin(), body.end());
        code.insert(code.end(), {0xbf8cc07fu, 0xd8d80000u, 0x02000000u, 0xbf8cc07fu, stage == ShaderStage::Fragment ? 0xf800180fu : 0xf80008cfu, 0x02020202u, 0xbf810000u});
        RecompileRequest request{};
        request.shader = {stage, 0x40000u, code, 0, {}};
        request.context.waveSize = waveSize;
        if (stage == ShaderStage::Fragment) {
            ShaderPixelStageInfo info{};
            info.wave32 = waveSize == 32u;
            info.inputAddr = PixelInputBit(PixelInput::PerspectiveCenter);
            info.hasPerspectiveCenterVgpr = true;
            info.targetOutputMode[0] = 9u;
            info.targetExportMapping.fill(0xe4u);
            request.context.pixel = info;
        } else {
            request.context.vertex = ShaderVertexStageInfo{};
        }
        request.target.vulkanVersion = 0x00401000u;
        request.target.spirvVersion = 0x00010300u;
        request.target.subgroupSize = 32u;
        request.target.supportedCapabilities = capabilities;
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        return Recompile(request);
    };
    const auto violations = [](const std::vector<std::uint32_t>& words) {
        std::map<std::uint32_t, std::uint32_t> storage;
        std::map<std::uint32_t, std::uint32_t> constants;
        std::map<std::uint32_t, std::uint32_t> types;
        for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
            const auto op = static_cast<spv::Op>(words[at] & 0xffffu);
            if (op == spv::OpTypePointer) storage[words[at + 1]] = words[at + 2];
            if (op == spv::OpConstant && (words[at] >> 16u) == 4u) constants[words[at + 2]] = words[at + 3];
            if (op == spv::OpVariable || op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain) types[words[at + 2]] = words[at + 1];
        }
        const auto workgroupScope = [&](std::uint32_t id) { return constants.count(id) != 0 && constants.at(id) == spv::ScopeWorkgroup; };
        std::size_t found = 0;
        for (std::size_t at = 5; at < words.size() && (words[at] >> 16u) != 0; at += words[at] >> 16u) {
            const auto op = static_cast<spv::Op>(words[at] & 0xffffu);
            if (op == spv::OpMemoryBarrier && workgroupScope(words[at + 1])) ++found;
            if ((op < spv::OpAtomicLoad || op > spv::OpAtomicXor) && op != spv::OpAtomicFMinEXT && op != spv::OpAtomicFMaxEXT && op != spv::OpAtomicFAddEXT) continue;
            const auto pointer = words[at + (op == spv::OpAtomicStore ? 1u : 3u)];
            const auto type = types.find(pointer);
            if (type != types.end() && storage.count(type->second) != 0 && storage.at(type->second) == spv::StorageClassFunction) ++found;
            if (workgroupScope(words[at + (op == spv::OpAtomicStore ? 2u : 4u)])) ++found;
        }
        return found;
    };
    using Program = std::pair<const char*, std::array<std::uint32_t, 2>>;
    static constexpr std::array<Program, 8> programs{{
        {"ds_write_b8", {0xd8780000u, 0x00000100u}},
        {"ds_write_b16", {0xd87c0000u, 0x00000100u}},
        {"ds_write_b8_d16_hi", {0xda800000u, 0x00000100u}},
        {"ds_write_b16_d16_hi", {0xda840000u, 0x00000100u}},
        {"ds_add_u32", {0xd8000000u, 0x00000100u}},
        {"ds_inc_rtn_u32", {0xd88c0000u, 0x03000100u}},
        {"ds_mskor_b32", {0xd8300000u, 0x00010100u}},
        {"ds_append", {0xd8f80000u, 0x03000000u}},
    }};
    for (const auto& [name, body] : programs) {
        for (const auto stage : {ShaderStage::Fragment, ShaderStage::Vertex}) {
            for (const auto waveSize : {64u, 32u}) {
                const auto what = std::string("function LDS: ") + name + (stage == ShaderStage::Fragment ? " in a pixel" : " in a vertex") + " wave" + std::to_string(waveSize) + " program";
                const auto result = compile(stage, waveSize, body);
                require(!result.spirv.empty(), (what + " did not compile").c_str());
                require(violations(result.spirv.Words()) == 0, (what + " uses an atomic on a Function pointer or a Workgroup memory scope").c_str());
            }
        }
    }
}
}

int main() {
    try {
        verifyFunctionLdsUpdates();
        std::puts("function LDS updates passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
