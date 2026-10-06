#include "MetalTestSupport.hpp"
#include "MetalShaderPipeline.hpp"
#include <spirv.hpp>
#include <array>
#include <cmath>
#include <cstring>

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
    module.Add(spv::OpConstant, {3, 8, 2});
    module.Add(spv::OpTypeArray, {9, 7, 8});
    module.Add(spv::OpTypePointer, {10, spv::StorageClassStorageBuffer, 9});
    module.Add(spv::OpTypePointer, {11, spv::StorageClassStorageBuffer, 7});
    module.Add(spv::OpTypeStruct, {12, 3, 3});
    module.Add(spv::OpTypePointer, {13, spv::StorageClassPushConstant, 12});
    module.Add(spv::OpTypePointer, {14, spv::StorageClassPushConstant, 3});
    module.Add(spv::OpTypePointer, {15, spv::StorageClassStorageBuffer, 3});
    module.Add(spv::OpConstant, {3, 16, 0});
    module.Add(spv::OpConstant, {3, 17, 1});
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
    module.Add(spv::OpArrayLength, {3, 42, 22, 0});
    module.Add(spv::OpIMul, {3, 43, 36, 40});
    module.Add(spv::OpIAdd, {3, 44, 43, 37});
    module.Add(spv::OpIAdd, {3, 45, 44, 41});
    module.Add(spv::OpIAdd, {3, 46, 45, 42});
    module.Add(spv::OpAccessChain, {15, 47, 22, 16, 33});
    module.Add(spv::OpStore, {47, 46});
    module.Add(spv::OpReturn, {});
    module.Add(spv::OpFunctionEnd, {});
    const std::array<std::uint32_t, 2> push{5, 9};
    return module.Result({Descriptor(DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 2, 7, 2),
                          Descriptor(DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 5, 3)}, Bytes(push));
}

RecompileResult VertexFixture() {
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
    module.Add(spv::OpConstant, {5, 12, 0x40000000u});
    module.Add(spv::OpConstant, {5, 13, 0});
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
    bindings[1].buffers = {{input0, 0, sizeof(first)}, {input1, 16, sizeof(second)}};
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
    pipeline.Encode(commands, bindings, MTLSizeMake(4, 1, 1));
    Complete(commands);
    const std::array<std::uint32_t, 7> expected{33, 60, 107, 181, 0xa5a5a5a5u, 0xa5a5a5a5u, 0xa5a5a5a5u};
    MetalTests::Require(std::memcmp(output.contents, expected.data(), sizeof(expected)) == 0,
                        "Converted SPIRV compute lost descriptor arrays, buffer offsets, ArrayLength, push offset, or output bounds");
    const std::array<std::uint8_t, 16> prefix{0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
                                          0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab};
    MetalTests::Require(std::memcmp(input1.contents, prefix.data(), prefix.size()) == 0 &&
                        std::memcmp(static_cast<std::byte*>(input1.contents) + 16, second.data(), sizeof(second)) == 0,
                        "Converted compute modified input data or its untouched prefix");
}

void Render(const MetalTests::Context& context) {
    TargetOptions options;
    options.pushConstantOffsetBytes = 16;
    auto vertex = ConvertToMetal(VertexFixture(), ShaderStage::Vertex);
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
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    Complete(commands);
    std::array<std::uint8_t, 8 * 8 * 4> pixels{};
    [target getBytes:pixels.data() bytesPerRow:8 * 4 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    const std::array<int, 4> expected{64, 64, 24, 255};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        MetalTests::Require(std::abs(static_cast<int>(pixels[i]) - expected[i % 4]) <= 1,
                            "Converted SPIRV vertex/fragment draw lost varying, texture, sampler, uniform, or offset push data");
    }
}

}

void RunShaderPipelineTests(const MetalTests::Context& context) {
    Compute(context);
    Render(context);
}
