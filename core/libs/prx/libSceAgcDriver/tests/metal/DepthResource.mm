#include "MetalTestSupport.hpp"
#include "MetalDepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include <cmath>
#include <memory>
#include <vector>

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
struct LayeredDepthOut { float4 position [[position]]; uint layer [[render_target_array_index]]; };
vertex LayeredDepthOut LayeredDepthVertex(uint index [[vertex_id]], uint instance [[instance_id]], constant float* depths [[buffer(0)]]) {
    const float2 positions[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return {float4(positions[index], depths[instance], 1), instance};
}
kernel void ReadDepthArray(depth2d_array<float> depth [[texture(0)]], device float* values [[buffer(0)]], uint3 at [[thread_position_in_grid]]) {
    values[(at.z * 3 + at.y) * 5 + at.x] = depth.read(at.xy, at.z);
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

// Shadow cascades render each slice of a depth array as its own resident surface (the depth target's
// address advanced by whole slices) and sample them through one 2D-array descriptor: the gathered
// array must hold each slice's depth in its own layer, and a range must start at its first layer.
void depthArray(const MetalTests::Context& context, const AgcDriver::Metal::MetalDevice& backend, id<MTLLibrary> shaders, VkFormat format) {
    const bool d16 = format == VK_FORMAT_D16_UNORM;
    const auto sliceBytes = AgcDriver::Graphics::DepthSliceBytes({width, height}, d16 ? 2u : 4u);
    std::vector<std::shared_ptr<Surface>> surfaces;
    for (std::uint32_t slice = 0; slice < 2; ++slice) {
        AgcDriver::Graphics::DepthTarget target{0x400000 + slice * sliceBytes, 0, {width, height}, format, 1, 0};
        surfaces.push_back(std::make_shared<Surface>(backend, target));
    }
    render(context, shaders, *surfaces[1]);
    NSError* error = nil;
    auto pipeline = [context.device newComputePipelineStateWithFunction:[shaders newFunctionWithName:@"ReadDepthArray"] error:&error];
    if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth array compute pipeline creation failed");
    const auto read = [&](id<MTLTexture> array) {
        const auto layers = static_cast<std::uint32_t>(array.arrayLength);
        auto output = context.Buffer(width * height * layers * sizeof(float), 0xa5);
        auto commands = [backend.Queue() commandBuffer];
        auto encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:array atIndex:0];
        [encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(width, height, layers) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Depth array read dispatch failed");
        return std::vector<float>(static_cast<const float*>(output.contents), static_cast<const float*>(output.contents) + width * height * layers);
    };
    const auto array = AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *surfaces[0], 0, 1, nil);
    MetalTests::Require(array.textureType == MTLTextureType2DArray && array.arrayLength == 2, "Depth array has the wrong shape");
    const auto both = read(array);
    for (std::size_t i = 0; i < width * height; ++i) {
        MetalTests::Require(both[i] == 1.0f, "Depth array layer 0 does not hold slice 0 (cleared to 1)");
        MetalTests::Require(both[width * height + i] == 0.0f, "Depth array layer 1 does not hold slice 1 (rendered at 0)");
    }
    const auto again = AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *surfaces[0], 0, 1, array);
    MetalTests::Require(again == array, "Depth array of the same shape was not reused");
    const auto second = read(AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *surfaces[0], 1, 1, nil));
    MetalTests::Require(second.size() == width * height && second[0] == 0.0f, "Depth array range does not start at its first layer");
    bool rejected = false;
    try {
        static_cast<void>(AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *surfaces[0], 0, 2, nil));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    MetalTests::Require(rejected, "Depth array with a slice no draw rendered was gathered");
}

