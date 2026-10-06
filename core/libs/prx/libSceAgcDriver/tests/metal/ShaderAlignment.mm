#include "MetalTestSupport.hpp"
#include "MetalShaderPipeline.hpp"
#include <spirv.hpp>
#include <array>
#include <cstring>

namespace {

using namespace ShaderRecompiler;
using namespace ShaderRecompiler::MetalBackend;
using namespace AgcDriver::Metal;

RecompileResult AlignmentFixture(bool wideInteger) {
    RecompileResult result;
    std::vector<std::uint32_t> words{spv::MagicNumber, 0x00010300u, 0u, 64u, 0u};
    const auto add = [&words](spv::Op op, std::initializer_list<std::uint32_t> values) {
        words.push_back((static_cast<std::uint32_t>(values.size() + 1) << 16) | op);
        words.insert(words.end(), values);
    };
    add(spv::OpCapability, {spv::CapabilityShader});
    if (wideInteger) add(spv::OpCapability, {spv::CapabilityInt64});
    add(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    add(spv::OpEntryPoint, {spv::ExecutionModelGLCompute, 20, 0x6e69616du, 0});
    add(spv::OpExecutionMode, {20, spv::ExecutionModeLocalSize, 1, 1, 1});
    add(spv::OpDecorate, {5, spv::DecorationBlock});
    add(spv::OpMemberDecorate, {5, 0, spv::DecorationOffset, 0});
    add(spv::OpDecorate, {10, spv::DecorationDescriptorSet, 2});
    add(spv::OpDecorate, {10, spv::DecorationBinding, 7});
    add(spv::OpDecorate, {11, spv::DecorationDescriptorSet, 2});
    add(spv::OpDecorate, {11, spv::DecorationBinding, 8});
    add(spv::OpTypeVoid, {1});
    add(spv::OpTypeFunction, {2, 1});
    add(spv::OpTypeInt, {3, 32, 0});
    if (wideInteger) add(spv::OpTypeInt, {4, 64, 0});
    else add(spv::OpTypeVector, {4, 3, 4});
    add(spv::OpTypeStruct, {5, 4});
    add(spv::OpTypePointer, {6, spv::StorageClassStorageBuffer, 5});
    add(spv::OpTypePointer, {7, spv::StorageClassStorageBuffer, 4});
    add(spv::OpConstant, {3, 8, 0});
    add(spv::OpVariable, {6, 10, spv::StorageClassStorageBuffer});
    add(spv::OpVariable, {6, 11, spv::StorageClassStorageBuffer});
    add(spv::OpFunction, {1, 20, spv::FunctionControlMaskNone, 2});
    add(spv::OpLabel, {21});
    add(spv::OpAccessChain, {7, 22, 10, 8});
    add(spv::OpLoad, {4, 23, 22});
    add(spv::OpAccessChain, {7, 24, 11, 8});
    add(spv::OpStore, {24, 23});
    add(spv::OpReturn, {});
    add(spv::OpFunctionEnd, {});
    result.spirv = std::move(words);
    for (const auto binding : {7u, 8u}) {
        DescriptorBinding descriptor{};
        descriptor.kind = DescriptorKind::StorageBuffer;
        descriptor.role = DescriptorRole::GuestBuffers;
        descriptor.descriptorSet = 2;
        descriptor.binding = binding;
        descriptor.count = 1;
        result.bindings.push_back(descriptor);
    }
    return result;
}

void CheckAlignment(const MetalTests::Context& context, bool wideInteger) {
    TargetOptions options;
    options.supportsInt64 = wideInteger;
    auto converted = ConvertToMetal(AlignmentFixture(wideInteger), ShaderStage::Compute, options);
    MetalComputePipeline pipeline(context.device, converted);
    const NSUInteger alignment = wideInteger ? 8 : 16;
    const NSUInteger bytes = alignment;
    const std::array<std::uint32_t, 4> values{0x12345678u, 0xabcdef01u, 0x87654321u, 0x10203040u};
    auto input = context.Buffer(64, 0xa5);
    auto output = context.Buffer(64, 0x5a);
    std::memcpy(static_cast<std::byte*>(input.contents) + alignment, values.data(), bytes);
    std::array<MetalShaderResourceBinding, 2> bindings;
    bindings[0].descriptorSet = bindings[1].descriptorSet = 2;
    bindings[0].binding = 7;
    bindings[1].binding = 8;
    bindings[0].buffers = {{input, alignment, bytes}};
    bindings[1].buffers = {{output, alignment, bytes}};
    auto commands = [context.queue commandBuffer];
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        for (NSUInteger offset = 4; offset < alignment; offset += 4) {
            bindings[index].buffers[0].offset = offset;
            bool rejected = false;
            try {
                pipeline.Encode(commands, bindings, MTLSizeMake(1, 1, 1));
            } catch (const std::invalid_argument& error) {
                rejected = std::strstr(error.what(), "native alignment") != nullptr;
            }
            MetalTests::Require(rejected, "Converted SPIRV accepted a range-valid but natively misaligned buffer offset");
        }
        bindings[index].buffers[0].offset = alignment;
    }
    auto missing = converted;
    for (auto& resource : missing.resources) {
        if (resource.binding == 7) resource.buffer = 28;
    }
    MetalComputePipeline missingPipeline(context.device, std::move(missing));
    bool rejected = false;
    try {
        missingPipeline.Encode(commands, bindings, MTLSizeMake(1, 1, 1));
    } catch (const std::runtime_error& error) {
        rejected = std::strstr(error.what(), "missing native alignment reflection") != nullptr;
    }
    MetalTests::Require(rejected, "Converted SPIRV silently assumed an alignment for a slot absent from native reflection");
    pipeline.Encode(commands, bindings, MTLSizeMake(1, 1, 1));
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Aligned converted SPIRV GPU commands failed");
    std::array<std::byte, 64> expected;
    std::memset(expected.data(), 0x5a, expected.size());
    std::memcpy(expected.data() + alignment, values.data(), bytes);
    MetalTests::Require(std::memcmp(output.contents, expected.data(), expected.size()) == 0,
                        "Aligned converted SPIRV lost buffer offset values or overwrote untouched output bytes");
}

}

void RunShaderAlignmentTests(const MetalTests::Context& context) {
    CheckAlignment(context, true);
    CheckAlignment(context, false);
}
