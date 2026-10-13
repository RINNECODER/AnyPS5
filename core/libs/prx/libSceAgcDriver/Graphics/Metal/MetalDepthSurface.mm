#include "MetalDepthSurface.hpp"
#include "MetalRenderState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {
namespace {

id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, VkExtent2D extent, std::uint32_t samples) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:extent.width height:extent.height mipmapped:NO];
    if (samples > 1) {
        if (![device supportsTextureSampleCount:samples]) throw std::invalid_argument("Metal device does not support the depth surface sample count");
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = samples;
    }
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    auto result = [device newTextureWithDescriptor:descriptor];
    if (result == nil) throw std::runtime_error("Metal depth/stencil texture allocation failed");
    return result;
}

bool sameTarget(const Graphics::DepthTarget& left, const Graphics::DepthTarget& right) {
    return left.address == right.address && left.stencilAddress == right.stencilAddress &&
        left.extent.width == right.extent.width && left.extent.height == right.extent.height && left.format == right.format &&
        left.samples == right.samples;
}

}

MetalDepthSurface::MetalDepthSurface(const MetalDevice& backend, const Graphics::DepthTarget& target) : target(target) {
    if (target.extent.width == 0 || target.extent.height == 0 || target.extent.width > 16384 || target.extent.height > 16384 ||
        target.samples == 0 || target.samples > 8 || (target.samples & (target.samples - 1u)) != 0 ||
        !std::isfinite(target.clearDepth) || target.clearDepth < 0 || target.clearDepth > 1) {
        throw std::invalid_argument("Metal depth surface dimensions or clear depth are invalid");
    }
    const bool hasStencil = target.stencilAddress != 0;
    switch (target.format) {
    case VK_FORMAT_D16_UNORM:
        if (hasStencil) throw std::invalid_argument("Metal depth format has an unexpected stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth16Unorm, target.extent, target.samples);
        break;
    case VK_FORMAT_D32_SFLOAT:
        if (hasStencil) throw std::invalid_argument("Metal depth format has an unexpected stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth32Float, target.extent, target.samples);
        break;
    case VK_FORMAT_D16_UNORM_S8_UINT:
        if (!hasStencil) throw std::invalid_argument("Metal depth/stencil format requires a stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth16Unorm, target.extent, target.samples);
        stencil = texture(backend.Device(), MTLPixelFormatStencil8, target.extent, target.samples);
        sampledStencil = stencil;
        break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        if (!hasStencil) throw std::invalid_argument("Metal depth/stencil format requires a stencil plane");
        depth = texture(backend.Device(), MTLPixelFormatDepth32Float_Stencil8, target.extent, target.samples);
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
    if (encoder == nil) throw MetalGpuExecutionError("Metal depth initial clear encoder creation failed");
    [encoder endEncoding];
    backend.Wait(commands);
}

id<MTLTexture> MetalDepthSurface::DepthTexture() const { return depth; }
id<MTLTexture> MetalDepthSurface::StencilTexture() const { return stencil; }
id<MTLTexture> MetalDepthSurface::SampledDepthView() const { return depth; }
id<MTLTexture> MetalDepthSurface::SampledStencilView() const { return sampledStencil; }
const Graphics::DepthTarget& MetalDepthSurface::Target() const { return target; }

id<MTLTexture> GatherDepthSlices(const MetalDevice& backend, std::span<const std::shared_ptr<MetalDepthSurface>> surfaces,
    const MetalDepthSurface& base, std::uint32_t first, std::uint32_t last, id<MTLTexture> reuse) {
    const auto& shape = base.Target();
    if (last < first || shape.samples != 1) throw std::invalid_argument("Metal depth array slices need a single-sample range");
    const bool d16 = shape.format == VK_FORMAT_D16_UNORM || shape.format == VK_FORMAT_D16_UNORM_S8_UINT;
    const auto sliceBytes = Graphics::DepthSliceBytes(shape.extent, d16 ? 2u : 4u);
    std::vector<id<MTLTexture>> sources;
    for (std::uint32_t layer = first; layer <= last; ++layer) {
        const auto address = shape.address + layer * sliceBytes;
        // The newest surface at an address is the one draws render into (MetalDraw keeps them in order).
        const auto found = std::find_if(surfaces.rbegin(), surfaces.rend(), [&](const auto& surface) { return surface->Target().address == address; });
        if (found == surfaces.rend() || (*found)->Target().extent.width != shape.extent.width ||
            (*found)->Target().extent.height != shape.extent.height || (*found)->Target().format != shape.format || (*found)->Target().samples != 1) {
            // Name the surfaces that do follow the base, which shows a slice layout this does not model.
            std::string nearby;
            for (const auto& surface : surfaces) {
                const auto& other = surface->Target();
                if (other.address <= shape.address || other.address - shape.address > (last + 1ull) * sliceBytes) continue;
                char text[96];
                std::snprintf(text, sizeof text, " +0x%llx %ux%u fmt %d;", static_cast<unsigned long long>(other.address - shape.address),
                    other.extent.width, other.extent.height, static_cast<int>(other.format));
                nearby += text;
            }
            char text[160];
            std::snprintf(text, sizeof text, "Metal depth array slice %u has no resident surface of the base slice's shape (%ux%u fmt %d, slice 0x%llx bytes); after the base:",
                layer, shape.extent.width, shape.extent.height, static_cast<int>(shape.format), static_cast<unsigned long long>(sliceBytes));
            throw std::invalid_argument(text + (nearby.empty() ? std::string(" none") : nearby));
        }
        sources.push_back((*found)->SampledDepthView());
    }
    const auto format = sources.front().pixelFormat;
    const auto layers = static_cast<NSUInteger>(sources.size());
    id<MTLTexture> array = reuse;
    if (array == nil || array.textureType != MTLTextureType2DArray || array.pixelFormat != format || array.arrayLength != layers ||
        array.width != shape.extent.width || array.height != shape.extent.height) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
            width:shape.extent.width height:shape.extent.height mipmapped:NO];
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = layers;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        array = [backend.Device() newTextureWithDescriptor:descriptor];
        if (array == nil) throw std::runtime_error("Metal depth array texture creation failed");
    }
    auto commands = backend.CommandBuffer();
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (blit == nil) throw MetalGpuExecutionError("Metal depth array blit encoder creation failed");
    for (NSUInteger i = 0; i < layers; ++i) {
        [blit copyFromTexture:sources[i] sourceSlice:0 sourceLevel:0 toTexture:array destinationSlice:i destinationLevel:0 sliceCount:1 levelCount:1];
    }
    [blit endEncoding];
    [commands commit];
    return array;
}

bool MetalDepthSurface::OverlapsMappings(std::span<const NativeGuestMemory::BorrowedRange> changed) const {
    const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
    const auto overlaps = [&](std::uint64_t address, std::uint64_t bytes) {
        if (address == 0 || bytes == 0) return false;
        return std::any_of(changed.begin(), changed.end(), [&](const auto& range) {
            if (range.host.empty()) return false;
            return address <= range.guestAddress ? range.guestAddress - address < bytes : address - range.guestAddress < range.host.size();
        });
    };
    return overlaps(target.address, Graphics::DepthSliceBytes(target.extent, d16 ? 2u : 4u) * target.samples) ||
        overlaps(target.stencilAddress, Graphics::DepthSliceBytes(target.extent, 1u) * target.samples);
}

MetalColorSurface::MetalColorSurface(const MetalDevice& backend, const Graphics::ColorTarget& target) : target(target) {
    if (target.extent.width == 0 || target.extent.height == 0 || target.extent.width > 16384 || target.extent.height > 16384 ||
        target.samples < 2 || target.samples > 8 || (target.samples & (target.samples - 1u)) != 0) {
        throw std::invalid_argument("Metal multisampled color surface dimensions or sample count are invalid");
    }
    if (![backend.Device() supportsTextureSampleCount:target.samples]) throw std::invalid_argument("Metal device does not support the color surface sample count");
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:RenderPixelFormat(target.format)
        width:target.extent.width height:target.extent.height mipmapped:NO];
    descriptor.textureType = MTLTextureType2DMultisample;
    descriptor.sampleCount = target.samples;
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    texture = [backend.Device() newTextureWithDescriptor:descriptor];
    if (texture == nil) throw std::runtime_error("Metal multisampled color texture allocation failed");
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    auto commands = backend.CommandBuffer();
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) throw MetalGpuExecutionError("Metal color surface initial clear encoder creation failed");
    [encoder endEncoding];
    backend.Wait(commands);
}