// Every slice of a depth array is a layer of one set of planes: a layered draw (render target index
// per primitive) writes all of them at once, single-slice surfaces see what it wrote, a slice rendered
// on its own before its array was known moves into the array with its texels, the array grows without
// losing layers, and a 2D-array read of the slices is a view of the planes rather than a copy.
void layeredDepthArray(const MetalTests::Context& context, const AgcDriver::Metal::MetalDevice& backend, id<MTLLibrary> shaders, VkFormat format) {
    const bool d16 = format == VK_FORMAT_D16_UNORM;
    const auto sliceBytes = AgcDriver::Graphics::DepthSliceBytes({width, height}, d16 ? 2u : 4u);
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    const auto slice = [&](std::uint32_t index, std::uint32_t last) {
        AgcDriver::Graphics::DepthTarget target{0x400000 + index * sliceBytes, 0, {width, height}, format, 1, 0};
        target.slice = index;
        target.lastSlice = last;
        return target;
    };
    // Slice 1 rendered (to depth 0) as a plain surface before any draw names the array.
    auto plain = cache.Acquire(slice(0, 0));
    plain = nullptr;
    auto early = cache.Acquire([&] { auto target = slice(1, 0); target.slice = 0; return target; }());
    render(context, shaders, *early);
    auto first = cache.Acquire(slice(0, 2));
    auto second = cache.Acquire(slice(1, 2));
    MetalTests::Require(second == early && second->Planes() == first->Planes() && second->Layer() == 1 && first->Layer() == 0 &&
        first->Planes()->layers == 3, "Depth array slices do not share one set of planes");
    samples(context, shaders, *second, 0, 0);

    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [shaders newFunctionWithName:@"LayeredDepthVertex"];
    descriptor.depthAttachmentPixelFormat = first->DepthTexture().pixelFormat;
    descriptor.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    NSError* error = nil;
    auto pipeline = [context.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Layered depth pipeline creation failed");
    auto stateDescriptor = [MTLDepthStencilDescriptor new];
    stateDescriptor.depthCompareFunction = MTLCompareFunctionLess;
    stateDescriptor.depthWriteEnabled = YES;
    auto depthState = [context.device newDepthStencilStateWithDescriptor:stateDescriptor];
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = first->LayeredDepthTexture(3);
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.renderTargetArrayLength = 3;
    pass.renderTargetWidth = width;
    pass.renderTargetHeight = height;
    auto commands = [backend.Queue() commandBuffer];
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    MetalTests::Require(encoder != nil, "Layered depth encoder creation failed");
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:depthState];
    const std::array<float, 3> depths{0.25f, 0.75f, 0.5f};
    [encoder setVertexBytes:depths.data() length:sizeof(depths) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:3];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Layered depth draw failed");
    // Slice 2's surface is made after the draw: it must view what the draw wrote, not a cleared layer.
    auto third = cache.Acquire(slice(2, 2));
    MetalTests::Require(third->Planes() == first->Planes() && third->Layer() == 2, "A later slice surface is not a layer of its array");
    samples(context, shaders, *first, 0.25f, 0);
    samples(context, shaders, *second, 0, 0); // LESS kept the slice's own earlier depth
    samples(context, shaders, *third, 0.5f, 0);

    auto readPipeline = [context.device newComputePipelineStateWithFunction:[shaders newFunctionWithName:@"ReadDepthArray"] error:&error];
    if (readPipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Depth array compute pipeline creation failed");
    const auto read = [&](id<MTLTexture> array, std::vector<float> expected) {
        const auto layers = static_cast<std::uint32_t>(array.arrayLength);
        MetalTests::Require(layers == expected.size(), "Depth array view has the wrong layer count");
        auto output = context.Buffer(width * height * layers * sizeof(float), 0xa5);
        auto reads = [backend.Queue() commandBuffer];
        auto compute = [reads computeCommandEncoder];
        [compute setComputePipelineState:readPipeline];
        [compute setTexture:array atIndex:0];
        [compute setBuffer:output offset:0 atIndex:0];
        [compute dispatchThreads:MTLSizeMake(width, height, layers) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [compute endEncoding];
        [reads commit];
        [reads waitUntilCompleted];
        MetalTests::Require(reads.status == MTLCommandBufferStatusCompleted, "Depth array read dispatch failed");
        const auto* values = static_cast<const float*>(output.contents);
        for (std::uint32_t layer = 0; layer < layers; ++layer) for (std::uint32_t i = 0; i < width * height; ++i) {
            MetalTests::Require(std::abs(values[layer * width * height + i] - expected[layer]) < 0.00004f, "Depth array layer holds the wrong depth");
        }
    };
    std::vector<std::shared_ptr<Surface>> surfaces{first, second, third};
    const auto view = AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *first, 0, 2, nil);
    MetalTests::Require(view.parentTexture == first->Planes()->depth, "Slices of one array were copied instead of viewed in place");
    read(view, {0.25f, 0, 0.5f});
    read(AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *first, 1, 2, nil), {0, 0.5f});

    // A view naming a later slice grows the array: old layers keep their texels, surfaces follow.
    auto fourth = cache.Acquire(slice(3, 3));
    MetalTests::Require(fourth->Planes() == first->Planes() && first->Planes()->layers == 4 && fourth->Layer() == 3, "Depth array did not grow in place");
    surfaces.push_back(fourth);
    samples(context, shaders, *first, 0.25f, 0);
    samples(context, shaders, *fourth, 1, 0);
    read(AgcDriver::Metal::GatherDepthSlices(backend, surfaces, *first, 0, 3, nil), {0.25f, 0, 0.5f, 1});
}

