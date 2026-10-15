#include "MetalTestSupport.hpp"
#include "MetalShaderPipeline.hpp"
#include "MetalPipelineCache.hpp"
#include <spirv.hpp>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace ShaderRecompiler;
using namespace ShaderRecompiler::MetalBackend;
using namespace AgcDriver::Metal;

class SpirvFixture {
public:
    SpirvFixture() : words{spv::MagicNumber, 0x00010300u, 0u, 128u, 0u} {}

    void Add(spv::Op operation, std::initializer_list<std::uint32_t> operands) {
        words.push_back((static_cast<std::uint32_t>(operands.size() + 1) << 16) | operation);
        words.insert(words.end(), operands);
    }

    void Entry(spv::ExecutionModel stage, std::uint32_t function,
                std::initializer_list<std::uint32_t> interfaces) {
        const std::array<std::uint32_t, 2> name{0x6e69616du, 0u};
        words.push_back((static_cast<std::uint32_t>(5 + interfaces.size()) << 16) | spv::OpEntryPoint);
        words.push_back(stage);
        words.push_back(function);
        words.insert(words.end(), name.begin(), name.end());
        words.insert(words.end(), interfaces);
    }

    RecompileResult Result(std::vector<DescriptorBinding> bindings, std::span<const std::byte> pushConstants = {}) {
        RecompileResult result;
        result.spirv = std::move(words);
        result.bindings = std::move(bindings);
        result.pushConstants.assign(pushConstants.begin(), pushConstants.end());
        return result;
    }

private:
    std::vector<std::uint32_t> words;
};

DescriptorBinding Descriptor(DescriptorKind kind, DescriptorRole role, std::uint32_t set,
                             std::uint32_t binding, std::uint32_t count = 1) {
    DescriptorBinding result{};
    result.kind = kind;
    result.role = role;
    result.descriptorSet = set;
    result.binding = binding;
    result.count = count;
    return result;
}

template<class TValue>
std::span<const std::byte> Bytes(const TValue& value) {
    return std::as_bytes(std::span(&value, 1));
}

