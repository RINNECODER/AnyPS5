#import <Foundation/Foundation.h>
#include "MetalRenderState.hpp"
#include "MetalPipelineCache.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
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
    std::uint32_t flags = 1;
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
struct Parameters { float4 color; float depth; uint shape, reverse, flags; };
vertex float4 renderStateVertex(uint index [[vertex_id]], constant Parameters& parameters [[buffer(0)]]) {
    const float2 full[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    const float2 asymmetric[3] = {float2(-1, -1), float2(1, -1), float2(-1, 0)};
    if (parameters.reverse != 0 && index != 0) index = 3 - index;
    const float2 position = parameters.shape == 0 ? full[index] : asymmetric[index];
    const float y = (parameters.flags & 1) != 0 ? -position.y : position.y;
    const float z = (parameters.flags & 2) != 0 ? (parameters.depth + 1) * 0.5 : parameters.depth;
    return float4(position.x, y, z, 1);
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
        if (state.depth->format == VK_FORMAT_D16_UNORM_S8_UINT || state.depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
            id<MTLTexture> stencil = depth;
            if (state.depth->format == VK_FORMAT_D16_UNORM_S8_UINT) {
                auto stencilDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:8 height:8 mipmapped:NO];
                stencilDescriptor.storageMode = MTLStorageModePrivate;
                stencilDescriptor.usage = MTLTextureUsageRenderTarget;
                stencil = [device newTextureWithDescriptor:stencilDescriptor];
                Require(stencil != nil, "Render state fixture separate stencil allocation failed");
            }
            pass.stencilAttachment.texture = stencil;
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
        auto parameters = draw.parameters;
        parameters.flags = (draw.state.viewport.height > 0 ? 1u : 0u) | (draw.state.negativeOneToOne ? 2u : 0u);
        [encoder setVertexBytes:&parameters length:sizeof(parameters) atIndex:0];
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
    for (const auto format : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT}) {
        auto state = BasicState();
        Graphics::DepthTarget depth{};
        depth.format = format;
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
}

void ViewportWinding(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    for (const auto negativeHeight : {false, true}) {
        for (const auto frontFace : {VK_FRONT_FACE_CLOCKWISE, VK_FRONT_FACE_COUNTER_CLOCKWISE}) {
            for (const auto cull : {VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT}) {
                for (const auto reverse : {0u, 1u}) {
                    auto state = BasicState();
                    state.viewport = negativeHeight ? VkViewport{1, 6, 4, -4, 0, 1} : VkViewport{1, 2, 4, 4, 0, 1};
                    state.frontFace = frontFace;
                    state.cullMode = cull;
                    const bool front = (frontFace == VK_FRONT_FACE_CLOCKWISE) == ((reverse == 0) != negativeHeight);
                    const bool visible = cull == VK_CULL_MODE_BACK_BIT ? front : !front;
                    const std::array<Draw, 1> draws{{{state, {{1, 0, 0, 1}, 0.25f, 1, reverse}}}};
                    const auto pixels = Render(device, queue, library, draws, MTLClearColorMake(0, 0, 0, 1));
                    for (std::uint32_t y = 0; y < 8; ++y) {
                        for (std::uint32_t x = 0; x < 8; ++x) {
                            const auto relativeY = negativeHeight ? 7 - y : y;
                            const bool covered = relativeY == 2 ? x >= 1 && x <= 3 : relativeY == 3 && x == 1;
                            Match(pixels[y * 8 + x], visible && covered ? Pixel{255, 0, 0, 255} : Pixel{0, 0, 0, 255},
                                  "Vulkan viewport coordinates or front/back winding changed expected pixels");
                        }
                    }
                }
            }
        }
    }
}

// A guest viewport transform may map depth outside zero-to-one (a [-1, 1] clip-space
// title with no depth target); Metal takes such a range, so the draw renders.
void UnrestrictedViewportDepth(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    auto state = BasicState();
    state.negativeOneToOne = true;
    state.viewport.minDepth = -1;
    state.viewport.maxDepth = 1;
    const std::array<Draw, 1> draws{{{state, {{1, 0, 0, 1}, 0.25f}}}};
    for (const auto pixel : Render(device, queue, library, draws, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {255, 0, 0, 255}, "Viewport depth outside zero-to-one without a depth target did not render");
    }
}

void ClipDepth(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    auto state = BasicState();
    Graphics::DepthTarget depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    state.depth = depth;
    state.depthTest = true;
    state.depthWrite = true;
    state.depthCompare = VK_COMPARE_OP_LESS;
    for (const auto negativeOneToOne : {false, true}) {
        state.negativeOneToOne = negativeOneToOne;
        for (const auto depthValue : {-1.25f, -0.5f, 0.5f}) {
            const std::array<Draw, 1> draws{{{state, {{1, 0, 0, 1}, depthValue}}}};
            const bool visible = negativeOneToOne && depthValue == -0.5f;
            for (const auto pixel : Render(device, queue, library, draws, MTLClearColorMake(0, 0, 0, 1))) {
                Match(pixel, visible ? Pixel{255, 0, 0, 255} : Pixel{0, 0, 0, 255},
                      "Vulkan clip-depth range or transformed depth test changed expected pixels");
            }
        }
    }
}

void DepthBounds(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    bool supported = false;
    if (@available(macOS 26.0, *)) supported = [device supportsFamily:MTLGPUFamilyApple10];
    if (!supported) return;
    auto state = BasicState();
    Graphics::DepthTarget depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    state.depth = depth;
    state.depthBoundsTest = true;
    state.minDepthBounds = 0.1f;
    state.maxDepthBounds = 0.2f;
    const std::array<Draw, 1> rejected{{{state, {{1, 0, 0, 1}, 0.15f}}}};
    for (const auto pixel : Render(device, queue, library, rejected, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {0, 0, 0, 255}, "Depth bounds used incoming depth or ignored the stored depth bound");
    }
    state.minDepthBounds = 0.5f;
    state.maxDepthBounds = 0.5f;
    const std::array<Draw, 1> accepted{{{state, {{1, 0, 0, 1}, 0.9f}}}};
    for (const auto pixel : Render(device, queue, library, accepted, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {255, 0, 0, 255}, "Depth bounds rejected stored depth inside the inclusive range");
    }
    state.minDepthBounds = 0.1f;
    state.maxDepthBounds = 0.2f;
    auto disabled = state;
    disabled.depthBoundsTest = false;
    const std::array<Draw, 2> reset{{{state, {{1, 0, 0, 1}, 0.15f}}, {disabled, {{0, 1, 0, 1}, 0.9f}}}};
    for (const auto pixel : Render(device, queue, library, reset, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {0, 255, 0, 255}, "Disabled depth bounds leaked the preceding draw's range");
    }
}

void DepthBiasReset(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    // Decode a guest draw without writing PA_SU_POLY_OFFSET_CLAMP (0x2df).
    alignas(256) std::array<std::byte, 2048> colorMemory{};
    alignas(256) std::array<std::byte, 256> depthMemory{};
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(colorMemory.data());
    const auto depthAddress = reinterpret_cast<std::uintptr_t>(depthMemory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 2> ranges{{
        {colorAddress, colorMemory, true}, {depthAddress, depthMemory, true}
    }};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    const auto configure = [&](QueueState& guest) {
        auto& cx = guest.context;
        guest.userConfig[0x242] = 4;
        cx[0x2d5] = 0x2000;
        cx[0x204] = 0x80000;
        cx[0x205] = 0x1800;
        cx[0x200] = 0x12; // Depth test LESS, with depth writes disabled.
        cx[0x010] = 1; // D16, without stencil.
        cx[0x011] = 0;
        cx[0x007] = (7u << 16u) | 7u;
        for (const auto offset : {0x012u, 0x014u}) cx[offset] = static_cast<std::uint32_t>(depthAddress >> 8u);
        for (const auto offset : {0x01au, 0x01cu}) cx[offset] = static_cast<std::uint32_t>(depthAddress >> 40u);
        for (const auto offset : {0x2e0u, 0x2e2u}) cx[offset] = 0;
        for (const auto offset : {0x2e1u, 0x2e3u}) cx[offset] = std::bit_cast<std::uint32_t>(16384.0f);
        cx[0x8e] = 0xf000;
        cx[0x8f] = 0xffff;
        cx[0x1c3] = 4;
        cx[0x1c5] = 0x9000;
        constexpr auto stride = 3u * 0xfu;
        cx[0x318 + stride] = static_cast<std::uint32_t>(colorAddress >> 8u);
        cx[0x390 + 3] = static_cast<std::uint32_t>(colorAddress >> 40u);
        cx[0x31b + stride] = 0;
        cx[0x31c + stride] = 0x8028; // RGBA8 UNORM, clamped.
        cx[0x31d + stride] = 0;
        cx[0x3b0 + 3] = (7u << 14u) | 7u;
        cx[0x3b8 + 3] = 0x09000000;
        for (const auto offset : {0x10fu, 0x110u, 0x111u, 0x112u}) cx[offset] = std::bit_cast<std::uint32_t>(4.0f);
        cx[0x113] = std::bit_cast<std::uint32_t>(1.0f);
        cx[0x114] = 0;
        cx[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    };
    QueueState guest;
    configure(guest);
    const auto initial = Graphics::DecodeState(guest);
    Require(initial.depthBias && initial.depthBiasConstant == 16384.0f && initial.depthBiasSlope == 0 &&
            initial.depthBiasClamp == 0, "Initial depth-biased draw did not decode an unclamped default");
    guest.context[0x2df] = std::bit_cast<std::uint32_t>(0.03125f);
    const auto explicitClamp = Graphics::DecodeState(guest);
    Require(explicitClamp.depthBias && explicitClamp.depthBiasClamp == 0.03125f,
            "Guest-written nonzero depth-bias clamp did not decode");
    guest.ClearContext();
    configure(guest);
    const auto reset = Graphics::DecodeState(guest);
    Require(reset.depthBias && reset.depthBiasConstant == 16384.0f && reset.depthBiasClamp == 0,
            "Context reset did not restore the unclamped depth-biased draw");
    guest.context[0x205] = 0;
    const auto disabled = Graphics::DecodeState(guest);
    Require(!disabled.depthBias, "Depth bias remained enabled in the decoded control");
    Require(disabled.depth->slice == 0 && disabled.depth->lastSlice == 0 && !disabled.layered, "A plain depth view decoded as an array slice range");
    // DB_DEPTH_VIEW SLICE_START 2, SLICE_MAX 5 and a render target index export: a layered draw into
    // slices 2..5 whose depth address starts at slice 2.
    guest.context[0x002] = 2u | (5u << 13u);
    guest.context[0x207] = 1u << 18u;
    const auto layered = Graphics::DecodeState(guest);
    Require(layered.layered && layered.depth->slice == 2 && layered.depth->lastSlice == 5 &&
            layered.depth->address == depthAddress + 2 * Graphics::DepthSliceBytes({8, 8}, 2), "A layered depth array view did not decode its slice range");
    guest.context[0x002] = 3u;
    guest.context[0x207] = 0;
    const auto single = Graphics::DecodeState(guest);
    Require(!single.layered && single.depth->slice == 3 && single.depth->lastSlice == 3, "A one-slice depth view did not keep its slice as the last one");
    guest.context[0x002] = 0;

    // D16 bias of 16384 units adds approximately 0.25 to depth 0.375: LESS than
    // cleared 0.5 fails. A 0.03125 clamp admits the triangle; no bias also admits it.
    const std::array<Graphics::State, 4> states{initial, explicitClamp, reset, disabled};
    const std::array<Pixel, 4> expected{{{0, 0, 0, 255}, {255, 0, 0, 255}, {0, 0, 0, 255}, {0, 255, 0, 255}}};
    for (std::size_t i = 0; i < states.size(); ++i) {
        const std::array<Draw, 1> draws{{{states[i], {i == 3 ? std::array<float, 4>{0, 1, 0, 1} : std::array<float, 4>{1, 0, 0, 1}, 0.375f}}}};
        for (const auto pixel : Render(device, queue, library, draws, MTLClearColorMake(0, 0, 0, 1))) {
            Match(pixel, expected[i], "Decoded initial/explicit/reset/disabled depth bias changed expected native pixels");
        }
    }
    const std::array<Draw, 2> transition{{{explicitClamp, {{1, 0, 0, 1}, 0.375f}}, {reset, {{0, 1, 0, 1}, 0.375f}}}};
    for (const auto pixel : Render(device, queue, library, transition, MTLClearColorMake(0, 0, 0, 1))) {
        Match(pixel, {255, 0, 0, 255}, "Reset depth-bias clamp leaked the preceding draw's explicit clamp");
    }
    std::cout << "PASS: decoded depth-bias clamp initial=0 explicit=0.03125 reset=0; native 64-pixel outputs initial=black explicit=red reset=black disabled=green transition=red\n";
}

}

constexpr auto GuestColorStride = 3u * 0xfu;

// A guest draw of one RGBA8 color target at MRT slot 3 (8x8 at colorAddress), viewport and scissor
// covering it, no depth, no blending.
QueueState GuestDraw(std::uintptr_t colorAddress) {
    constexpr auto stride = GuestColorStride;
    QueueState guest;
    auto& cx = guest.context;
    guest.userConfig[0x242] = 4;
    cx[0x2d5] = 0x2000;
    cx[0x204] = 0x80000;
    cx[0x205] = 0;
    cx[0x200] = 0;
    cx[0x010] = 0;
    cx[0x011] = 0;
    cx[0x8e] = 0xf000;
    cx[0x8f] = 0xffff;
    cx[0x1c3] = 4;
    cx[0x1c5] = 0x9000;
    cx[0x318 + stride] = static_cast<std::uint32_t>(colorAddress >> 8u);
    cx[0x390 + 3] = static_cast<std::uint32_t>(colorAddress >> 40u);
    cx[0x31b + stride] = 0;
    cx[0x31c + stride] = 0x8028; // RGBA8 UNORM, clamped.
    cx[0x31d + stride] = 0;
    cx[0x3b0 + 3] = (7u << 14u) | 7u;
    cx[0x3b8 + 3] = 0x09000000;
    for (const auto offset : {0x10fu, 0x110u, 0x111u, 0x112u}) cx[offset] = std::bit_cast<std::uint32_t>(4.0f);
    cx[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    cx[0x114] = 0;
    cx[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    for (std::uint32_t constant = 0x105; constant < 0x109; ++constant) cx[constant] = 0;
    return guest;
}

// Shared State decode fixes ported from upstream, checked on the Metal path: CB_COLOR_INFO BLEND_BYPASS
// turns blending off (upstream PR 4862), and MSAA_ENABLE on a single-sample scan whose samples sit at
// the pixel center rasterizes like a plain single-sample draw (upstream fdfbeba8a).
void SharedStateDecode(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    alignas(256) std::array<std::byte, 2048> colorMemory{};
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(colorMemory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{colorAddress, colorMemory, true}}};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    constexpr auto stride = GuestColorStride;
    auto guest = GuestDraw(colorAddress);
    auto& cx = guest.context;
    // SRC_ALPHA, ADD, ONE_MINUS_SRC_ALPHA for color and alpha.
    cx[0x1e0 + 3] = (1u << 30u) | 4u | (5u << 8u);

    const auto blended = Graphics::DecodeState(guest);
    Require(blended.blends.at(3).blendEnable, "Enabled blending was not decoded");
    cx[0x31c + stride] |= 0x10000u;
    const auto bypassed = Graphics::DecodeState(guest);
    Require(!bypassed.blends.at(3).blendEnable && bypassed.blends.at(3).colorWriteMask != 0, "A target with BLEND_BYPASS still blends");
    const std::array<float, 4> halfRed{1, 0, 0, 0.5f};
    const std::array<Draw, 1> blendDraw{{{blended, {halfRed, 0.5f}}}};
    for (const auto pixel : Render(device, queue, library, blendDraw, MTLClearColorMake(0, 1, 0, 1))) {
        Match(pixel, {128, 128, 0, 191}, "Enabled blending did not mix the draw over the clear color", 2);
    }
    const std::array<Draw, 1> bypassDraw{{{bypassed, {halfRed, 0.5f}}}};
    for (const auto pixel : Render(device, queue, library, bypassDraw, MTLClearColorMake(0, 1, 0, 1))) {
        Match(pixel, {255, 0, 0, 128}, "BLEND_BYPASS did not write the shader color unblended", 2);
    }
    cx[0x1e0 + 3] = 0;
    cx[0x31c + stride] &= ~0x10000u;

    const auto plain = Graphics::DecodeState(guest);
    const auto plainRejection = Graphics::DrawRejection(guest, false);
    cx[0x292] = 1; // MSAA_ENABLE on a single-sample scan.
    for (std::uint32_t pixel = 0; pixel < 4; ++pixel) cx[0x2fe + pixel * 4u] = 0;
    const auto centered = Graphics::DecodeState(guest);
    Require(centered.rasterSamples == 1 && centered.colors.size() == 1 && centered.colors[0].samples == 1, "MSAA_ENABLE at the pixel center did not decode as single-sampled");
    Require(Graphics::DrawRejection(guest, false) == plainRejection, "MSAA_ENABLE at the pixel center changed the draw precheck");
    const std::array<Draw, 1> plainDraw{{{plain, {{0, 0, 1, 1}, 0.5f}}}};
    const std::array<Draw, 1> centeredDraw{{{centered, {{0, 0, 1, 1}, 0.5f}}}};
    const auto plainPixels = Render(device, queue, library, plainDraw, MTLClearColorMake(0, 0, 0, 1));
    const auto centeredPixels = Render(device, queue, library, centeredDraw, MTLClearColorMake(0, 0, 0, 1));
    Require(plainPixels == centeredPixels, "MSAA_ENABLE at the pixel center changed the rendered pixels");
    for (std::uint32_t pixel = 0; pixel < 4; ++pixel) {
        cx[0x2fe + pixel * 4u] = 0x04;
        bool refused = false;
        try {
            static_cast<void>(Graphics::DecodeState(guest));
        } catch (const std::exception& error) {
            refused = std::string(error.what()).find("off the pixel center") != std::string::npos;
        }
        Require(refused, "MSAA_ENABLE with a sample off the pixel center was not refused");
        cx[0x2fe + pixel * 4u] = 0;
    }
    std::cout << "PASS: BLEND_BYPASS disables decoded blending (native blend=128,128,0,191 bypass=255,0,0,128); MSAA_ENABLE single-sample at the pixel center renders as single-sample, off-center refused\n";
}

// CB_COLOR_VIEW SLICE_START..SLICE_MAX over several array slices, without a render target index
// export, renders into SLICE_START (upstream 1d8749d00, 257ff1e38): the decoded target is the one-slice
// view's. Layered draws into such a view and views of several 3D slices stay refused.
// Shared decode fixes ported from upstream in sync 9, checked on the Metal path's State.cpp and Pm4.cpp:
// PA_SC_AA_SAMPLE_LOCS_PIXEL_* start at their clear-state value (0), a single-sample color view of a
// higher mip decodes no CMASK fast clear, and INDEX_ATTRIBUTES_INDIRECT loads the index base, size and
// type from its 16-byte record.
void Sync9SharedDecode() {
    alignas(65536) static std::array<std::byte, 1u << 20u> memory{};
    const auto address = reinterpret_cast<std::uintptr_t>(memory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{address, memory, true}}};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    constexpr auto stride = GuestColorStride;
    {
        const QueueState fresh;
        for (std::uint32_t offset = 0x2fe; offset <= 0x30d; ++offset) {
            const auto found = fresh.context.find(offset);
            Require(found != fresh.context.end() && found->second == 0, "a sample location register does not start at its clear-state value 0");
        }
        auto guest = GuestDraw(address);
        guest.context[0x292] = 1;            // MSAA_ENABLE
        guest.context[0x2f8] = (2u << 20u) | 2u; // 4x, locations never written
        const auto state = Graphics::DecodeState(guest);
        Require(state.rasterSamples == 4 && state.customSampleLocations, "a 4x draw with unwritten sample locations did not decode as custom locations");
    }
    {
        auto guest = GuestDraw(address);
        auto& cx = guest.context;
        cx[0x3b0 + 3] |= 1u << 28u;      // two mips
        cx[0x31b + stride] = 1u << 26u;  // view mip 1
        cx[0x31c + stride] |= 0x2000u;   // FAST_CLEAR
        const auto color = Graphics::DecodeState(guest).colors.at(0);
        Require(color.cmaskAddress == 0, "a view of mip 1 of a fast-clear target decoded a CMASK");
        cx[0x31b + stride] = 0;
        bool refused = false;
        try {
            static_cast<void>(Graphics::DecodeState(guest));
        } catch (const std::exception& error) {
            refused = std::string(error.what()).find("CMASK fast clears") != std::string::npos;
        }
        Require(refused, "a fast clear of an unconfigured mip 0 target was not refused");
    }
    {
        auto* record = reinterpret_cast<std::uint32_t*>(memory.data() + 4096);
        const std::uint64_t indices = address + 8192;
        record[16 + 0] = static_cast<std::uint32_t>(indices);
        record[16 + 1] = static_cast<std::uint32_t>(indices >> 32u);
        record[16 + 2] = 36;
        record[16 + 3] = 1;
        const auto base = address + 4096;
        const std::array<std::uint32_t, 4> packet{(3u << 30u) | (2u << 16u) | (0x91u << 8u), static_cast<std::uint32_t>(base), static_cast<std::uint32_t>(base >> 32u), 4};
        QueueState queue;
        Pm4::Execute(packet, queue);
        Require(queue.indexBase == indices && queue.indexBufferSize == 36 && queue.indexType == 1, "INDEX_ATTRIBUTES_INDIRECT did not load record 4");
        record[16 + 3] = 4;
        bool refused = false;
        try {
            Pm4::Execute(packet, queue);
        } catch (const std::exception& error) {
            refused = std::string(error.what()).find("index-type modifiers") != std::string::npos;
        }
        Require(refused, "an INDEX_ATTRIBUTES_INDIRECT record with index type 4 was accepted");
    }
}

void ColorViewSlices() {
    alignas(65536) static std::array<std::byte, 1u << 20u> sliceMemory{};
    const auto address = reinterpret_cast<std::uintptr_t>(sliceMemory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{address, sliceMemory, true}}};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    constexpr auto stride = GuestColorStride;
    auto guest = GuestDraw(address);
    auto& cx = guest.context;
    const auto refused = [&](const char* reason) {
        try {
            static_cast<void>(Graphics::DecodeState(guest));
        } catch (const std::exception& error) {
            return std::string(error.what()).find(reason) != std::string::npos;
        }
        return false;
    };
    const auto plain = Graphics::DecodeState(guest).colors.at(0);
    cx[0x31b + stride] = 35u << 13u;
    const auto plainRange = Graphics::DecodeState(guest).colors.at(0);
    Require(plainRange.address == plain.address && plainRange.bytes == plain.bytes, "a view of slices 0..35 of a target with no array slices did not render into its only slice");
    cx[0x31b + stride] = 1u;
    Require(refused("ends before its first slice"), "a color view ending before its first slice was not refused");
    cx[0x3b8 + 3] = 0x09000001; // Two array slices.
    cx[0x31b + stride] = 1u | (1u << 13u);
    const auto second = Graphics::DecodeState(guest).colors.at(0);
    Require(second.address > address && second.bytes == plain.bytes, "a view of slice 1 did not move the target by one slice");
    cx[0x31b + stride] = 1u << 13u;
    const auto range = Graphics::DecodeState(guest).colors.at(0);
    Require(range.address == address && range.bytes == plain.bytes && range.arraySlice == 0, "a view of slices 0..1 did not render into slice 0");
    cx[0x31b + stride] = 1u | (1u << 13u);
    Require(Graphics::DecodeState(guest).colors.at(0).address == second.address, "a view of slices 1..1 changed after a range view");
    cx[0x31b + stride] = 1u << 14u;
    Require(refused("beyond the array surface"), "a view past the last array slice was not refused");
    cx[0x31b + stride] = 1u << 13u;
    cx[0x207] = 1u << 18u;
    Require(refused("layered draws into a color view of several array slices"), "a layered draw into a view of several slices was not refused");
    cx[0x207] = 0;
    cx[0x3b8 + 3] = 0x0a000003; // A 3D target of depth 4.
    cx[0x31b + stride] = 2u | (3u << 13u);
    Require(refused("several 3D slices"), "a view of several 3D slices was not refused");
    std::cout << "PASS: color views of several array slices render into their first slice; layered and 3D ranges refused\n";
}

// Upstream 04138f50a: clip and cull distance enables reach State::paClVsOutCntl (the Metal draw passes
// them to the vertex program); more than eight distances stay refused.
void ClipCullDecode() {
    alignas(65536) static std::array<std::byte, 1u << 20u> memory{};
    const auto address = reinterpret_cast<std::uintptr_t>(memory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{address, memory, true}}};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    auto guest = GuestDraw(address);
    const auto baseline = Graphics::DrawRejection(guest, false);
    guest.context[0x207] = 0x0040000fu;
    Require(Graphics::DecodeState(guest).paClVsOutCntl == 0x0040000fu, "four clip distances were not decoded");
    Require(Graphics::DrawRejection(guest, false) == baseline, "the precheck rejected four clip distances");
    guest.context[0x207] = 0x004001ffu;
    bool refused = false;
    try {
        static_cast<void>(Graphics::DecodeState(guest));
    } catch (const std::exception& error) {
        refused = std::string(error.what()).find("more than eight clip and cull distances") != std::string::npos;
    }
    Require(refused && Graphics::DrawRejection(guest, false).find("more than eight clip and cull distances") != std::string::npos, "nine clip and cull distances were not refused");
    std::cout << "PASS: clip and cull distance enables decode; more than eight are refused\n";
}