// A block-compressed texture (or one of another texel size) at a resident depth surface's address is
// another resource in reused memory: the surface is dropped; a depth-sized view keeps it.
void replacedDepth(const AgcDriver::Metal::MetalDevice& backend) {
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    AgcDriver::Graphics::DepthTarget target{0x400000, 0, {width, height}, VK_FORMAT_D16_UNORM, 1, 0};
    std::vector<std::shared_ptr<Surface>> surfaces{cache.Acquire(target)};
    AgcDriver::Graphics::GuestTextureResource resource{};
    resource.baseAddress = target.address;
    resource.width = width;
    resource.height = height;
    resource.dimension = AgcDriver::Graphics::TextureDimension::k2D;
    resource.format = 7; // R16_UNORM: reads the depth plane
    AgcDriver::Metal::DropReplacedDepthSurfaces(surfaces, cache, resource);
    MetalTests::Require(surfaces.size() == 1 && cache.Acquire(target) == surfaces[0], "A depth-plane view dropped its resident surface");
    resource.baseAddress = target.address + 256;
    resource.format = 169; // BC1, elsewhere
    AgcDriver::Metal::DropReplacedDepthSurfaces(surfaces, cache, resource);
    MetalTests::Require(surfaces.size() == 1, "A texture at another address dropped a resident depth surface");
    resource.baseAddress = target.address;
    const auto old = surfaces[0];
    AgcDriver::Metal::DropReplacedDepthSurfaces(surfaces, cache, resource);
    MetalTests::Require(surfaces.empty() && cache.Acquire(target) != old, "A block-compressed texture over a depth surface kept the dead surface");
    surfaces = {cache.Acquire(target)};
    resource.format = 22; // R32_SFLOAT over a 16-bit depth plane
    AgcDriver::Metal::DropReplacedDepthSurfaces(surfaces, cache, resource);
    MetalTests::Require(surfaces.empty(), "A texture of another texel size over a depth surface kept the dead surface");
}

// Writes `depth` and `stencil` into a surface's planes, as a draw through it would.
void writePlanes(const MetalTests::Context& context, const Surface& surface, float depth, std::uint32_t stencil) {
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = surface.DepthTexture();
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = depth;
    if (surface.StencilTexture() != nil) {
        pass.stencilAttachment.texture = surface.StencilTexture();
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.clearStencil = stencil;
    }
    auto commands = [context.queue commandBuffer];
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Depth plane write failed");
}

