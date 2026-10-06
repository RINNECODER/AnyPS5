#include "MetalShaderBridge.hpp"
#include <spirv.hpp>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace ShaderRecompiler;
using namespace ShaderRecompiler::MetalBackend;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static RecompileResult Sample(std::initializer_list<spv::Capability> extra = {}, bool specialized = false, bool push = false) {
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x00010300u, 0u, 24u, 0u};
    const auto emit = [&](spv::Op op, std::initializer_list<std::uint32_t> args) {
        words.push_back((static_cast<std::uint32_t>(args.size() + 1) << 16u) | op);
        words.insert(words.end(), args);
    };
    emit(spv::OpCapability, {spv::CapabilityShader});
    for (auto capability : extra) emit(spv::OpCapability, {capability});
    emit(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    emit(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, 10, 0x6e69616du, 0});
    if (specialized) emit(spv::OpExecutionModeId, {10, spv::ExecutionModeLocalSizeId, 16, 16, 16});
    else emit(spv::OpExecutionMode, {10, spv::ExecutionModeLocalSize, 1, 1, 1});
    if (specialized) emit(spv::OpDecorate, {16, spv::DecorationSpecId, 1});
    emit(spv::OpDecorate, {4, spv::DecorationArrayStride, 4});
    emit(spv::OpDecorate, {5, spv::DecorationBlock});
    emit(spv::OpMemberDecorate, {5, 0, spv::DecorationOffset, 0});
    emit(spv::OpDecorate, {7, spv::DecorationDescriptorSet, 2});
    emit(spv::OpDecorate, {7, spv::DecorationBinding, 7});
    if (push) {
        emit(spv::OpDecorate, {17, spv::DecorationBlock});
        emit(spv::OpMemberDecorate, {17, 0, spv::DecorationOffset, 16});
    }
    emit(spv::OpTypeVoid, {1});
    emit(spv::OpTypeFunction, {2, 1});
    emit(spv::OpTypeInt, {3, 32, 0});
    emit(spv::OpTypeRuntimeArray, {4, 3});
    emit(spv::OpTypeStruct, {5, 4});
    emit(spv::OpTypePointer, {6, spv::StorageClassStorageBuffer, 5});
    emit(spv::OpTypePointer, {8, spv::StorageClassStorageBuffer, 3});
    emit(spv::OpConstant, {3, 9, 0});
    emit(spv::OpConstant, {3, 14, 42});
    if (specialized) emit(spv::OpSpecConstant, {3, 16, 4});
    if (push) {
        emit(spv::OpTypeStruct, {17, 3});
        emit(spv::OpTypePointer, {18, spv::StorageClassPushConstant, 17});
        emit(spv::OpTypePointer, {20, spv::StorageClassPushConstant, 3});
        emit(spv::OpVariable, {18, 19, spv::StorageClassPushConstant});
    }
    emit(spv::OpVariable, {6, 7, spv::StorageClassStorageBuffer});
    emit(spv::OpFunction, {1, 10, spv::FunctionControlMaskNone, 2});
    emit(spv::OpLabel, {11});
    emit(spv::OpArrayLength, {3, 12, 7, 0});
    emit(spv::OpIAdd, {3, 13, 12, 14});
    emit(spv::OpAccessChain, {8, 15, 7, 9, 9});
    if (push) {
        emit(spv::OpAccessChain, {20, 21, 19, 9});
        emit(spv::OpLoad, {3, 22, 21});
        emit(spv::OpIAdd, {3, 23, 13, 22});
    }
    emit(spv::OpStore, {15, push ? 23u : 13u});
    emit(spv::OpReturn, {});
    emit(spv::OpFunctionEnd, {});
    RecompileResult guest;
    guest.spirv = std::move(words);
    DescriptorBinding binding{};
    binding.kind = DescriptorKind::StorageBuffer;
    binding.role = DescriptorRole::GuestBuffers;
    binding.descriptorSet = 2;
    binding.binding = 7;
    binding.count = 1;
    binding.bufferWritten = {true};
    guest.bindings.push_back(std::move(binding));
    guest.variantId = 191;
    guest.hostSubgroupSize = 32;
    if (push) {
        const std::uint32_t bias = 11;
        guest.pushConstants.resize(sizeof(bias));
        std::memcpy(guest.pushConstants.data(), &bias, sizeof(bias));
    }
    return guest;
}

