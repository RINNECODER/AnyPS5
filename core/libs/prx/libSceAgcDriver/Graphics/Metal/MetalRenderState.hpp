#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"

namespace AgcDriver::Metal {

[[nodiscard]] bool PromoteD16();
[[nodiscard]] MTLPixelFormat RenderPixelFormat(VkFormat format);
// The texture a color attachment of `format` renders through. Guest color surfaces are typed by their
// memory layout (a COMP_SWAP_ALT 8_8_8_8 target is RGBA8 memory written as BGRA), so a same-size
// attachment format renders through a pixel format view of the surface; other mismatches throw.
[[nodiscard]] id<MTLTexture> ColorAttachmentView(id<MTLTexture> texture, MTLPixelFormat format);
[[nodiscard]] MTLPrimitiveType PrimitiveType(const Graphics::State& state);
void ConfigureRenderPipelineDescriptor(MTLRenderPipelineDescriptor* descriptor, const Graphics::State& state);
void ConfigureRenderPipelineDescriptor(MTLMeshRenderPipelineDescriptor* descriptor, const Graphics::State& state);
[[nodiscard]] id<MTLDepthStencilState> CreateDepthStencilState(id<MTLDevice> device, const Graphics::State& state);
// Sets the pass's programmable sample positions for a custom PA_SC_AA_SAMPLE_LOCS pattern. A standard
// pattern keeps Metal's default positions, which are the same locations. Throws when the device has no
// programmable sample positions.
void ConfigureSamplePositions(MTLRenderPassDescriptor* pass, id<MTLDevice> device, const Graphics::State& state);
void BindRenderState(id<MTLRenderCommandEncoder> encoder, const Graphics::State& state,
                     id<MTLDepthStencilState> depthStencilState);

}