RecompileResult ComputeFixture() {
    SpirvFixture module;
    module.Add(spv::OpCapability, {spv::CapabilityShader});
    module.Add(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    module.Entry(spv::ExecutionModelGLCompute, 30, {20});
    module.Add(spv::OpExecutionMode, {30, spv::ExecutionModeLocalSize, 4, 1, 1});
    module.Add(spv::OpDecorate, {20, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId});
    module.Add(spv::OpDecorate, {6, spv::DecorationArrayStride, 4});
    module.Add(spv::OpDecorate, {7, spv::DecorationBlock});
    module.Add(spv::OpMemberDecorate, {7, 0, spv::DecorationOffset, 0});
    module.Add(spv::OpDecorate, {21, spv::DecorationDescriptorSet, 2});
    module.Add(spv::OpDecorate, {21, spv::DecorationBinding, 7});
    module.Add(spv::OpDecorate, {22, spv::DecorationDescriptorSet, 5});
    module.Add(spv::OpDecorate, {22, spv::DecorationBinding, 3});
    module.Add(spv::OpDecorate, {12, spv::DecorationBlock});
    module.Add(spv::OpMemberDecorate, {12, 0, spv::DecorationOffset, 16});
    module.Add(spv::OpMemberDecorate, {12, 1, spv::DecorationOffset, 20});
    module.Add(spv::OpTypeVoid, {1});
    module.Add(spv::OpTypeFunction, {2, 1});
    module.Add(spv::OpTypeInt, {3, 32, 0});
    module.Add(spv::OpTypeVector, {4, 3, 3});
    module.Add(spv::OpTypePointer, {5, spv::StorageClassInput, 4});
    module.Add(spv::OpTypeRuntimeArray, {6, 3});
    module.Add(spv::OpTypeStruct, {7, 6});
    module.Add(spv::OpConstant, {3, 8, 3});
    module.Add(spv::OpTypeArray, {9, 7, 8});
    module.Add(spv::OpTypePointer, {10, spv::StorageClassStorageBuffer, 9});
    module.Add(spv::OpTypePointer, {11, spv::StorageClassStorageBuffer, 7});
    module.Add(spv::OpTypeStruct, {12, 3, 3});
    module.Add(spv::OpTypePointer, {13, spv::StorageClassPushConstant, 12});
    module.Add(spv::OpTypePointer, {14, spv::StorageClassPushConstant, 3});
    module.Add(spv::OpTypePointer, {15, spv::StorageClassStorageBuffer, 3});
    module.Add(spv::OpConstant, {3, 16, 0});
    module.Add(spv::OpConstant, {3, 17, 1});
    module.Add(spv::OpConstant, {3, 18, 2});
    module.Add(spv::OpVariable, {5, 20, spv::StorageClassInput});
    module.Add(spv::OpVariable, {10, 21, spv::StorageClassStorageBuffer});
    module.Add(spv::OpVariable, {11, 22, spv::StorageClassStorageBuffer});
    module.Add(spv::OpVariable, {13, 23, spv::StorageClassPushConstant});
    module.Add(spv::OpFunction, {1, 30, spv::FunctionControlMaskNone, 2});
    module.Add(spv::OpLabel, {31});
    module.Add(spv::OpLoad, {4, 32, 20});
    module.Add(spv::OpCompositeExtract, {3, 33, 32, 0});
    module.Add(spv::OpAccessChain, {15, 34, 21, 16, 16, 33});
    module.Add(spv::OpAccessChain, {15, 35, 21, 17, 16, 33});
    module.Add(spv::OpLoad, {3, 36, 34});
    module.Add(spv::OpLoad, {3, 37, 35});
    module.Add(spv::OpAccessChain, {14, 38, 23, 16});
    module.Add(spv::OpAccessChain, {14, 39, 23, 17});
    module.Add(spv::OpLoad, {3, 40, 38});
    module.Add(spv::OpLoad, {3, 41, 39});
    module.Add(spv::OpAccessChain, {11, 48, 21, 18});
    module.Add(spv::OpArrayLength, {3, 42, 48, 0});
    module.Add(spv::OpIMul, {3, 43, 36, 40});
    module.Add(spv::OpIAdd, {3, 44, 43, 37});
    module.Add(spv::OpIAdd, {3, 45, 44, 41});
    module.Add(spv::OpIAdd, {3, 46, 45, 42});
    module.Add(spv::OpAccessChain, {15, 47, 22, 16, 33});
    module.Add(spv::OpStore, {47, 46});
    module.Add(spv::OpReturn, {});
    module.Add(spv::OpFunctionEnd, {});
    const std::array<std::uint32_t, 2> push{5, 9};
    return module.Result({Descriptor(DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 2, 7, 3),
                          Descriptor(DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 5, 3)}, Bytes(push));
}

RecompileResult VertexFixture(std::uint32_t depth = 0) {
    SpirvFixture module;
    module.Add(spv::OpCapability, {spv::CapabilityShader});
    module.Add(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    module.Entry(spv::ExecutionModelVertex, 30, {20, 21, 22});
    module.Add(spv::OpDecorate, {20, spv::DecorationBuiltIn, spv::BuiltInVertexIndex});
    module.Add(spv::OpDecorate, {21, spv::DecorationBuiltIn, spv::BuiltInPosition});
    module.Add(spv::OpDecorate, {22, spv::DecorationLocation, 0});
    module.Add(spv::OpTypeVoid, {1});
    module.Add(spv::OpTypeFunction, {2, 1});
    module.Add(spv::OpTypeInt, {3, 32, 0});
    module.Add(spv::OpTypeInt, {4, 32, 1});
    module.Add(spv::OpTypeFloat, {5, 32});
    module.Add(spv::OpTypeVector, {6, 5, 4});
    module.Add(spv::OpTypePointer, {7, spv::StorageClassInput, 4});
    module.Add(spv::OpTypePointer, {8, spv::StorageClassOutput, 6});
    module.Add(spv::OpConstant, {3, 9, 1});
    module.Add(spv::OpConstant, {3, 10, 2});
    module.Add(spv::OpConstant, {5, 11, 0xbf800000u});
    module.Add(spv::OpConstant, {5, 12, 0x3f000000u});
    module.Add(spv::OpConstant, {5, 13, depth});
    module.Add(spv::OpConstant, {5, 14, 0x3f800000u});
    module.Add(spv::OpConstant, {5, 15, 0x3e800000u});
    module.Add(spv::OpConstant, {5, 16, 0x3f000000u});
    module.Add(spv::OpConstant, {5, 17, 0x3f400000u});
    module.Add(spv::OpConstantComposite, {6, 18, 15, 16, 17, 14});
    module.Add(spv::OpVariable, {7, 20, spv::StorageClassInput});
    module.Add(spv::OpVariable, {8, 21, spv::StorageClassOutput});
    module.Add(spv::OpVariable, {8, 22, spv::StorageClassOutput});
    module.Add(spv::OpFunction, {1, 30, spv::FunctionControlMaskNone, 2});
    module.Add(spv::OpLabel, {31});
    module.Add(spv::OpLoad, {4, 32, 20});
    module.Add(spv::OpBitcast, {3, 33, 32});
    module.Add(spv::OpShiftLeftLogical, {3, 34, 33, 9});
    module.Add(spv::OpBitwiseAnd, {3, 35, 34, 10});
    module.Add(spv::OpBitwiseAnd, {3, 36, 33, 10});
    module.Add(spv::OpConvertUToF, {5, 37, 35});
    module.Add(spv::OpConvertUToF, {5, 38, 36});
    module.Add(spv::OpFMul, {5, 39, 37, 12});
    module.Add(spv::OpFMul, {5, 40, 38, 12});
    module.Add(spv::OpFAdd, {5, 41, 39, 11});
    module.Add(spv::OpFAdd, {5, 42, 40, 11});
    module.Add(spv::OpCompositeConstruct, {6, 43, 41, 42, 13, 14});
    module.Add(spv::OpStore, {21, 43});
    module.Add(spv::OpStore, {22, 18});
    module.Add(spv::OpReturn, {});
    module.Add(spv::OpFunctionEnd, {});
    return module.Result({});
}

RecompileResult FragmentFixture() {
    SpirvFixture module;
    module.Add(spv::OpCapability, {spv::CapabilityShader});
    module.Add(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
    module.Entry(spv::ExecutionModelFragment, 40, {30, 31});
    module.Add(spv::OpExecutionMode, {40, spv::ExecutionModeOriginUpperLeft});
    module.Add(spv::OpDecorate, {30, spv::DecorationLocation, 0});
    module.Add(spv::OpDecorate, {31, spv::DecorationLocation, 0});
    module.Add(spv::OpDecorate, {32, spv::DecorationDescriptorSet, 4});
    module.Add(spv::OpDecorate, {32, spv::DecorationBinding, 6});
    module.Add(spv::OpDecorate, {33, spv::DecorationDescriptorSet, 4});
    module.Add(spv::OpDecorate, {33, spv::DecorationBinding, 9});
    module.Add(spv::OpDecorate, {34, spv::DecorationDescriptorSet, 7});
    module.Add(spv::OpDecorate, {34, spv::DecorationBinding, 2});
    module.Add(spv::OpDecorate, {13, spv::DecorationBlock});
    module.Add(spv::OpMemberDecorate, {13, 0, spv::DecorationOffset, 0});
    module.Add(spv::OpDecorate, {16, spv::DecorationBlock});
    module.Add(spv::OpMemberDecorate, {16, 0, spv::DecorationOffset, 16});
    module.Add(spv::OpTypeVoid, {1});
    module.Add(spv::OpTypeFunction, {2, 1});
    module.Add(spv::OpTypeFloat, {3, 32});
    module.Add(spv::OpTypeVector, {4, 3, 4});
    module.Add(spv::OpTypeVector, {5, 3, 2});
    module.Add(spv::OpTypePointer, {6, spv::StorageClassInput, 4});
    module.Add(spv::OpTypePointer, {7, spv::StorageClassOutput, 4});
    module.Add(spv::OpTypeImage, {8, 3, spv::Dim2D, 0, 0, 0, 1, spv::ImageFormatUnknown});
    module.Add(spv::OpTypeSampler, {9});
    module.Add(spv::OpTypeSampledImage, {10, 8});
    module.Add(spv::OpTypePointer, {11, spv::StorageClassUniformConstant, 8});
    module.Add(spv::OpTypePointer, {12, spv::StorageClassUniformConstant, 9});
    module.Add(spv::OpTypeStruct, {13, 4});
    module.Add(spv::OpTypePointer, {14, spv::StorageClassUniform, 13});
    module.Add(spv::OpTypePointer, {15, spv::StorageClassUniform, 4});
    module.Add(spv::OpTypeStruct, {16, 4});
    module.Add(spv::OpTypePointer, {17, spv::StorageClassPushConstant, 16});
    module.Add(spv::OpTypePointer, {18, spv::StorageClassPushConstant, 4});
    module.Add(spv::OpTypeInt, {19, 32, 0});
    module.Add(spv::OpConstant, {19, 20, 0});
    module.Add(spv::OpConstant, {3, 21, 0x3f000000u});
    module.Add(spv::OpConstantComposite, {5, 22, 21, 21});
    module.Add(spv::OpVariable, {6, 30, spv::StorageClassInput});
    module.Add(spv::OpVariable, {7, 31, spv::StorageClassOutput});
    module.Add(spv::OpVariable, {11, 32, spv::StorageClassUniformConstant});
    module.Add(spv::OpVariable, {12, 33, spv::StorageClassUniformConstant});
    module.Add(spv::OpVariable, {14, 34, spv::StorageClassUniform});
    module.Add(spv::OpVariable, {17, 35, spv::StorageClassPushConstant});
    module.Add(spv::OpFunction, {1, 40, spv::FunctionControlMaskNone, 2});
    module.Add(spv::OpLabel, {41});
    module.Add(spv::OpLoad, {8, 42, 32});
    module.Add(spv::OpLoad, {9, 43, 33});
    module.Add(spv::OpSampledImage, {10, 44, 42, 43});
    module.Add(spv::OpImageSampleImplicitLod, {4, 45, 44, 22});
    module.Add(spv::OpLoad, {4, 46, 30});
    module.Add(spv::OpAccessChain, {15, 47, 34, 20});
    module.Add(spv::OpLoad, {4, 48, 47});
    module.Add(spv::OpAccessChain, {18, 49, 35, 20});
    module.Add(spv::OpLoad, {4, 50, 49});
    module.Add(spv::OpFMul, {4, 51, 45, 46});
    module.Add(spv::OpFMul, {4, 52, 51, 48});
    module.Add(spv::OpFMul, {4, 53, 52, 50});
    module.Add(spv::OpStore, {31, 53});
    module.Add(spv::OpReturn, {});
    module.Add(spv::OpFunctionEnd, {});
    const std::array<float, 4> push{2.0f, 1.0f, 0.5f, 1.0f};
    auto texture = Descriptor(DescriptorKind::SampledImage, DescriptorRole::GuestImages, 4, 6);
    texture.imageShape = DescriptorImageShape::Image2D;
    return module.Result({Descriptor(DescriptorKind::Sampler, DescriptorRole::GuestSamplers, 4, 9),
                          Descriptor(DescriptorKind::UniformBuffer, DescriptorRole::ShaderData, 7, 2), texture}, Bytes(push));
}

void Complete(id<MTLCommandBuffer> commands) {
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
        const char* error = commands.error.localizedDescription.UTF8String;
        throw std::runtime_error(error == nullptr ? "Converted shader GPU commands did not complete" : error);
    }
}

void Compute(const MetalTests::Context& context) {
    auto guest = ComputeFixture();
    TargetOptions options;
    options.pushConstantOffsetBytes = 16;
    MetalComputePipeline pipeline(context.device, ConvertToMetal(guest, ShaderStage::Compute, options));
    const std::array<std::uint32_t, 4> first{3, 8, 17, 31};
    const std::array<std::uint32_t, 4> second{2, 4, 6, 10};
    auto input0 = context.Buffer(sizeof(first));
    auto input1 = context.Buffer(16 + sizeof(second), 0xab);
    auto output = context.Buffer(7 * sizeof(std::uint32_t), 0xa5);
    std::memcpy(input0.contents, first.data(), sizeof(first));
    std::memcpy(static_cast<std::byte*>(input1.contents) + 16, second.data(), sizeof(second));
    std::array<MetalShaderResourceBinding, 2> bindings;
    bindings[0].descriptorSet = 5;
    bindings[0].binding = 3;
    bindings[0].buffers = {{output, 0, output.length}};
    bindings[1].descriptorSet = 2;
    bindings[1].binding = 7;
    bindings[1].buffers = {{input0, 0, sizeof(first)}, {input1, 16, sizeof(second)}, {output, 0, output.length}};
    id<MTLCommandBuffer> commands = [context.queue commandBuffer];
    bool rejected = false;
    try {
        pipeline.Encode(commands, std::span(bindings).first(1), MTLSizeMake(4, 1, 1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    MetalTests::Require(rejected, "Converted compute silently accepted an unresolved active descriptor");
    rejected = false;
    try {
        pipeline.Encode(commands, bindings, MTLSizeMake(4, 1, 1), guest.pushConstants);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    MetalTests::Require(rejected, "Converted compute accepted a stage-region push override as a complete block");
    rejected = false;
    try {
        pipeline.Encode(commands, bindings, MTLSizeMake(3, 1, 1));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    MetalTests::Require(rejected, "Converted compute accepted a partial native workgroup for a fixed-size SPIRV contract");
    for (const NSUInteger visibleBytes : {output.length, NSUInteger{0}}) {
        bindings[1].buffers[2].length = visibleBytes;
        std::memset(output.contents, 0xa5, output.length);
        commands = [context.queue commandBuffer];
        pipeline.Encode(commands, bindings, MTLSizeMake(4, 1, 1));
        Complete(commands);
        const std::array<std::uint32_t, 7> expected = visibleBytes == 0 ?
            std::array<std::uint32_t, 7>{26, 53, 100, 174, 0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u} :
            std::array<std::uint32_t, 7>{33, 60, 107, 181, 0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u};
        MetalTests::Require(std::memcmp(output.contents, expected.data(), sizeof(expected)) == 0,
                            "Converted SPIRV compute lost descriptor arrays, buffer offsets, explicit zero ArrayLength, push offset, or output bounds");
    }
    // DISPATCH_INDIRECT: the group counts are zero when encoded and written by earlier GPU work in
    // the same command buffer; the dispatch must run with the counts the GPU wrote.
    {
        bindings[1].buffers[2].length = output.length;
        std::memset(output.contents, 0xa5, output.length);
        const auto groups = static_cast<std::uint32_t>(4 / pipeline.Reflection().threadsPerThreadgroup[0]);
        const std::array<std::uint32_t, 4> counts{groups, 1, 1, 0};
        auto staged = context.Buffer(sizeof(counts));
        std::memcpy(staged.contents, counts.data(), sizeof(counts));
        auto arguments = context.Buffer(4 + sizeof(counts), 0);
        commands = [context.queue commandBuffer];
        auto blit = [commands blitCommandEncoder];
        [blit copyFromBuffer:staged sourceOffset:0 toBuffer:arguments destinationOffset:4 size:sizeof(counts)];
        [blit endEncoding];
        rejected = false;
        try {
            pipeline.EncodeIndirect(commands, bindings, arguments, 2);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        MetalTests::Require(rejected, "Converted indirect compute accepted misaligned arguments");
        pipeline.EncodeIndirect(commands, bindings, arguments, 4);
        Complete(commands);
        const std::array<std::uint32_t, 7> expected{33, 60, 107, 181, 0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u};
        MetalTests::Require(std::memcmp(output.contents, expected.data(), sizeof(expected)) == 0,
                            "Converted indirect compute did not run with the group counts earlier GPU work wrote");
        // Zero groups dispatch nothing.
        std::memset(output.contents, 0xa5, output.length);
        std::memset(arguments.contents, 0, arguments.length);
        commands = [context.queue commandBuffer];
        pipeline.EncodeIndirect(commands, bindings, arguments, 4);
        Complete(commands);
        MetalTests::Require(static_cast<const std::uint32_t*>(output.contents)[0] == 0xa5a5a5a5u, "Converted indirect compute with zero groups wrote output");
    }
    const std::array<std::uint8_t, 16> prefix{0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
                                          0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab};
    MetalTests::Require(std::memcmp(input1.contents, prefix.data(), prefix.size()) == 0 &&
                        std::memcmp(static_cast<std::byte*>(input1.contents) + 16, second.data(), sizeof(second)) == 0,
                        "Converted compute modified input data or its untouched prefix");
}

void Render(const MetalTests::Context& context, bool negativeOneToOne = false) {
    TargetOptions options;
    options.pushConstantOffsetBytes = 16;
    TargetOptions vertexOptions;
    vertexOptions.fixupClipSpace = negativeOneToOne;
    auto vertex = ConvertToMetal(VertexFixture(negativeOneToOne ? 0xbf000000u : 0u), ShaderStage::Vertex, vertexOptions);
    auto fragment = ConvertToMetal(FragmentFixture(), ShaderStage::Fragment, options);
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    MetalRenderPipeline pipeline(context.device, std::move(vertex), std::move(fragment), descriptor);
    const std::array<float, 4> tint{0.5f, 1.0f, 1.0f, 1.0f};
    auto tintBuffer = context.Buffer(sizeof(tint));
    std::memcpy(tintBuffer.contents, tint.data(), sizeof(tint));
    MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                              width:2 height:2 mipmapped:NO];
    textureDescriptor.storageMode = MTLStorageModeShared;
    textureDescriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [context.device newTextureWithDescriptor:textureDescriptor];
    MetalTests::Require(texture != nil, "Cannot allocate converted fragment input texture");
    const std::array<std::uint8_t, 16> texels{255, 128, 64, 255, 255, 128, 64, 255,
                                          255, 128, 64, 255, 255, 128, 64, 255};
    [texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:texels.data() bytesPerRow:8];
    MTLSamplerDescriptor* samplerDescriptor = [[MTLSamplerDescriptor alloc] init];
    samplerDescriptor.minFilter = MTLSamplerMinMagFilterNearest;
    samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
    id<MTLSamplerState> sampler = [context.device newSamplerStateWithDescriptor:samplerDescriptor];
    MetalTests::Require(sampler != nil, "Cannot allocate converted fragment sampler");
    std::array<MetalShaderResourceBinding, 3> bindings;
    bindings[0].descriptorSet = 7;
    bindings[0].binding = 2;
    bindings[0].buffers = {{tintBuffer, 0, sizeof(tint)}};
    bindings[1].descriptorSet = 4;
    bindings[1].binding = 6;
    bindings[1].textures = {texture};
    bindings[2].descriptorSet = 4;
    bindings[2].binding = 9;
    bindings[2].samplers = {sampler};
    MTLTextureDescriptor* targetDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                             width:8 height:8 mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModeShared;
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [context.device newTextureWithDescriptor:targetDescriptor];
    MetalTests::Require(target != nil, "Cannot allocate converted draw offscreen target");
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLCommandBuffer> commands = [context.queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    MetalTests::Require(encoder != nil, "Cannot encode converted draw render pass");
    MTLTextureDescriptor* wrongDescriptor = [textureDescriptor copy];
    wrongDescriptor.textureType = MTLTextureType2DArray;
    wrongDescriptor.arrayLength = 2;
    id<MTLTexture> wrongTexture = [context.device newTextureWithDescriptor:wrongDescriptor];
    MetalTests::Require(wrongTexture != nil, "Cannot allocate wrong-dimension native texture fixture");
    bindings[1].textures = {wrongTexture};
    bool rejected = false;
    try {
        pipeline.Bind(encoder, {}, bindings);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!rejected) [encoder endEncoding];
    MetalTests::Require(rejected, "Converted render binding silently accepted wrong native texture dimensions");
    bindings[1].textures = {texture};
    pipeline.Bind(encoder, {}, bindings);
    [encoder setViewport:MTLViewport{0, 0, 8, 8, 0, 1}];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    Complete(commands);
    std::array<std::uint8_t, 8 * 8 * 4> pixels{};
    [target getBytes:pixels.data() bytesPerRow:8 * 4 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    const std::array<int, 4> expected{64, 64, 24, 255};
    const std::array<std::array<std::size_t, 2>, 6> covered{{{0, 0}, {1, 0}, {0, 1}, {2, 0}, {0, 2}, {1, 1}}};
    for (const auto& point : covered) {
        const auto offset = (point[1] * 8 + point[0]) * 4;
        for (std::size_t channel = 0; channel < 4; ++channel)
            MetalTests::Require(std::abs(static_cast<int>(pixels[offset + channel]) - expected[channel]) <= 1,
                                negativeOneToOne ? "Converted SPIRV negative-one-to-one clip depth incorrectly clipped a triangle at z=-0.5,w=1" :
                                    "Converted SPIRV positive-viewport triangle lost its original upper-left coverage or fragment data");
    }
    const std::array<std::array<std::size_t, 2>, 5> untouched{{{0, 7}, {1, 6}, {2, 5}, {7, 0}, {4, 4}}};
    for (const auto& point : untouched) {
        const auto offset = (point[1] * 8 + point[0]) * 4;
        for (std::size_t channel = 0; channel < 4; ++channel)
            MetalTests::Require(pixels[offset + channel] == 0,
                                "Converted SPIRV positive-viewport triangle covered pixels outside its original upper-left region");
    }
}

MTLRenderPipelineDescriptor* ColorDescriptor(MTLPixelFormat format, bool blend = false) {
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.colorAttachments[0].pixelFormat = format;
    descriptor.colorAttachments[0].blendingEnabled = blend;
    return descriptor;
}

void RenderVariant(const MetalTests::Context& context, MTLRenderPipelineDescriptor* descriptor, bool fixup = false) {
    TargetOptions vertexOptions;
    vertexOptions.fixupClipSpace = fixup;
    TargetOptions fragmentOptions;
    fragmentOptions.pushConstantOffsetBytes = 16;
    MetalRenderPipeline pipeline(context.device, ConvertToMetal(VertexFixture(), ShaderStage::Vertex, vertexOptions),
                                 ConvertToMetal(FragmentFixture(), ShaderStage::Fragment, fragmentOptions), descriptor);
}

// Libraries, pipelines and their reflection are reused only for identical native inputs, and a warm
// pipeline still produces the cold pipeline's results (Compute and Render check their output).
void Caches(const MetalTests::Context& context) {
    ClearMetalPipelineCaches();
    ClearConversionCache();
    Compute(context);
    const auto cold = GetMetalPipelineCacheStatistics();
    MetalTests::Require(cold.libraries.misses == 1 && cold.libraries.hits == 0 && cold.computePipelines.misses == 1 &&
                        cold.computePipelines.hits == 0, "cold compute pipeline was not a counted library and pipeline miss");
    Compute(context);
    const auto warm = GetMetalPipelineCacheStatistics();
    MetalTests::Require(warm.libraries.misses == 1 && warm.computePipelines.misses == 1 && warm.computePipelines.hits >= 1,
                        "identical compute pipeline recompiled its library or pipeline state");

    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA8Unorm));
    const auto renderCold = GetMetalPipelineCacheStatistics();
    MetalTests::Require(renderCold.renderPipelines.misses == 1 && renderCold.renderPipelines.hits == 0, "cold render pipeline was not a counted miss");
    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA8Unorm));
    auto stats = GetMetalPipelineCacheStatistics();
    MetalTests::Require(stats.renderPipelines.hits == 1 && stats.renderPipelines.misses == 1 &&
                        stats.libraries.misses == renderCold.libraries.misses, "identical render pipeline was rebuilt");
    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA16Float));
    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA8Unorm, true));
    stats = GetMetalPipelineCacheStatistics();
    MetalTests::Require(stats.renderPipelines.misses == 3 && stats.renderPipelines.hits == 1 &&
                        stats.libraries.misses == renderCold.libraries.misses,
                        "attachment format or blend variants shared a pipeline, or rebuilt identical libraries");
    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA8Unorm), true);
    stats = GetMetalPipelineCacheStatistics();
    MetalTests::Require(stats.renderPipelines.misses == 4 && stats.libraries.misses == renderCold.libraries.misses + 1,
                        "a clip-space fixup variant shared the unfixed vertex library or pipeline");
    Render(context);
    Render(context);
    Render(context, true);
    MetalTests::Require(GetMetalPipelineCacheStatistics().renderPipelines.hits >= 2, "repeated draws did not reuse their pipeline");
}

