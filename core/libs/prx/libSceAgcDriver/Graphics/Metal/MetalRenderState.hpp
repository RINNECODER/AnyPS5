#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"

namespace AgcDriver::Metal {

[[nodiscard]] MTLPixelFormat RenderPixelFormat(VkFormat format);
// The texture a color attachment of `format` renders through. Guest color surfaces are typed by their
// memory layout (a COMP_SWAP_ALT 8_8_8_8 target is RGBA8 memory written as BGRA), so a same-size
// attachment format renders through a pixel format view of the surface; other mismatches throw.
[[nodiscard]] id<MTLTexture> ColorAttachmentView(id<MTLTexture> texture, MTLPixelFormat format);
[[nodiscard]] MTLPrimitiveType PrimitiveType(const Graphics::State& state);
void ConfigureRenderPipelineDescriptor(MTLRenderPipelineDescriptor* descriptor, const Graphics::State& state);
void ConfigureRenderPipelineDescriptor(MTLMeshRenderPipelineDescriptor* descriptor, const Graphics::State& state);
[[nodiscard]] id<MTLDepthStencilState> CreateDepthStencilState(id<MTLDevice> device, const Graphics::State& state);
void BindRenderState(id<MTLRenderCommandEncoder> encoder, const Graphics::State& state,
                     id<MTLDepthStencilState> depthStencilState);

}