// Upstream 44c4b3f8b: PA_SC_AA_CONFIG COVERED_CENTROID_IS_CENTER is admitted while the pixel program
// reads no centroid input, and refused (decode and precheck) when it does.
void CoveredCentroid() {
    alignas(65536) static std::array<std::byte, 1u << 20u> memory{};
    const auto address = reinterpret_cast<std::uintptr_t>(memory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{address, memory, true}}};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    auto guest = GuestDraw(address);
    guest.shader[0x008] = 0x1000;
    guest.context[0x1b3] = 2u;
    guest.context[0x1b4] = 2u;
    const auto baseline = Graphics::DrawRejection(guest, false);
    Require(baseline.find("COVERED_CENTROID") == std::string::npos, "the base draw already mentions covered-centroid mode");
    guest.context[0x2f8] = 1u << 29u;
    Require(Graphics::DecodeState(guest).rasterSamples == 1 && Graphics::DrawRejection(guest, false) == baseline, "covered-centroid mode without centroid inputs was refused");
    const auto refused = [&] {
        try {
            static_cast<void>(Graphics::DecodeState(guest));
        } catch (const std::exception& error) {
            return std::string(error.what()).find("COVERED_CENTROID_IS_CENTER") != std::string::npos &&
                Graphics::DrawRejection(guest, false).find("COVERED_CENTROID_IS_CENTER") != std::string::npos;
        }
        return false;
    };
    for (const auto centroid : {4u, 64u}) {
        guest.context[0x1b3] = 2u | centroid;
        guest.context[0x1b4] = 2u | centroid;
        Require(refused(), "covered-centroid mode with an active centroid input was not refused");
        guest.context[0x1b4] = 2u;
        Require(Graphics::DrawRejection(guest, false) == baseline, "an unaddressed centroid input refused covered-centroid mode");
    }
    guest.context[0x1b3] = 2u;
    guest.context.erase(0x1b4);
    Require(refused(), "covered-centroid mode with unspecified centroid inputs was not refused");
    std::cout << "PASS: covered-centroid mode is admitted without centroid inputs and refused with them\n";
}