id<MTLTexture> MetalColorSurface::Texture() const { return texture; }
const Graphics::ColorTarget& MetalColorSurface::Target() const { return target; }

bool MetalColorSurface::Matches(const Graphics::ColorTarget& other) const {
    return target.address == other.address && target.extent.width == other.extent.width && target.extent.height == other.extent.height &&
        RenderPixelFormat(target.format) == RenderPixelFormat(other.format) && target.samples == other.samples;
}

bool MetalColorSurface::OverlapsMappings(std::span<const NativeGuestMemory::BorrowedRange> changed) const {
    const auto bytes = target.bytes * target.samples;
    return std::any_of(changed.begin(), changed.end(), [&](const auto& range) {
        if (range.host.empty() || bytes == 0) return false;
        return target.address <= range.guestAddress ? range.guestAddress - target.address < bytes : target.address - range.guestAddress < range.host.size();
    });
}

MetalDepthSurfaceCache::MetalDepthSurfaceCache(const MetalDevice& backend) : backend(backend) {}

std::shared_ptr<MetalColorSurface> MetalDepthSurfaceCache::Acquire(const Graphics::ColorTarget& target) {
    std::lock_guard lock(mutex);
    auto found = std::find_if(colorSurfaces.begin(), colorSurfaces.end(), [&](const auto& surface) { return surface->Matches(target); });
    if (found != colorSurfaces.end()) return *found;
    // A different layout at the same address replaces the old fragments.
    std::erase_if(colorSurfaces, [&](const auto& surface) { return surface->Target().address == target.address; });
    auto result = std::make_shared<MetalColorSurface>(backend, target);
    colorSurfaces.push_back(result);
    return result;
}

