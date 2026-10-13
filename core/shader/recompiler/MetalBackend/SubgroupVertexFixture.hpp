#pragma once
// Synthetic vertex shader for the Metal subgroup-lane contract (no guest game data). It has the shape the
// recompiler emits for guest lane work in a vertex stage: a SubgroupLocalInvocationId input, a shuffle by
// that lane, a ballot and its lowest set lane. Per vertex it stores
// uint4(lane, shuffle(vertexIndex, lane), ballot.x, findLSB(ballot)) at out[vertexIndex] (set 0, binding 0).
#include "MetalShaderBridge.hpp"
#if __has_include(<spirv/unified1/spirv.hpp>)
#include <spirv/unified1/spirv.hpp>
#else
#include <spirv.hpp>
#endif
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace ShaderRecompiler::MetalBackend::Fixtures {

inline RecompileResult SubgroupLaneVertex(std::uint32_t hostSubgroupSize = 32) {
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x00010300u, 0u, 48u, 0u};
    const auto emit = [&](spv::Op op, std::initializer_list<std::uint32_t> args) {
        words.push_back((static_cast<std::uint32_t>(args.size() + 1) << 16u) | op);
        words.insert(words.end(), args);
    };
    for (const auto capability : {spv::CapabilityShader, spv::CapabilityGroupNonUniform, spv::CapabilityGroupNonUniformBallot,
                                  spv::CapabilityGroupNonUniformShuffle})
        emit(spv::OpCapability, {capability});
    emit(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit(spv::OpEntryPoint, {spv::ExecutionModelVertex, 30, 0x6e69616du, 0, 9, 10, 12});
    emit(spv::OpDecorate, {9, spv::DecorationBuiltIn, spv::BuiltInSubgroupLocalInvocationId});
    emit(spv::OpDecorate, {10, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
    emit(spv::OpDecorate, {12, spv::DecorationBuiltIn, spv::BuiltInPosition});
    emit(spv::OpDecorate, {13, spv::DecorationArrayStride, 16});
    emit(spv::OpDecorate, {14, spv::DecorationBlock});
    emit(spv::OpMemberDecorate, {14, 0, spv::DecorationOffset, 0});
    emit(spv::OpDecorate, {16, spv::DecorationDescriptorSet, 0});
    emit(spv::OpDecorate, {16, spv::DecorationBinding, 0});
    emit(spv::OpTypeVoid, {1});
    emit(spv::OpTypeFunction, {2, 1});
    emit(spv::OpTypeInt, {3, 32, 0});
    emit(spv::OpTypeBool, {4});
    emit(spv::OpTypeVector, {5, 3, 4});
    emit(spv::OpTypeFloat, {6, 32});
    emit(spv::OpTypeVector, {7, 6, 4});
    emit(spv::OpTypePointer, {8, spv::StorageClassInput, 3});
    emit(spv::OpTypePointer, {11, spv::StorageClassOutput, 7});
    emit(spv::OpTypeRuntimeArray, {13, 5});
    emit(spv::OpTypeStruct, {14, 13});
    emit(spv::OpTypePointer, {15, spv::StorageClassStorageBuffer, 14});
    emit(spv::OpTypePointer, {17, spv::StorageClassStorageBuffer, 5});
    emit(spv::OpConstant, {3, 18, 0});
    emit(spv::OpConstant, {3, 19, spv::ScopeSubgroup});
    emit(spv::OpConstantTrue, {4, 20});
    emit(spv::OpConstant, {6, 21, 0});
    emit(spv::OpConstant, {6, 22, 0x3f800000u});
    emit(spv::OpConstantComposite, {7, 23, 21, 21, 21, 22});
    emit(spv::OpVariable, {8, 9, spv::StorageClassInput});
    emit(spv::OpVariable, {8, 10, spv::StorageClassInput});
    emit(spv::OpVariable, {11, 12, spv::StorageClassOutput});
    emit(spv::OpVariable, {15, 16, spv::StorageClassStorageBuffer});
    emit(spv::OpFunction, {1, 30, spv::FunctionControlMaskNone, 2});
    emit(spv::OpLabel, {31});
    emit(spv::OpLoad, {3, 32, 9});
    emit(spv::OpLoad, {3, 33, 10});
    emit(spv::OpGroupNonUniformShuffle, {3, 34, 19, 33, 32});
    emit(spv::OpGroupNonUniformBallot, {5, 35, 19, 20});
    emit(spv::OpGroupNonUniformBallotFindLSB, {3, 36, 19, 35});
    emit(spv::OpCompositeExtract, {3, 37, 35, 0});
    emit(spv::OpCompositeConstruct, {5, 38, 32, 34, 37, 36});
    emit(spv::OpAccessChain, {17, 39, 16, 18, 33});
    emit(spv::OpStore, {39, 38});
    emit(spv::OpStore, {12, 23});
    emit(spv::OpReturn, {});
    emit(spv::OpFunctionEnd, {});
    RecompileResult guest;
    guest.spirv = std::move(words);
    DescriptorBinding binding{};
    binding.kind = DescriptorKind::StorageBuffer;
    binding.role = DescriptorRole::GuestBuffers;
    binding.descriptorSet = 0;
    binding.binding = 0;
    binding.count = 1;
    binding.bufferWritten = {true};
    guest.bindings.push_back(std::move(binding));
    guest.hostSubgroupSize = hostSubgroupSize;
    return guest;
}

} // namespace ShaderRecompiler::MetalBackend::Fixtures