// Upstream 320bd37c9: DB_SHADER_CONTROL.DEPTH_BEFORE_SHADER and Z_ORDER 3 test depth before the pixel
// shader. On Metal a depth-exporting shader stays late ([[depth]] is not allowed with
// [[early_fragment_tests]]).
void EarlyDepthDecode() {
    Registers cx;
    // PA_SC_AA_MASK_X0Y0_X1Y0 / X0Y1_X1Y1: every sample of the quad covered (the pixel stage reads the quad mask).
    cx[0x30e] = 0xffffffffu;
    cx[0x30f] = 0xffffffffu;
    cx[0x1b3] = 2u;
    cx[0x1b4] = 2u;
    cx[0x1b6] = 0u;
    cx[0x1c5] = 0u;
    std::array<std::uint8_t, 8> exports{};
    const auto early = [&](std::uint32_t shaderControl) {
        cx[0x203] = shaderControl;
        return Graphics::DecodePixelStageInfo(cx, exports).earlyZ;
    };
    constexpr std::uint32_t depthExport = 1u, kill = 1u << 6u, maskExport = 1u << 8u, depthBeforeShader = 1u << 12u;
    Require(!early(0) && early(1u << 4u) && early(3u << 4u) && !early(2u << 4u), "Z_ORDER 1 and 3 must be early, 0 and 2 late");
    Require(!early((1u << 4u) | kill) && !early((3u << 4u) | maskExport), "a killing or mask-exporting shader without DEPTH_BEFORE_SHADER must stay late");
    Require(early(depthBeforeShader) && early(depthBeforeShader | kill) && early(depthBeforeShader | maskExport), "DEPTH_BEFORE_SHADER must test depth early");
    Require(!early(depthBeforeShader | depthExport) && !early((1u << 4u) | depthExport), "a depth-exporting shader must stay late on Metal");
    std::cout << "PASS: DEPTH_BEFORE_SHADER and Z_ORDER 3 decode as early depth; depth exports stay late\n";
}

