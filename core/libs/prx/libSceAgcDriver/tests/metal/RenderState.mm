#import <Foundation/Foundation.h>
#include "MetalRenderState.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using namespace AgcDriver;
using Pixel = std::array<std::uint8_t, 4>;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Graphics::State BasicState() {
    Graphics::State state{};
    state.stages.path = Graphics::ShaderPath::Vertex;
    state.viewport = {0, 0, 8, 8, 0, 1};
    state.scissor = {{0, 0}, {8, 8}};
    state.renderExtent = {8, 8};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    Graphics::ColorTarget color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.exportIndex = 3;
    state.colors = {color};
    state.hasColorTarget = true;
    state.blends.resize(4);
    state.blends[3].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return state;
}

struct alignas(16) Parameters {
    std::array<float, 4> color;
    float depth;
    std::uint32_t shape = 0;
    std::uint32_t reverse = 0;
};
static_assert(sizeof(Parameters) == 32);

struct Draw {
    Graphics::State state;
    Parameters parameters;
};

id<MTLLibrary> Library(id<MTLDevice> device) {
    NSString* source = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct Parameters { float4 color; float depth; uint shape, reverse; };
vertex float4 renderStateVertex(uint index [[vertex_id]], constant Parameters& parameters [[buffer(0)]]) {
    const float2 full[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    const float2 asymmetric[3] = {float2(-1, -1), float2(1, -1), float2(-1, 0)};
    if (parameters.reverse != 0 && index != 0) index = 3 - index;
    const float2 position = parameters.shape == 0 ? full[index] : asymmetric[index];
    return float4(position.x, -position.y, parameters.depth, 1);
}
struct Fragment { float4 color [[color(3)]]; };
fragment Fragment renderStateFragment(constant Parameters& parameters [[buffer(0)]]) {
    return {parameters.color};
}
)metal";
    NSError* error = nil;
    auto library = [device newLibraryWithSource:source options:nil error:&error];
    if (library == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Render state fixture compilation failed");
    return library;
}

std::array<Pixel, 64> Render(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library,
                           std::span<const Draw> draws, MTLClearColor clearColor) {
    Require(!draws.empty(), "Render state fixture requires a draw");
    const auto& state = draws.front().state;
    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, state);
    descriptor.vertexFunction = [library newFunctionWithName:@"renderStateVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"renderStateFragment"];
    NSError* error = nil;
    auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Render state fixture pipeline creation failed");
    auto colorDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    colorDescriptor.storageMode = MTLStorageModeShared;
    colorDescriptor.usage = MTLTextureUsageRenderTarget;
    auto color = [device newTextureWithDescriptor:colorDescriptor];
    Require(color != nil, "Render state fixture color allocation failed");
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[3].texture = color;
    pass.colorAttachments[3].loadAction = MTLLoadActionClear;
    pass.colorAttachments[3].storeAction = MTLStoreActionStore;
    pass.colorAttachments[3].clearColor = clearColor;
    if (state.depth) {
        auto depthDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:Metal::RenderPixelFormat(state.depth->format)
                                                                               width:8 height:8 mipmapped:NO];
        depthDescriptor.storageMode = MTLStorageModePrivate;
        depthDescriptor.usage = MTLTextureUsageRenderTarget;
        auto depth = [device newTextureWithDescriptor:depthDescriptor];
        Require(depth != nil, "Render state fixture depth allocation failed");
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 0.5;
        if (state.depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            pass.stencilAttachment.texture = depth;
            pass.stencilAttachment.loadAction = MTLLoadActionClear;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = 7;
        }
    }
    auto commands = [queue commandBuffer];
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    Require(encoder != nil, "Render state fixture encoder allocation failed");
    [encoder setRenderPipelineState:pipeline];
    for (const auto& draw : draws) {
        const auto depthStencil = Metal::CreateDepthStencilState(device, draw.state);
        Metal::BindRenderState(encoder, draw.state, depthStencil);
        [encoder setVertexBytes:&draw.parameters length:sizeof(draw.parameters) atIndex:0];
        [encoder setFragmentBytes:&draw.parameters length:sizeof(draw.parameters) atIndex:0];
        [encoder drawPrimitives:Metal::PrimitiveType(draw.state) vertexStart:0 vertexCount:3];
    }
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) throw std::runtime_error(commands.error.localizedDescription.UTF8String ?: "Render state GPU command failed");
    std::array<Pixel, 64> result{};
    [color getBytes:result.data() bytesPerRow:8 * sizeof(Pixel) fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    return result;
}

