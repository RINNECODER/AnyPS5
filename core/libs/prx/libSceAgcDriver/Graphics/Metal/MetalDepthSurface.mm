#include "MetalDepthSurface.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace AgcDriver::Metal {
namespace {

id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, VkExtent2D extent) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:extent.width height:extent.height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    auto result = [device newTextureWithDescriptor:descriptor];
    if (result == nil) throw std::runtime_error("Metal depth/stencil texture allocation failed");
    return result;
}

bool sameTarget(const Graphics::DepthTarget& left, const Graphics::DepthTarget& right) {
    return left.address == right.address && left.stencilAddress == right.stencilAddress &&
        left.extent.width == right.extent.width && left.extent.height == right.extent.height && left.format == right.format;
}

}

MetalDepthSurface::MetalDepthSurface(const MetalDevice& backend, const Graphics::DepthTarget& target) : target(target) {
    if (target.extent.width == 0 || target.extent.height == 0 || target.extent.width > 16384 || target.extent.height > 16384 ||
        !std::isfinite(target.clearDepth) || target.clearDepth < 0 || target.clearDepth > 1) {
        throw std::invalid_argument("Metal depth surface dimensions or clear depth are invalid");
    }
    const bool hasStencil = target.stencilAddress != 0;
    switch (target.format) {
    case VK_FORMAT_D16_UNORM:
        if (hasStencil) throw std::invalid_argument("Metal depth format has an unexpected stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth16Unorm, target.extent);
        break;
    case VK_FORMAT_D32_SFLOAT:
        if (hasStencil) throw std::invalid_argument("Metal depth format has an unexpected stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth32Float, target.extent);
        break;
    case VK_FORMAT_D16_UNORM_S8_UINT:
        if (!hasStencil) throw std::invalid_argument("Metal depth/stencil format requires a stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth16Unorm, target.extent);
        stencil = texture(backend.Device(), MTLPixelFormatStencil8, target.extent);
        sampledStencil = stencil;
        break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        if (!hasStencil) throw std::invalid_argument("Metal depth/stencil format requires a stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth32Float_Stencil8, target.extent);
        stencil = depth;
        sampledStencil = [depth newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        if (sampledStencil == nil) throw std::runtime_error("Metal stencil sampled view creation failed");
        break;
    default:
        throw std::invalid_argument("Metal depth surface format is unsupported");
    }
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = target.clearDepth;
    if (hasStencil) {
        pass.stencilAttachment.texture = stencil;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.clearStencil = target.clearStencil;
    }
    auto commands = backend.CommandBuffer();
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) throw std::runtime_error("Metal depth initial clear encoder creation failed");
    [encoder endEncoding];
    backend.Wait(commands);
}

id<MTLTexture> MetalDepthSurface::DepthTexture() const { return depth; }
id<MTLTexture> MetalDepthSurface::StencilTexture() const { return stencil; }
id<MTLTexture> MetalDepthSurface::SampledDepthView() const { return depth; }
id<MTLTexture> MetalDepthSurface::SampledStencilView() const { return sampledStencil; }
const Graphics::DepthTarget& MetalDepthSurface::Target() const { return target; }

MetalDepthSurfaceCache::MetalDepthSurfaceCache(const MetalDevice& backend) : backend(backend) {}

std::shared_ptr<MetalDepthSurface> MetalDepthSurfaceCache::Acquire(const Graphics::DepthTarget& target) {
    std::lock_guard lock(mutex);
    auto found = std::find_if(surfaces.begin(), surfaces.end(), [&](const auto& surface) { return sameTarget(surface->Target(), target); });
    if (found != surfaces.end()) return *found;
    auto result = std::make_shared<MetalDepthSurface>(backend, target);
    surfaces.push_back(result);
    return result;
}

void MetalDepthSurfaceCache::Clear() {
    std::lock_guard lock(mutex);
    surfaces.clear();
}

}