// Upstream a96f8bd64, in the fork's shared DccClearEncoding.cpp: packed R11G11B10 float targets get a
// DCC fast-clear encoding (RGB zero or one; the format has no alpha).
void PackedFloatDccClear() {
    for (const bool alphaOnMsb : {false, true}) {
        for (const auto keys : {Graphics::DccKeys::Clear0000, Graphics::DccKeys::Clear0001, Graphics::DccKeys::Clear1110, Graphics::DccKeys::Clear1111}) {
            std::array<std::uint32_t, 4> texels{};
            texels.fill(0xdeadbeefu);
            Require(Graphics::FillDccClear(VK_FORMAT_B10G11R11_UFLOAT_PACK32, keys, alphaOnMsb, std::as_writable_bytes(std::span(texels))), "a packed float fast clear was not encoded");
            const std::uint32_t expected = keys == Graphics::DccKeys::Clear1110 || keys == Graphics::DccKeys::Clear1111 ? 0x781e03c0u : 0u;
            for (const auto texel : texels) Require(texel == expected, "a packed float DCC clear did not encode RGB zero or one");
        }
    }
    std::cout << "PASS: packed R11G11B10 float DCC fast clears encode\n";
}

// Renders one draw of `state` (rasterSamples > 1) into a multisampled RGBA8 target with the pass's
// sample positions from ConfigureSamplePositions, and returns the resolved pixels.
std::array<Pixel, 64> RenderResolved(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library,
                                     const Graphics::State& state, Parameters parameters) {
    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, state);
    descriptor.vertexFunction = [library newFunctionWithName:@"renderStateVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"renderStateFragment"];
    NSError* error = nil;
    auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Multisampled fixture pipeline creation failed");
    auto msDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    msDescriptor.textureType = MTLTextureType2DMultisample;
    msDescriptor.sampleCount = state.rasterSamples;
    msDescriptor.storageMode = MTLStorageModePrivate;
    msDescriptor.usage = MTLTextureUsageRenderTarget;
    auto multisampled = [device newTextureWithDescriptor:msDescriptor];
    auto resolveDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    resolveDescriptor.storageMode = MTLStorageModeShared;
    resolveDescriptor.usage = MTLTextureUsageRenderTarget;
    auto resolved = [device newTextureWithDescriptor:resolveDescriptor];
    Require(multisampled != nil && resolved != nil, "Multisampled fixture allocation failed");
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[3].texture = multisampled;
    pass.colorAttachments[3].resolveTexture = resolved;
    pass.colorAttachments[3].loadAction = MTLLoadActionClear;
    pass.colorAttachments[3].storeAction = MTLStoreActionMultisampleResolve;
    pass.colorAttachments[3].clearColor = MTLClearColorMake(0, 0, 0, 1);
    Metal::ConfigureSamplePositions(pass, device, state);
    auto commands = [queue commandBuffer];
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    Require(encoder != nil, "Multisampled fixture encoder allocation failed");
    [encoder setRenderPipelineState:pipeline];
    Metal::BindRenderState(encoder, state, Metal::CreateDepthStencilState(device, state));
    parameters.flags = state.viewport.height > 0 ? 1u : 0u;
    [encoder setVertexBytes:&parameters length:sizeof(parameters) atIndex:0];
    [encoder setFragmentBytes:&parameters length:sizeof(parameters) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) throw std::runtime_error(commands.error.localizedDescription.UTF8String ?: "Multisampled fixture GPU command failed");
    std::array<Pixel, 64> result{};
    [resolved getBytes:result.data() bytesPerRow:8 * sizeof(Pixel) fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    return result;
}

// PA_SC_AA_SAMPLE_LOCS on the Metal path (upstream c9e48e5ae, ae6dfd907): the standard 2x/4x/8x patterns,
// 8x's last sample at (7, -7) included, decode as standard and equal Metal's default positions; a
// uniform custom pattern is rasterized at its positions through programmable sample positions; a
// pattern varying over the 2x2 pixel grid, or a custom one on a pass whose attachments fix another
// sample count, is refused. Multisampling with MSAA_ENABLE clear rasterizes every sample at the pixel
// center, covering what a single-sample draw covers.
void CustomSampleLocations(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLLibrary> library) {
    alignas(256) std::array<std::byte, 2048> colorMemory{};
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(colorMemory.data());
    const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{colorAddress, colorMemory, true}}};
    const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
    auto guest = GuestDraw(colorAddress);
    auto& cx = guest.context;
    cx[0x292] = 1; // MSAA_ENABLE
    const auto setLocations = [&](std::uint32_t low, std::uint32_t high) {
        for (std::uint32_t pixel = 0; pixel < 4; ++pixel) {
            cx[0x2fe + pixel * 4u] = low;
            cx[0x2ff + pixel * 4u] = high;
            cx[0x300 + pixel * 4u] = 0;
            cx[0x301 + pixel * 4u] = 0;
        }
    };
    const auto refusal = [&](const char* expected) {
        try {
            static_cast<void>(Graphics::DecodeState(guest));
        } catch (const std::exception& error) {
            return std::string(error.what()).find(expected) != std::string::npos;
        }
        return false;
    };
    struct Standard {
        std::uint32_t log2, low, high;
    };
    for (const auto& standard : {Standard{1, 0xcc44u, 0}, Standard{2, 0x622ae6aeu, 0}, Standard{3, 0xbd153fd1u, 0x9773f95bu}}) {
        const auto samples = 1u << standard.log2;
        cx[0x2f8] = (standard.log2 << 20u) | standard.log2;
        setLocations(standard.low, standard.high);
        const auto state = Graphics::DecodeState(guest);
        Require(state.rasterSamples == samples && !state.customSampleLocations, "a standard sample pattern decoded as custom");
        const auto locations = Graphics::SampleLocations(state);
        std::array<MTLSamplePosition, 8> defaults{};
        [device getDefaultSamplePositions:defaults.data() count:samples];
        for (std::uint32_t sample = 0; sample < samples; ++sample) {
            Require(locations[sample].x == defaults[sample].x && locations[sample].y == defaults[sample].y, "a standard sample location differs from Metal's default position");
        }
    }

    cx[0x2f8] = 0x00200002; // 4x
    cx[0x31d + GuestColorStride] = 0; // one fragment: the pass rasterizes at PA_SC_AA_CONFIG's 4 samples
    setLocations(0x622ae6aeu, 0);
    const auto standard = Graphics::DecodeState(guest);
    setLocations(0x88888888u, 0); // every sample at (-8, -8): the pixel's top-left corner
    const auto topLeft = Graphics::DecodeState(guest);
    setLocations(0x77777777u, 0); // every sample at (7, 7)
    const auto bottomRight = Graphics::DecodeState(guest);
    Require(topLeft.customSampleLocations && bottomRight.customSampleLocations && topLeft.rasterSamples == 4, "a uniform custom pattern did not decode as custom");
    const auto corner = Graphics::SampleLocations(bottomRight);
    Require(corner[3].x == 15.0f / 16.0f && corner[3].y == 15.0f / 16.0f && Graphics::SampleLocations(topLeft)[0].x == 0.0f, "custom sample coordinates changed");

    const Parameters edge{{1, 1, 1, 1}, 0.5f, 1};
    const auto partial = [](const std::array<Pixel, 64>& pixels) {
        return std::count_if(pixels.begin(), pixels.end(), [](Pixel pixel) { return pixel[0] != 0 && pixel[0] != 255; });
    };
    const auto covered = [](const std::array<Pixel, 64>& pixels) {
        return std::count_if(pixels.begin(), pixels.end(), [](Pixel pixel) { return pixel[0] == 255; });
    };
    const auto standardPixels = RenderResolved(device, queue, library, standard, edge);
    const auto topLeftPixels = RenderResolved(device, queue, library, topLeft, edge);
    const auto bottomRightPixels = RenderResolved(device, queue, library, bottomRight, edge);
    Require(partial(standardPixels) > 0, "the standard 4x pattern left no partly covered edge pixel");
    Require(partial(topLeftPixels) == 0 && partial(bottomRightPixels) == 0, "samples at one custom position still gave partial coverage");
    Require(covered(topLeftPixels) != covered(bottomRightPixels), "custom sample positions did not move the covered pixels");

    cx[0x292] = 0; // MSAA_ENABLE clear: every sample at the pixel center, whatever the pattern registers say
    setLocations(0x622ae6aeu, 0);
    const auto centered = Graphics::DecodeState(guest);
    cx[0x292] = 1;
    Require(centered.customSampleLocations && centered.rasterSamples == 4 && Graphics::SampleLocations(centered)[2].x == 0.5f && Graphics::SampleLocations(centered)[2].y == 0.5f, "multisampling without MSAA_ENABLE did not decode as centered samples");
    const auto centeredPixels = RenderResolved(device, queue, library, centered, edge);
    auto single = standard;
    single.rasterSamples = 1;
    single.customSampleLocations = false;
    const std::array<Draw, 1> singleDraw{{{single, edge}}};
    const auto singlePixels = Render(device, queue, library, singleDraw, MTLClearColorMake(0, 0, 0, 1));
    Require(partial(centeredPixels) == 0 && covered(centeredPixels) == covered(singlePixels), "multisampling without MSAA_ENABLE did not cover the pixels a single-sample draw covers");

    setLocations(0x88888888u, 0);
    cx[0x30a] = 0x77777777u; // pixel (1, 1) differs
    Require(refusal("varying between pixels"), "a sample pattern varying over the pixel grid was not refused");
    setLocations(0x88888888u, 0);
    cx[0x31d + GuestColorStride] = (1u << 12u) | (1u << 15u); // a 2-fragment target fixes a 2-sample pass
    Require(refusal("fix another sample count"), "a custom pattern on a pass of another sample count was not refused");
    std::cout << "PASS: standard 2x/4x/8x sample patterns equal Metal defaults; custom 4x positions (partial edge pixels standard="
              << partial(standardPixels) << ", covered top-left=" << covered(topLeftPixels) << " bottom-right=" << covered(bottomRightPixels)
              << "); MSAA_ENABLE clear renders as single-sample coverage (" << covered(centeredPixels) << " pixels); varying or mismatched patterns refused\n";
}