// One depth plane bound with two stencil planes (and one stencil plane with two depth planes) is a
// surface per pairing; each binding takes the plane's current texels from the surface that bound it last.
void sharedPlanes(const MetalTests::Context& context, const AgcDriver::Metal::MetalDevice& backend, id<MTLLibrary> shaders, VkFormat format) {
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    const auto target = [&](std::uint64_t depth, std::uint64_t stencil) {
        return AgcDriver::Graphics::DepthTarget{depth, stencil, {width, height}, format, 1, 7};
    };
    auto first = cache.Acquire(target(0x400000, 0x800000));
    writePlanes(context, *first, 0.25f, 3);
    auto second = cache.Acquire(target(0x400000, 0x900000));
    MetalTests::Require(second != first, "Two stencil pairings of a depth plane share one surface");
    samples(context, shaders, *second, 0.25f, 7);
    bytes(context, *second, true, 7);  // only the depth plane carried over
    writePlanes(context, *second, 0.5f, 9);
    MetalTests::Require(cache.Acquire(target(0x400000, 0x900000)) == second, "Binding the same pairing again made another surface");
    samples(context, shaders, *second, 0.5f, 9);
    MetalTests::Require(cache.Acquire(target(0x400000, 0x800000)) == first, "Going back to the first pairing made another surface");
    samples(context, shaders, *first, 0.5f, 3);  // the depth written through the second pairing, its own stencil
    auto third = cache.Acquire(target(0xa00000, 0x800000));
    samples(context, shaders, *third, 1, 3);  // a new depth plane starts cleared; the stencil plane carries over
    bytes(context, *third, true, 3);
    auto other = cache.Acquire(AgcDriver::Graphics::DepthTarget{0x400000, 0xb00000, {width + 1, height}, format, 1, 7});
    writePlanes(context, *other, 0.75f, 1);
    MetalTests::Require(cache.Acquire(target(0x400000, 0x800000)) == first, "A pairing of another extent replaced the first pairing");
    samples(context, shaders, *first, 0.5f, 3);  // a plane of another extent does not carry over
    cache.Clear();
    auto fresh = cache.Acquire(target(0x400000, 0x900000));
    samples(context, shaders, *fresh, 1, 7);  // a cleared cache forgets which surface held the plane
}

// A multisampled depth plane pads its extent to the smaller swizzle blocks of its sample count, not to
// the single-sample blocks times the samples. A title placed a single-sample 1920x1080 depth target
// 0x1FF0000 bytes after a 4x one: the overstated 4x extent (35.4 MB) reached it, and binding it
// retired the 4x scene depth that water and fog then sampled from stale guest memory.
void multisampledDepthPlane(const AgcDriver::Metal::MetalDevice& backend) {
    const VkExtent2D extent{1920, 1080};
    MetalTests::Require(AgcDriver::Graphics::DepthPlaneBytes(extent, 4u, 1u) == AgcDriver::Graphics::DepthSliceBytes(extent, 4u) &&
        AgcDriver::Graphics::DepthPlaneBytes(extent, 2u, 1u) == AgcDriver::Graphics::DepthSliceBytes(extent, 2u) &&
        AgcDriver::Graphics::DepthPlaneBytes(extent, 1u, 1u) == AgcDriver::Graphics::DepthSliceBytes(extent, 1u),
        "A single-sample depth plane changed size");
    MetalTests::Require(AgcDriver::Graphics::DepthPlaneBytes(extent, 4u, 4u) == 1920ull * 1088u * 4u * 4u &&
        AgcDriver::Graphics::DepthPlaneBytes(extent, 4u, 2u) == 1920ull * 1088u * 4u * 2u &&
        AgcDriver::Graphics::DepthPlaneBytes(extent, 2u, 8u) == 1920ull * 1088u * 2u * 8u &&
        AgcDriver::Graphics::DepthPlaneBytes(extent, 1u, 4u) == 1920ull * 1152u * 4u,
        "A multisampled depth plane is not padded to its sample count's 64 KiB blocks");
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    AgcDriver::Graphics::DepthTarget scene{0x10000000, 0, extent, VK_FORMAT_D32_SFLOAT, 1, 0};
    scene.samples = 4;
    const AgcDriver::Graphics::DepthTarget next{scene.address + 0x1FF0000, 0, extent, VK_FORMAT_D32_SFLOAT, 1, 0};
    auto sceneSurface = cache.Acquire(scene);
    auto nextSurface = cache.Acquire(next);
    std::vector<std::shared_ptr<Surface>> surfaces{sceneSurface, nextSurface};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, next, nextSurface.get());
    MetalTests::Require(surfaces.size() == 2, "Binding a depth target after a multisampled one retired it");
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, scene, sceneSurface.get());
    MetalTests::Require(surfaces.size() == 2, "Binding a multisampled depth target retired the one after it");
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, scene.address + AgcDriver::Graphics::DepthPlaneBytes(extent, 4u, 4u) - 0x100, 0x100, nextSurface.get());
    MetalTests::Require(surfaces.size() == 1 && surfaces[0] == nextSurface, "A writer in the last fragments of a multisampled depth plane did not retire it");
}