void Match(Pixel actual, Pixel expected, const char* message, int tolerance = 0) {
    for (std::size_t channel = 0; channel < actual.size(); ++channel) {
        Require(std::abs(static_cast<int>(actual[channel]) - expected[channel]) <= tolerance, message);
    }
}

void BlendScissor(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    auto state = BasicState();
    state.scissor = {{2, 1}, {3, 2}};
    auto& blend = state.blends[3];
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_B_BIT;
    state.blendConstants = {0.25f, 0.5f, 0.75f, 0.25f};
    const std::array<Draw, 1> draws{{{state, {{1, 0.5f, 0.25f, 0.5f}, 0.25f}}}};
    const auto pixels = Render(device, queue, library, draws, MTLClearColorMake(0.25, 0.5, 0.75, 1));
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const bool inside = x >= 2 && x < 5 && y >= 1 && y < 3;
            Match(pixels[y * 8 + x], inside ? Pixel{112, 128, 96, 255} : Pixel{64, 128, 191, 255},
                  "Sparse export blending, write mask or scissor changed expected pixels", 1);
        }
    }
}

void DepthStencil(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    auto state = BasicState();
    Graphics::DepthTarget depth{};
    depth.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    state.depth = depth;
    state.depthTest = true;
    state.depthWrite = true;
    state.depthCompare = VK_COMPARE_OP_LESS;
    state.stencilTest = true;
    state.stencilFront = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 255, 255, 7};
    state.stencilBack = state.stencilFront;
    auto referenceEight = state;
    referenceEight.stencilFront.reference = 8;
    referenceEight.stencilBack.reference = 8;
    const std::array<Draw, 4> draws{{
        {state, {{0, 0, 1, 1}, 0.75f}},
        {state, {{1, 0, 0, 1}, 0.25f}},
        {state, {{0, 1, 0, 1}, 0.1f}},
        {referenceEight, {{0, 0, 1, 1}, 0.4f}}
    }};
    for (const auto pixel : Render(device, queue, library, draws, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {255, 0, 0, 255}, "Depth ordering/write or stencil increment/reference changed expected pixels");
    }
    state.stencilTest = false;
    auto disabled = state;
    disabled.depthTest = false;
    disabled.depthWrite = true;
    const std::array<Draw, 2> disabledTest{{{disabled, {{0, 0, 1, 1}, 0.1f}}, {state, {{1, 0, 0, 1}, 0.25f}}}};
    for (const auto pixel : Render(device, queue, library, disabledTest, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {255, 0, 0, 255}, "Disabled Vulkan depth testing incorrectly wrote depth through the native always-pass comparison");
    }
}

void ViewportWinding(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    for (const auto frontFace : {VK_FRONT_FACE_CLOCKWISE, VK_FRONT_FACE_COUNTER_CLOCKWISE}) {
        for (const auto cull : {VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT}) {
            for (const auto reverse : {0u, 1u}) {
                auto state = BasicState();
                state.viewport = {1, 2, 4, 4, 0, 1};
                state.frontFace = frontFace;
                state.cullMode = cull;
                const bool front = (frontFace == VK_FRONT_FACE_CLOCKWISE) == (reverse == 0);
                const bool visible = cull == VK_CULL_MODE_BACK_BIT ? front : !front;
                const std::array<Draw, 1> draws{{{state, {{1, 0, 0, 1}, 0.25f, 1, reverse}}}};
                const auto pixels = Render(device, queue, library, draws, MTLClearColorMake(0, 0, 0, 1));
                for (std::uint32_t y = 0; y < 8; ++y) {
                    for (std::uint32_t x = 0; x < 8; ++x) {
                        const bool covered = y == 2 ? x >= 1 && x <= 3 : y == 3 && x == 1;
                        Match(pixels[y * 8 + x], visible && covered ? Pixel{255, 0, 0, 255} : Pixel{0, 0, 0, 255},
                              "Positive Vulkan viewport coordinates or front/back winding changed expected pixels");
                    }
                }
            }
        }
    }
}

}

int main() {
    @autoreleasepool {
        try {
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "Render state tests require a Metal device");
            auto queue = [device newCommandQueue];
            Require(queue != nil, "Render state test queue creation failed");
            const auto library = Library(device);
            BlendScissor(device, queue, library);
            DepthStencil(device, queue, library);
            ViewportWinding(device, queue, library);
            std::cout << "PASS: original State sparse exports, blending/write masks/scissor, depth/stencil, disabled depth writes, viewport and winding pixels\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