std::shared_ptr<MetalDepthSurface> MetalDepthSurfaceCache::Acquire(const Graphics::DepthTarget& target) {
    std::lock_guard lock(mutex);
    auto found = std::find_if(surfaces.begin(), surfaces.end(), [&](const auto& surface) { return sameTarget(surface->Target(), target); });
    if (found != surfaces.end()) return *found;
    auto result = std::make_shared<MetalDepthSurface>(backend, target);
    surfaces.push_back(result);
    return result;
}

void MetalDepthSurfaceCache::Invalidate(std::span<const NativeGuestMemory::BorrowedRange> changed) {
    std::lock_guard lock(mutex);
    std::erase_if(surfaces, [&](const auto& surface) { return surface->OverlapsMappings(changed); });
    std::erase_if(colorSurfaces, [&](const auto& surface) { return surface->OverlapsMappings(changed); });
}

void MetalDepthSurfaceCache::Clear() {
    std::lock_guard lock(mutex);
    surfaces.clear();
    colorSurfaces.clear();
}

MetalMultisampleExpander::MetalMultisampleExpander(const MetalDevice& backend) : backend(backend) {}

id<MTLTexture> MetalMultisampleExpander::Expand(id<MTLCommandBuffer> commands, id<MTLTexture> source, std::uint32_t samples, std::uint32_t slot) {
    if (commands == nil || source == nil || source.textureType != MTLTextureType2D || source.mipmapLevelCount != 1 || source.sampleCount != 1) {
        throw std::invalid_argument("Metal multisample expansion needs a single-level 2D source");
    }
    if (samples < 2 || samples > 8 || (samples & (samples - 1u)) != 0 || ![backend.Device() supportsTextureSampleCount:samples]) {
        throw std::invalid_argument("Metal device does not support the multisample expansion sample count");
    }
    std::lock_guard lock(mutex);
    if (library == nil) {
        NSError* error = nil;
        library = [backend.Device() newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 expandVertex(uint id [[vertex_id]]) { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0, 1); }\n"
            "fragment float4 expandFragment(float4 position [[position]], texture2d<float, access::read> source [[texture(0)]]) {\n"
            "    return source.read(uint2(position.xy)); }\n" options:nil error:&error];
        if (library == nil) throw std::runtime_error(std::string("Metal multisample expansion library failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
    }
    const auto key = (static_cast<std::uint64_t>(source.pixelFormat) << 8u) | samples;
    auto pipeline = std::find_if(pipelines.begin(), pipelines.end(), [&](const auto& entry) { return entry.first == key; });
    if (pipeline == pipelines.end()) {
        auto descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"expandVertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"expandFragment"];
        descriptor.colorAttachments[0].pixelFormat = source.pixelFormat;
        descriptor.rasterSampleCount = samples;
        NSError* error = nil;
        id<MTLRenderPipelineState> state = [backend.Device() newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (state == nil) throw std::runtime_error(std::string("Metal multisample expansion pipeline failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
        pipeline = pipelines.insert(pipelines.end(), {key, state});
    }
    auto transient = std::find_if(transients.begin(), transients.end(), [&](const auto& entry) {
        return entry.first == slot && entry.second.pixelFormat == source.pixelFormat && entry.second.width == source.width &&
            entry.second.height == source.height && entry.second.sampleCount == samples;
    });
    if (transient == transients.end()) {
        std::erase_if(transients, [&](const auto& entry) { return entry.first == slot; });
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat width:source.width height:source.height mipmapped:NO];
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = samples;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [backend.Device() newTextureWithDescriptor:descriptor];
        if (texture == nil) throw std::runtime_error("Metal multisample expansion texture allocation failed");
        transient = transients.insert(transients.end(), {slot, texture});
    }
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = transient->second;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) throw MetalGpuExecutionError("Metal multisample expansion encoder creation failed");
    [encoder setRenderPipelineState:pipeline->second];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    return transient->second;
}

}
