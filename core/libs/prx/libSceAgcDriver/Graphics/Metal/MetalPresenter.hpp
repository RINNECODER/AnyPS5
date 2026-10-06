#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <map>
#include <mutex>

namespace AgcDriver::Metal {

class MetalPresenter {
public:
    MetalPresenter(id<MTLDevice> device, id<MTLLibrary> library);
    MetalPresenter(const MetalPresenter&) = delete;
    MetalPresenter& operator=(const MetalPresenter&) = delete;
    void Encode(id<MTLCommandBuffer> commands, id<MTLTexture> source, id<MTLTexture> target, bool linear = true) const;
    [[nodiscard]] bool Present(id<MTLCommandBuffer> commands, id<MTLTexture> source, CAMetalLayer* layer, bool linear = true) const;

private:
    id<MTLRenderPipelineState> pipeline(MTLPixelFormat format) const;
    id<MTLDevice> device;
    id<MTLFunction> vertex;
    id<MTLFunction> fragment;
    id<MTLSamplerState> nearestSampler;
    id<MTLSamplerState> linearSampler;
    mutable std::mutex mutex;
    mutable std::map<MTLPixelFormat, id<MTLRenderPipelineState>> pipelines;
};

}