void SetEnvironment(const char* name, const char* value) {
    if (value == nullptr) unsetenv(name);
    else setenv(name, value, 1);
}

// The binary archive lives in the shader cache directory, is written on flush, survives a reload, and
// a corrupt or disabled archive never fails pipeline creation.
void Archive(const MetalTests::Context& context) {
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-metal-archive-test-" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(directory);
    SetEnvironment("ANYPS5_NO_SHADER_CACHE", nullptr);
    SetEnvironment("APS5_NO_SHADER_CACHE", nullptr);
    SetEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.c_str());
    ClearMetalPipelineCaches();
    const auto path = MetalPipelineArchivePath(context.device);
    MetalTests::Require(!path.empty() && path.parent_path() == directory, "the pipeline archive is not under the shader cache directory");
    Compute(context);
    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA8Unorm));
    auto stats = GetMetalPipelineCacheStatistics();
    MetalTests::Require(stats.archiveAdditions >= 2 && stats.archiveFailures == 0, "new pipelines were not added to the binary archive");
    MetalTests::Require(FlushMetalPipelineArchives() && std::filesystem::file_size(path) > 0, "the binary archive was not written");
    ClearMetalPipelineCaches();
    Compute(context);
    MetalTests::Require(GetMetalPipelineCacheStatistics().archiveLoads == 1, "the written binary archive was not loaded");
    ClearMetalPipelineCaches();
    { std::ofstream(path, std::ios::binary | std::ios::trunc) << "not a metal binary archive"; }
    Compute(context);
    RenderVariant(context, ColorDescriptor(MTLPixelFormatRGBA8Unorm));
    MetalTests::Require(GetMetalPipelineCacheStatistics().archiveLoads == 0, "a corrupt binary archive was reported as loaded");
    ClearMetalPipelineCaches();
    SetEnvironment("APS5_NO_SHADER_CACHE", "1");
    MetalTests::Require(MetalPipelineArchivePath(context.device).empty(), "APS5_NO_SHADER_CACHE=1 kept the binary archive");
    Compute(context);
    MetalTests::Require(GetMetalPipelineCacheStatistics().archiveAdditions == 0, "a disabled disk cache still archived pipelines");
    ClearMetalPipelineCaches();
    SetEnvironment("APS5_NO_SHADER_CACHE", nullptr);
    SetEnvironment("ANYPS5_SHADER_CACHE_DIR", nullptr);
    SetEnvironment("ANYPS5_NO_SHADER_CACHE", "1");
    std::filesystem::remove_all(directory);
}