// A color target or another depth target over a resident depth surface's planes retires it from the
// surfaces a draw samples; the cache keeps it, so binding its target again brings it back.
void retiredDepth(const AgcDriver::Metal::MetalDevice& backend) {
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    const AgcDriver::Graphics::DepthTarget target{0x400000, 0x800000, {width, height}, VK_FORMAT_D32_SFLOAT_S8_UINT, 1, 0};
    const auto depthBytes = AgcDriver::Graphics::DepthSliceBytes(target.extent, 4u);
    const auto stencilBytes = AgcDriver::Graphics::DepthSliceBytes(target.extent, 1u);
    auto surface = cache.Acquire(target);
    std::vector<std::shared_ptr<Surface>> surfaces{surface};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, target.address + depthBytes, 0x10000);
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, target.address - 0x100, 0x100);
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, target.stencilAddress + stencilBytes, 0x100);
    MetalTests::Require(surfaces.size() == 1, "A write next to a depth surface's planes retired it");
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, target.address + 0x100, 0x1000, surface.get());
    MetalTests::Require(surfaces.size() == 1, "The surface being bound retired itself");
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, target.address + 0x100, 0x1000);
    MetalTests::Require(surfaces.empty(), "A color target inside a depth plane did not retire its surface");
    MetalTests::Require(cache.Acquire(target) == surface, "A retired depth surface left the cache");
    surfaces = {surface};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, target.stencilAddress - 0x40, 0x80);
    MetalTests::Require(surfaces.empty(), "A writer over the stencil plane did not retire the surface");
    auto wide = cache.Acquire(AgcDriver::Graphics::DepthTarget{target.address, target.stencilAddress, {width * 2, height}, target.format, 1, 0});
    surfaces = {surface, wide};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, wide->Target().address, depthBytes * 2, wide.get());
    MetalTests::Require(surfaces.size() == 1 && surfaces[0] == wide, "Binding a depth target of another extent at the address kept the other surface");

    // A multisampled color target covers bytes * samples: one whose later fragments reach a depth plane retires it.
    AgcDriver::Graphics::ColorTarget color{};
    color.address = target.address - 0x1000;
    color.bytes = 0x1000;
    color.samples = 4;
    surfaces = {surface};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, color);
    MetalTests::Require(surfaces.empty(), "A multisampled color target over a depth plane in its later fragments did not retire it");
    color.samples = 1;
    surfaces = {surface};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, color);
    MetalTests::Require(surfaces.size() == 1, "A single-sample color target next to a depth plane retired it");

    // A depth target retires surfaces over its stencil plane too, and not only those at its depth address:
    // a layered draw retires per slice what each slice's planes overlap.
    const auto slice = AgcDriver::Metal::DepthArraySlice(target, 1);
    MetalTests::Require(slice.address == target.address + depthBytes && slice.stencilAddress == target.stencilAddress + stencilBytes && slice.slice == target.slice + 1,
        "A depth array slice does not start one slice past the target");
    auto sliceSurface = cache.Acquire(slice);
    auto insideDepth = cache.Acquire(AgcDriver::Graphics::DepthTarget{slice.address + 0x100, 0, {width, height}, VK_FORMAT_D32_SFLOAT, 1, 0});
    auto onStencil = cache.Acquire(AgcDriver::Graphics::DepthTarget{0x2000000, slice.stencilAddress + 0x40, {width, height}, target.format, 1, 0});
    surfaces = {surface, insideDepth, onStencil, sliceSurface};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, slice, sliceSurface.get());
    MetalTests::Require(surfaces.size() == 2 && surfaces[0] == surface && surfaces[1] == sliceSurface,
        "A depth slice did not retire the surfaces inside its depth or stencil plane, or retired its neighbour or itself");

    // A stencil-only target has no depth plane: its slices have none either, and retire nothing at low addresses.
    const AgcDriver::Graphics::DepthTarget stencilOnly{0, 0x800000, {width, height}, VK_FORMAT_S8_UINT, 1, 0};
    const auto stencilSlice = AgcDriver::Metal::DepthArraySlice(stencilOnly, 2);
    MetalTests::Require(stencilSlice.address == 0 && stencilSlice.stencilAddress == stencilOnly.stencilAddress + 2 * stencilBytes,
        "A stencil-only slice was given a depth plane");
    auto low = cache.Acquire(AgcDriver::Graphics::DepthTarget{2 * depthBytes, 0, {width, height}, VK_FORMAT_D32_SFLOAT, 1, 0});
    surfaces = {low};
    AgcDriver::Metal::RetireDepthSurfaces(surfaces, stencilSlice);
    MetalTests::Require(surfaces.size() == 1, "A stencil-only slice retired a depth surface at a fabricated depth address");
}

