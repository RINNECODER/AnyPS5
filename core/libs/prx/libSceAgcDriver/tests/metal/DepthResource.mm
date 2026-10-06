#include "MetalTestSupport.hpp"
#include "MetalDepthSurface.hpp"
#include <cmath>

namespace {
using Surface = AgcDriver::Metal::MetalDepthSurface;
constexpr std::uint32_t width = 5, height = 3;

id<MTLLibrary> library(id<MTLDevice> device) {
    NSError* error = nil;
    auto result = [device newLibraryWithSource:@R"metal(
#include <metal_stdlib>
using namespace metal;
vertex float4 DepthVertex(uint index [[vertex_id]], constant float& depth [[buffer(0)]]) {
    const float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(positions[index], depth, 1);
}
fragment float4 DepthColor(constant float4& color [[buffer(0)]]) { return color; }
kernel void SampleDepth(depth2d<float> depth [[texture(0)]], device float* values [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
    constexpr sampler nearest(coord::pixel, address::clamp_to_edge, filter::nearest);
    values[at.y * 5 + at.x] = depth.sample(nearest, float2(at) + 0.5);
}
kernel void ReadStencil(texture2d<uint> stencil [[texture(0)]], device uint* values [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
    values[at.y * 5 + at.x] = stencil.read(at).x;
}
)metal" options:nil error:&error];
    if (result == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth resource shader compilation failed");
    return result;
}

void samples(const MetalTests::Context& context, id<MTLLibrary> shaders, const Surface& surface, float expectedDepth, std::uint32_t expectedStencil) {
    for (const bool stencil : {false, true}) {
        if (stencil && surface.StencilTexture() == nil) continue;
        NSError* error = nil;
        auto pipeline = [context.device newComputePipelineStateWithFunction:[shaders newFunctionWithName:stencil ? @"ReadStencil" : @"SampleDepth"] error:&error];
        if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth resource compute pipeline creation failed");
        auto output = context.Buffer(width * height * sizeof(std::uint32_t), 0xa5);
        auto commands = [context.queue commandBuffer];
        auto encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:stencil ? surface.SampledStencilView() : surface.SampledDepthView() atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Depth resource sampling dispatch failed");
        for (std::size_t i = 0; i < width * height; ++i) {
            if (stencil) MetalTests::Require(static_cast<const std::uint32_t*>(output.contents)[i] == expectedStencil, "Sampled stencil plane returned incorrect GPU bytes");
            else MetalTests::Require(std::abs(static_cast<const float*>(output.contents)[i] - expectedDepth) < 0.00004f, "Sampled depth plane returned incorrect GPU values");
        }
    }
}

void bytes(const MetalTests::Context& context, const Surface& surface, bool stencil, std::uint32_t expected) {
    constexpr std::size_t rowBytes = 256;
    const bool d16 = surface.Target().format == VK_FORMAT_D16_UNORM || surface.Target().format == VK_FORMAT_D16_UNORM_S8_UINT;
    const std::size_t elementBytes = stencil ? 1 : d16 ? 2 : 4;
    auto output = context.Buffer(rowBytes * height + 256, 0xa5);
    auto commands = [context.queue commandBuffer];
    auto encoder = [commands blitCommandEncoder];
    const bool combined = surface.DepthTexture().pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    const auto options = combined ? (stencil ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil) : MTLBlitOptionNone;
    [encoder copyFromTexture:stencil ? surface.StencilTexture() : surface.DepthTexture() sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                   toBuffer:output destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * height options:options];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Depth/stencil verification blit failed");
    const auto* data = static_cast<const std::uint8_t*>(output.contents);
    for (std::size_t i = 0; i < output.length; ++i) {
        const auto row = i / rowBytes, column = i % rowBytes;
        const auto wanted = row < height && column < width * elementBytes ? std::uint8_t(expected >> (8 * (column % elementBytes))) : 0xa5;
        MetalTests::Require(data[i] == wanted, "Depth/stencil plane bytes or blit row padding differ from the expected GPU result");
    }
}

void render(const MetalTests::Context& context, id<MTLLibrary> shaders, const Surface& surface) {
    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [shaders newFunctionWithName:@"DepthVertex"];
    descriptor.fragmentFunction = [shaders newFunctionWithName:@"DepthColor"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.depthAttachmentPixelFormat = surface.DepthTexture().pixelFormat;
    descriptor.stencilAttachmentPixelFormat = surface.StencilTexture() == nil ? MTLPixelFormatInvalid : surface.StencilTexture().pixelFormat;
    NSError* error = nil;
    auto pipeline = [context.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth resource render pipeline creation failed");
    auto stateDescriptor = [MTLDepthStencilDescriptor new];
    stateDescriptor.depthCompareFunction = MTLCompareFunctionLess;
    stateDescriptor.depthWriteEnabled = YES;
    if (surface.StencilTexture() != nil) {
        auto stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = MTLCompareFunctionAlways;
        stencil.depthStencilPassOperation = MTLStencilOperationIncrementClamp;
        stateDescriptor.frontFaceStencil = stencil;
        stateDescriptor.backFaceStencil = stencil;
    }
    auto state = [context.device newDepthStencilStateWithDescriptor:stateDescriptor];
    auto colorDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    colorDescriptor.usage = MTLTextureUsageRenderTarget;
    auto color = [context.device newTextureWithDescriptor:colorDescriptor];
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.depthAttachment.texture = surface.DepthTexture();
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    if (surface.StencilTexture() != nil) {
        pass.stencilAttachment.texture = surface.StencilTexture();
        pass.stencilAttachment.loadAction = MTLLoadActionLoad;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    auto commands = [context.queue commandBuffer];
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    MetalTests::Require(encoder != nil, "Depth resource render encoder creation failed");
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:state];
    const std::array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1};
    const float behind = 1, front = 0;
    [encoder setVertexBytes:&behind length:sizeof(behind) atIndex:0];
    [encoder setFragmentBytes:red.data() length:sizeof(red) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder setVertexBytes:&front length:sizeof(front) atIndex:0];
    [encoder setFragmentBytes:green.data() length:sizeof(green) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    auto pixels = context.Buffer(256 * height, 0xa5);
    auto blit = [commands blitCommandEncoder];
    [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
                toBuffer:pixels destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Depth resource rendering failed");
    const std::array<std::uint8_t, 4> greenBytes{0, 255, 0, 255};
    const auto* data = static_cast<const std::uint8_t*>(pixels.contents);
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        MetalTests::Require(std::memcmp(data + y * 256 + x * 4, greenBytes.data(), 4) == 0, "Retained depth attachment produced an incorrect rendered pixel");
    }
}
}

void RunDepthResourceTests(const MetalTests::Context& context) {
    AgcDriver::Metal::MetalDevice backend(context.device, context.library);
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    auto shaders = library(context.device);
    for (const auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        const bool stencil = format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
        AgcDriver::Graphics::DepthTarget target{0x400000, stencil ? 0x800000ull : 0, {width, height}, format, 1, 13};
        auto surface = cache.Acquire(target);
        MetalTests::Require(surface->DepthTexture().storageMode == MTLStorageModePrivate, "Depth surface did not allocate a private Metal attachment");
        samples(context, shaders, *surface, 1, 13);
        bytes(context, *surface, false, format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT ? 0xffff : 0x3f800000);
        if (stencil) bytes(context, *surface, true, 13);
        render(context, shaders, *surface);
        target.clearDepth = 0.75f;
        target.clearStencil = 99;
        auto retained = cache.Acquire(target);
        MetalTests::Require(retained == surface, "Depth surface cache did not retain attachment identity across changed clear registers");
        samples(context, shaders, *retained, 0, 14);
        bytes(context, *retained, false, 0);
        if (stencil) bytes(context, *retained, true, 14);
        cache.Clear();
        target.clearDepth = 1;
        auto fresh = cache.Acquire(target);
        MetalTests::Require(fresh != retained, "Depth surface cache clear retained a stale GPU resource");
        samples(context, shaders, *fresh, 1, 99);
    }
}
