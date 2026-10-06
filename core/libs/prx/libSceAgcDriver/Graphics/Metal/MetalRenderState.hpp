#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"

namespace AgcDriver::Metal {

[[nodiscard]] MTLPixelFormat RenderPixelFormat(VkFormat format);
[[nodiscard]] MTLPrimitiveType PrimitiveType(const Graphics::State& state);
void ConfigureRenderPipelineDescriptor(MTLRenderPipelineDescriptor* descriptor, const Graphics::State& state);
[[nodiscard]] id<MTLDepthStencilState> CreateDepthStencilState(id<MTLDevice> device, const Graphics::State& state);
void BindRenderState(id<MTLRenderCommandEncoder> encoder, const Graphics::State& state,
                     id<MTLDepthStencilState> depthStencilState);

}