// Depth/stencil states are shared by draws whose native descriptor matches; the dynamic stencil reference
// is encoder state and must not split them, while any compare, write or stencil operation change must.
void DepthStencilCache(id<MTLDevice> device) {
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
    const auto first = Metal::CreateDepthStencilState(device, state);
    auto reference = state;
    reference.stencilFront.reference = 8;
    Require(Metal::CreateDepthStencilState(device, reference) == first, "a stencil reference change rebuilt the depth/stencil state");
    auto compare = state;
    compare.depthCompare = VK_COMPARE_OP_GREATER;
    auto write = state;
    write.depthWrite = false;
    auto stencil = state;
    stencil.stencilBack.passOp = VK_STENCIL_OP_REPLACE;
    auto mask = state;
    mask.stencilFront.writeMask = 15;
    for (const auto& variant : {compare, write, stencil, mask}) {
        const auto native = Metal::CreateDepthStencilState(device, variant);
        Require(native != first && Metal::CreateDepthStencilState(device, variant) == native,
                "depth/stencil variants shared a state, or an identical variant was rebuilt");
    }
}

void DepthPromotionAndState(id<MTLDevice> device) {
    Require(Metal::RenderPixelFormat(VK_FORMAT_D24_UNORM_S8_UINT) == MTLPixelFormatDepth32Float_Stencil8,
            "RenderPixelFormat did not promote D24S8 to Depth32Float_Stencil8");
    auto state = BasicState();
    Graphics::DepthTarget depth{};
    depth.format = VK_FORMAT_D24_UNORM_S8_UINT;
    state.depth = depth;
    state.depthTest = true;
    state.depthWrite = true;
    state.stencilTest = true;
    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, state);
    Require(descriptor.depthAttachmentPixelFormat == MTLPixelFormatDepth32Float_Stencil8,
            "Pipeline depth format did not promote D24S8 to Depth32Float_Stencil8");
    Require(descriptor.stencilAttachmentPixelFormat == MTLPixelFormatDepth32Float_Stencil8,
            "Pipeline stencil format did not set Depth32Float_Stencil8 for D24S8");
    const auto d16Format = Metal::RenderPixelFormat(VK_FORMAT_D16_UNORM);
    Require(d16Format == (Metal::PromoteD16() ? MTLPixelFormatDepth32Float : MTLPixelFormatDepth16Unorm),
            "D16 promotion mismatch");
}