// Many threads build the same and different compute pipelines while the binary archive is written
// (every batch and on flush) and dropped and re-read from disk (clear): every returned pipeline is a
// live object of the device, every write succeeds, and the final archive holds every pipeline.
void ConcurrentArchive(const MetalTests::Context& context) {
    const auto directory = std::filesystem::temp_directory_path() / ("aps5-metal-archive-stress-" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(directory);
    SetEnvironment("ANYPS5_NO_SHADER_CACHE", nullptr);
    SetEnvironment("APS5_NO_SHADER_CACHE", nullptr);
    SetEnvironment("ANYPS5_SHADER_CACHE_DIR", directory.c_str());
    ClearMetalPipelineCaches();
    constexpr std::size_t Kernels = 96;
    constexpr std::size_t Workers = 8;
    constexpr std::size_t Requests = 160;
    std::vector<std::string> sources;
    for (std::size_t i = 0; i < Kernels; ++i) {
        sources.push_back("#include <metal_stdlib>\nusing namespace metal;\nkernel void stress(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i] = i * " +
                          std::to_string(i + 3) + "u + " + std::to_string(i * 7) + "u; }\n");
    }
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> built{0};
    std::atomic<bool> failed{false};
    std::string failure;
    std::mutex failureMutex;
    const auto fail = [&](const std::string& what) {
        std::lock_guard lock(failureMutex);
        if (!failed.exchange(true)) failure = what;
    };
    const auto request = [&](std::size_t index) {
        id<MTLFunction> function = CachedShaderFunction(context.device, sources[index], "stress", false);
        auto descriptor = [MTLComputePipelineDescriptor new];
        descriptor.computeFunction = function;
        NSError* error = nil;
        const auto created = CachedComputePipelineState(context.device, descriptor, &error);
        if (created.state == nil || created.reflection == nil || created.state.device != context.device ||
            created.state.maxTotalThreadsPerThreadgroup == 0 || created.reflection.bindings.count != 1) {
            fail("a concurrent compute pipeline request returned no live pipeline or reflection");
        }
    };
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < Workers; ++worker) {
        workers.emplace_back([&, worker] {
            std::mt19937 random(static_cast<std::uint32_t>(worker * 7919 + 1));
            for (std::size_t iteration = 0; iteration < Requests && !failed.load(); ++iteration) {
                @autoreleasepool {
                    // Half the requests go to a few shared kernels, the rest spread over all of them.
                    try {
                        request((iteration % 2 == 0) ? random() % 4 : random() % Kernels);
                        built.fetch_add(1);
                    } catch (const std::exception& error) {
                        fail(std::string("a concurrent compute pipeline request threw: ") + error.what());
                    }
                }
            }
        });
    }
    std::thread writer([&] {
        for (std::size_t round = 0; !stop.load(); ++round) {
            @autoreleasepool {
                if (!FlushMetalPipelineArchives()) fail("writing the pipeline archive failed under concurrent pipeline creation");
                if (round % 8 == 7) ClearMetalPipelineCaches();
            }
        }
    });
    for (auto& worker : workers) worker.join();
    stop = true;
    writer.join();
    MetalTests::Require(!failed.load(), failure.c_str());
    MetalTests::Require(built.load() == Workers * Requests, "not every concurrent pipeline request completed");
    // Every kernel once more, then the written archive must hold each one (creation fails on a miss).
    for (std::size_t i = 0; i < Kernels; ++i) request(i);
    MetalTests::Require(!failed.load(), failure.c_str());
    const auto path = MetalPipelineArchivePath(context.device);
    MetalTests::Require(FlushMetalPipelineArchives() && std::filesystem::is_regular_file(path), "the final pipeline archive write failed");
    @autoreleasepool {
        MTLBinaryArchiveDescriptor* archiveDescriptor = [MTLBinaryArchiveDescriptor new];
        archiveDescriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        NSError* error = nil;
        id<MTLBinaryArchive> archive = [context.device newBinaryArchiveWithDescriptor:archiveDescriptor error:&error];
        MetalTests::Require(archive != nil, "the pipeline archive written under concurrent creation does not load");
        for (std::size_t i = 0; i < Kernels; ++i) {
            auto descriptor = [MTLComputePipelineDescriptor new];
            descriptor.computeFunction = CachedShaderFunction(context.device, sources[i], "stress", false);
            descriptor.binaryArchives = @[archive];
            id<MTLComputePipelineState> state = [context.device newComputePipelineStateWithDescriptor:descriptor
                options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
            MetalTests::Require(state != nil, "the pipeline archive written under concurrent creation misses a pipeline");
        }
    }
    ClearMetalPipelineCaches();
    SetEnvironment("ANYPS5_SHADER_CACHE_DIR", nullptr);
    SetEnvironment("ANYPS5_NO_SHADER_CACHE", "1");
    std::filesystem::remove_all(directory);
}

// A relinked title declares an old SDK, so the process default is MSL 2.3; converted shaders still
// compile as MSL 3.2, which the bridge's workgroup acquire/release fences need.
void LanguageVersion(const MetalTests::Context& context) {
    if (@available(macOS 15.0, *)) {
        using AgcDriver::Metal::ConvertedShaderLanguageVersion;
        MetalTests::Require(ConvertedShaderLanguageVersion(MTLLanguageVersion2_3, false) == MTLLanguageVersion3_2 &&
            ConvertedShaderLanguageVersion(MTLLanguageVersion3_2, false) == MTLLanguageVersion3_2 &&
            ConvertedShaderLanguageVersion(MTLLanguageVersion4_0, false) == MTLLanguageVersion4_0 &&
            ConvertedShaderLanguageVersion(MTLLanguageVersion4_0, true) == MTLLanguageVersion3_2,
            "Converted shader language version does not reach MSL 3.2");
        NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void fence(device atomic_uint* value [[buffer(0)]]) {\n"
            "  atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_threadgroup);\n"
            "  atomic_fetch_add_explicit(value, 1u, memory_order_relaxed);\n}\n";
        const auto compiles = [&](MTLLanguageVersion version) {
            auto options = [[MTLCompileOptions alloc] init];
            options.languageVersion = version;
            NSError* error = nil;
            return [context.device newLibraryWithSource:source options:options error:&error] != nil;
        };
        MetalTests::Require(!compiles(MTLLanguageVersion2_3), "MSL 2.3 unexpectedly accepted a seq_cst thread-scope fence");
        MetalTests::Require(compiles(ConvertedShaderLanguageVersion(MTLLanguageVersion2_3, false)),
            "Converted shader language version rejected the bridge's seq_cst fence");
    }
}

}

void RunShaderPipelineTests(const MetalTests::Context& context) {
    LanguageVersion(context);
    Compute(context);
    Render(context);
    Render(context, true);
    Caches(context);
    Archive(context);
    ConcurrentArchive(context);
}