template<class Callable>
static void Reject(Callable callable, const char* reason) {
    try { callable(); }
    catch (const std::exception& error) {
        Require(std::strstr(error.what(), reason) != nullptr, error.what());
        return;
    }
    throw std::runtime_error(std::string("accepted invalid contract: ") + reason);
}

int main() {
    try {
        const auto sample = Sample();
        TargetOptions target;
        target.pushConstantBuffer = 0;
        target.bufferSizesBuffer = 1;
        const auto converted = ConvertToMetal(sample, ShaderStage::Compute, target);
        Require(!converted.source.empty() && !converted.entryPoint.empty(), "conversion omitted executable MSL");
        Require(converted.stage == ShaderStage::Compute && converted.threadsPerThreadgroup == std::array<std::uint32_t, 3>{1, 1, 1}, "lost stage/workgroup contract");
        Require(converted.resources.size() == 1 && converted.resources[0].active && converted.resources[0].buffer == 2,
                "descriptor remapping overlaps auxiliary buffers");
        Require(converted.resources[0].descriptorSet == 2 && converted.resources[0].binding == 7 &&
                converted.resources[0].requiresByteLengths && converted.bufferSizesBuffer == 1,
                "array-length resource reflection omitted byte-length binding");
        Require(converted.guest.variantId == 191 && converted.guest.hostSubgroupSize == 32 &&
                converted.guest.bindings[0].bufferWritten == std::vector<bool>{true}, "lost guest variant/write contract");
        const auto pushGuest = Sample({}, false, true);
        target.pushConstantOffsetBytes = 16;
        const auto withPush = ConvertToMetal(pushGuest, ShaderStage::Compute, target);
        Require(withPush.pushConstantBuffer == 0 && withPush.pushConstantBytes == 20 && withPush.pushConstantData.size() == 20, "lost declared push block size");
        Require(std::all_of(withPush.pushConstantData.begin(), withPush.pushConstantData.begin() + 16, [](auto byte) { return byte == std::byte{}; }) &&
                std::equal(pushGuest.pushConstants.begin(), pushGuest.pushConstants.end(), withPush.pushConstantData.begin() + 16), "stage push region was not padded at its original offset");
        Require(withPush.guest.pushConstants == pushGuest.pushConstants, "changed original guest push region");
        target.pushConstantOffsetBytes = 17;
        Reject([&] { static_cast<void>(ConvertToMetal(pushGuest, ShaderStage::Compute, target)); }, "region exceeds the shader block");
        auto invalid = sample;
        invalid.bindings.clear();
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "missing from guest binding metadata");
        invalid = sample;
        invalid.bindings[0].count = 2;
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "kind/count disagrees");
        invalid = sample;
        invalid.bindings[0].kind = DescriptorKind::UniformBuffer;
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "kind/count disagrees");
        invalid = sample;
        invalid.bindings.push_back(invalid.bindings[0]);
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "duplicate descriptor");
        Reject([&] { static_cast<void>(ConvertToMetal(sample, ShaderStage::Vertex)); }, "matching the requested stage");
        Reject([&] { static_cast<void>(ConvertToMetal(sample, ShaderStage::TessellationControl)); }, "unimplemented native Metal scheduling");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityFloat64}), ShaderStage::Compute)); }, "Float64");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityInt64Atomics}), ShaderStage::Compute)); }, "64-bit atomics");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityInt64}), ShaderStage::Compute)); }, "lacks 64-bit integer");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityPhysicalStorageBufferAddresses}), ShaderStage::Compute)); }, "lacks GPU-address");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({spv::CapabilityGroupNonUniform}), ShaderStage::Compute)); }, "lacks SIMD-group");
        Reject([&] { static_cast<void>(ConvertToMetal(Sample({}, true), ShaderStage::Compute)); }, "specialized compute workgroup");
        invalid = sample;
        invalid.bdaAbiVersion = 1;
        Reject([&] { static_cast<void>(ConvertToMetal(invalid, ShaderStage::Compute)); }, "incompatible BDA ABI");
        std::cout << "PASS: descriptor reflection, implicit buffer lengths, preserved guest metadata, and unsupported-contract rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