void memorylessAndPromotion(const AgcDriver::Metal::MetalDevice& backend) {
    AgcDriver::Metal::MetalDepthSurfaceCache cache(backend);
    AgcDriver::Graphics::DepthTarget target{0x400000, 0x800000ull, {width, height}, VK_FORMAT_D32_SFLOAT_S8_UINT, 1.0f, 0};
    target.samples = 4;
    target.memoryless = true;
    auto memorylessDepth = cache.Acquire(target);
    MetalTests::Require(memorylessDepth->DepthTexture().storageMode == MTLStorageModeMemoryless, "Memoryless depth target did not allocate memoryless storage");
    MetalTests::Require(memorylessDepth->SampledDepthView() == nil, "Memoryless depth target returned non-nil sampled view");
    MetalTests::Require(memorylessDepth->SampledStencilView() == nil, "Memoryless stencil target returned non-nil sampled view");
    MetalTests::Require(memorylessDepth->SampledDepthPlane().texture == nil, "Memoryless depth plane returned non-nil texture");
    MetalTests::Require(memorylessDepth->SampledStencilPlane().texture == nil, "Memoryless stencil plane returned non-nil texture");

    AgcDriver::Graphics::ColorTarget colorTarget{};
    colorTarget.address = 0x600000;
    colorTarget.extent = {width, height};
    colorTarget.format = VK_FORMAT_R8G8B8A8_UNORM;
    colorTarget.samples = 4;
    colorTarget.memoryless = true;
    auto memorylessColor = cache.Acquire(colorTarget);
    MetalTests::Require(memorylessColor->Texture().storageMode == MTLStorageModeMemoryless, "Memoryless color target did not allocate memoryless storage");

    AgcDriver::Graphics::DepthTarget d24Target{0x400000, 0x800000ull, {width, height}, VK_FORMAT_D24_UNORM_S8_UINT, 1, 0};
    auto d24Surface = cache.Acquire(d24Target);
    MetalTests::Require(d24Surface->DepthTexture().pixelFormat == MTLPixelFormatDepth32Float_Stencil8, "D24S8 target was not promoted to Depth32Float_Stencil8");
    MetalTests::Require(d24Surface->StencilTexture() == d24Surface->DepthTexture(), "D24S8 target did not share combined depth/stencil planes");
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
    for (const auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT}) depthArray(context, backend, shaders, format);
    for (const auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT}) layeredDepthArray(context, backend, shaders, format);
    replacedDepth(backend);
    for (const auto format : {VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) sharedPlanes(context, backend, shaders, format);
    retiredDepth(backend);
    multisampledDepthPlane(backend);
    memorylessAndPromotion(backend);
}