void Forced1xMsaa(id<MTLDevice> device) {
    unsetenv("ANYPS5_METAL_FORCE_1X_MSAA");
    Require(!Metal::Force1xMsaaEnabled(), "Forced 1x MSAA enabled by default");

    for (const char* value : {"1", "true", "TRUE", "on", "ON", "yes", "YES"}) {
        setenv("ANYPS5_METAL_FORCE_1X_MSAA", value, 1);
        Require(Metal::Force1xMsaaEnabled(), "Forced 1x MSAA was not enabled for valid truthy value");
    }

    for (const char* value : {"0", "false", "off", "no", "anything_else"}) {
        setenv("ANYPS5_METAL_FORCE_1X_MSAA", value, 1);
        Require(!Metal::Force1xMsaaEnabled(), "Forced 1x MSAA was enabled for non-truthy value");
    }

    setenv("ANYPS5_METAL_FORCE_1X_MSAA", "1", 1);
    Require(Metal::Force1xMsaaEnabled(), "Forced 1x MSAA was not enabled");

    auto state = BasicState();
    state.rasterSamples = 4;
    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, state);
    Require(descriptor.rasterSampleCount == 1, "ConfigureRenderPipelineDescriptor did not force rasterSampleCount to 1");

    Metal::ConfigureSamplePositions(nil, nil, state);

    unsetenv("ANYPS5_METAL_FORCE_1X_MSAA");
    Require(!Metal::Force1xMsaaEnabled(), "Forced 1x MSAA still active after unsetenv");
    auto normalDescriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(normalDescriptor, state);
    Require(normalDescriptor.rasterSampleCount == 4, "ConfigureRenderPipelineDescriptor did not retain 4 samples when override disabled");
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
            DepthStencilCache(device);
            DepthPromotionAndState(device);
            ViewportWinding(device, queue, library);
            ClipDepth(device, queue, library);
            UnrestrictedViewportDepth(device, queue, library);
            DepthBounds(device, queue, library);
            DepthBiasReset(device, queue, library);
            SharedStateDecode(device, queue, library);
            CustomSampleLocations(device, queue, library);
            ColorViewSlices();
            Sync9SharedDecode();
            ClipCullDecode();
            CoveredCentroid();
            EarlyDepthDecode();
            PackedFloatDccClear();
            Forced1xMsaa(device);
            std::cout << "PASS: original State sparse exports, blending/write masks/scissor, depth/stencil, disabled depth writes, positive/negative viewport and winding, clip depth and supported depth bounds pixels\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
