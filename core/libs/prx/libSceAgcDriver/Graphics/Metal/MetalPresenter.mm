#include "MetalPresenter.hpp"
#include "prx/libSceAgcDriver/Execution/include/AspectFit.hpp"
#include <limits>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {

MetalPresenter::MetalPresenter(id<MTLDevice> device, id<MTLLibrary> library) : device(device) {
    if (device == nil || library == nil || library.device != device) throw std::invalid_argument("MetalPresenter requires a matching device and library");
    vertex = [library newFunctionWithName:@"presentationVertex"];
    fragment = [library newFunctionWithName:@"presentationFragment"];
    if (vertex == nil || fragment == nil) throw std::runtime_error("Metal presentation shader functions are missing");
    MTLSamplerDescriptor* sampler = [MTLSamplerDescriptor new];
    sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler.minFilter = MTLSamplerMinMagFilterNearest;
    sampler.magFilter = MTLSamplerMinMagFilterNearest;
    nearestSampler = [device newSamplerStateWithDescriptor:sampler];
    sampler.minFilter = MTLSamplerMinMagFilterLinear;
    sampler.magFilter = MTLSamplerMinMagFilterLinear;
    linearSampler = [device newSamplerStateWithDescriptor:sampler];
    if (nearestSampler == nil || linearSampler == nil) throw std::runtime_error("Metal presentation sampler creation failed");
}

id<MTLRenderPipelineState> MetalPresenter::pipeline(MTLPixelFormat format) const {
    std::lock_guard lock(mutex);
    if (const auto found = pipelines.find(format); found != pipelines.end()) return found->second;
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = format;
    NSError* error = nil;
    id<MTLRenderPipelineState> result = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (result == nil) throw std::runtime_error(std::string("Metal presentation pipeline creation failed: ") + (error.localizedDescription.UTF8String ?: "unknown error"));
    pipelines.emplace(format, result);
    return result;
}

void MetalPresenter::Encode(id<MTLCommandBuffer> commands, id<MTLTexture> source, id<MTLTexture> target, bool linear) const {
    if (commands == nil || commands.commandQueue.device != device || commands.status != MTLCommandBufferStatusNotEnqueued) throw std::invalid_argument("Metal presentation requires an uncommitted command buffer on its device");
    if (source == nil || target == nil || source.device != device || target.device != device || source == target) throw std::invalid_argument("Metal presentation requires distinct textures on its device");
    if (source.textureType != MTLTextureType2D || target.textureType != MTLTextureType2D || source.sampleCount != 1 || target.sampleCount != 1) throw std::invalid_argument("Metal presentation requires single-sample 2D textures");
    if ((source.usage & MTLTextureUsageShaderRead) == 0 || (target.usage & MTLTextureUsageRenderTarget) == 0) throw std::invalid_argument("Metal presentation texture usage does not support sampling and rendering");
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    if (source.width > maximum || source.height > maximum || target.width > maximum || target.height > maximum) throw std::invalid_argument("Metal presentation texture extent exceeds aspect-fit limits");
    const auto fit = ComputeContainRect_nid_postfix(static_cast<std::uint32_t>(source.width), static_cast<std::uint32_t>(source.height), static_cast<std::uint32_t>(target.width), static_cast<std::uint32_t>(target.height));
    const auto renderPipeline = pipeline(target.pixelFormat);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) throw std::runtime_error("Metal presentation encoder creation failed");
    [encoder setRenderPipelineState:renderPipeline];
    [encoder setViewport:MTLViewport{static_cast<double>(fit.x), static_cast<double>(fit.y), static_cast<double>(fit.width), static_cast<double>(fit.height), 0, 1}];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder setFragmentSamplerState:linear ? linearSampler : nearestSampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
}

bool MetalPresenter::Present(id<MTLCommandBuffer> commands, id<MTLTexture> source, CAMetalLayer* layer, bool linear) const {
    if (layer == nil || layer.device != device) throw std::invalid_argument("Metal presentation requires a layer on its device");
    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) return false;
    Encode(commands, source, drawable.texture, linear);
    [commands presentDrawable:drawable];
    return true;
}

}
