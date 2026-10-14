#include "MetalDepthSurface.hpp"
#include "MetalRenderState.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {
namespace {

id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, VkExtent2D extent, std::uint32_t samples, std::uint32_t layers) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:extent.width height:extent.height mipmapped:NO];
    if (samples > 1) {
        if (![device supportsTextureSampleCount:samples]) throw std::invalid_argument("Metal device does not support the depth surface sample count");
        descriptor.textureType = MTLTextureType2DMultisample;
        descriptor.sampleCount = samples;
    } else {
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = layers;
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

bool sameShape(const Graphics::DepthTarget& left, const Graphics::DepthTarget& right) {
    return left.extent.width == right.extent.width && left.extent.height == right.extent.height && left.format == right.format &&
        left.samples == right.samples && (left.stencilAddress != 0) == (right.stencilAddress != 0);
}

bool hasD16(const Graphics::DepthTarget& target) {
    return target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
}

// Guest addresses of slice 0 of the depth array a target's slice belongs to.
std::pair<std::uint64_t, std::uint64_t> arrayBase(const Graphics::DepthTarget& target, std::uint32_t layer) {
    const auto depthBytes = Graphics::DepthSliceBytes(target.extent, hasD16(target) ? 2u : 4u);
    const auto stencilBytes = Graphics::DepthSliceBytes(target.extent, 1u);
    return {target.address == 0 ? 0 : target.address - std::uint64_t{layer} * depthBytes,
        target.stencilAddress == 0 ? 0 : target.stencilAddress - std::uint64_t{layer} * stencilBytes};
}

void validate(const Graphics::DepthTarget& target) {
    if (target.extent.width == 0 || target.extent.height == 0 || target.extent.width > 16384 || target.extent.height > 16384 ||
        target.samples == 0 || target.samples > 8 || (target.samples & (target.samples - 1u)) != 0 ||
        !std::isfinite(target.clearDepth) || target.clearDepth < 0 || target.clearDepth > 1) {
        throw std::invalid_argument("Metal depth surface dimensions or clear depth are invalid");
    }
    const bool hasStencil = target.stencilAddress != 0;
    switch (target.format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D32_SFLOAT:
        if (hasStencil) throw std::invalid_argument("Metal depth format has an unexpected stencil plane");
        break;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        if (!hasStencil) throw std::invalid_argument("Metal depth/stencil format requires a stencil plane");
        break;
    default:
        throw std::invalid_argument("Metal depth surface format is unsupported");
    }
}

}

std::shared_ptr<MetalDepthPlanes> CreateDepthPlanes(const MetalDevice& backend, const Graphics::DepthTarget& target, std::uint32_t layers) {
    validate(target);
    if (layers == 0 || layers > 2048 || (target.samples > 1 && layers != 1)) throw std::invalid_argument("Metal depth array layer count is invalid");
    auto planes = std::make_shared<MetalDepthPlanes>();
    planes->layers = layers;
    const auto device = backend.Device();
    switch (target.format) {
    case VK_FORMAT_D16_UNORM:
        planes->depth = texture(device, MTLPixelFormatDepth16Unorm, target.extent, target.samples, layers);
        break;
    case VK_FORMAT_D32_SFLOAT:
        planes->depth = texture(device, MTLPixelFormatDepth32Float, target.extent, target.samples, layers);
        break;
    case VK_FORMAT_D16_UNORM_S8_UINT:
        planes->depth = texture(device, MTLPixelFormatDepth16Unorm, target.extent, target.samples, layers);
        planes->stencil = texture(device, MTLPixelFormatStencil8, target.extent, target.samples, layers);
        break;
    default:
        planes->depth = texture(device, MTLPixelFormatDepth32Float_Stencil8, target.extent, target.samples, layers);
        planes->stencil = planes->depth;
        break;
    }
    // One layered pass clears every layer.
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = planes->depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = target.clearDepth;
    if (planes->stencil != nil) {
        pass.stencilAttachment.texture = planes->stencil;
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionStore;
        pass.stencilAttachment.clearStencil = target.clearStencil;
    }
    if (target.samples == 1) pass.renderTargetArrayLength = layers;
    auto commands = backend.CommandBuffer();
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) throw MetalGpuExecutionError("Metal depth initial clear encoder creation failed");
    [encoder endEncoding];
    backend.Wait(commands);
    return planes;
}

MetalDepthSurface::MetalDepthSurface(const MetalDevice& backend, const Graphics::DepthTarget& target)
    : MetalDepthSurface(target, CreateDepthPlanes(backend, target, 1), 0) {}

MetalDepthSurface::MetalDepthSurface(const Graphics::DepthTarget& target, std::shared_ptr<MetalDepthPlanes> planes, std::uint32_t layer)
    : target(target), planes(std::move(planes)), layer(layer) {
    validate(target);
    if (this->planes == nullptr || layer >= this->planes->layers) throw std::invalid_argument("Metal depth surface layer is outside its planes");
    refreshViews();
}

void MetalDepthSurface::refreshViews() const {
    if (viewGeneration == planes->generation) return;
    const auto view = [&](id<MTLTexture> source, MTLPixelFormat format) -> id<MTLTexture> {
        if (source.textureType != MTLTextureType2DArray) {
            if (format == source.pixelFormat) return source;
            return [source newTextureViewWithPixelFormat:format];
        }
        return [source newTextureViewWithPixelFormat:format textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(layer, 1)];
    };
    depth = view(planes->depth, planes->depth.pixelFormat);
    stencil = planes->stencil == nil ? nil : planes->stencil == planes->depth ? depth : view(planes->stencil, planes->stencil.pixelFormat);
    sampledStencil = planes->stencil == nil ? nil : planes->stencil == planes->depth ? view(planes->depth, MTLPixelFormatX32_Stencil8) : stencil;
    if (depth == nil || (planes->stencil != nil && (stencil == nil || sampledStencil == nil))) throw std::runtime_error("Metal depth surface view creation failed");
    viewGeneration = planes->generation;
}

id<MTLTexture> MetalDepthSurface::DepthTexture() const { refreshViews(); return depth; }
id<MTLTexture> MetalDepthSurface::StencilTexture() const { refreshViews(); return stencil; }
id<MTLTexture> MetalDepthSurface::SampledDepthView() const { refreshViews(); return depth; }
id<MTLTexture> MetalDepthSurface::SampledStencilView() const { refreshViews(); return sampledStencil; }
MetalDepthPlaneView MetalDepthSurface::SampledDepthPlane() const { return {planes->depth, planes->depth.pixelFormat, layer}; }
MetalDepthPlaneView MetalDepthSurface::SampledStencilPlane() const {
    if (planes->stencil == nil) return {};
    return {planes->stencil, planes->stencil == planes->depth ? MTLPixelFormatX32_Stencil8 : planes->stencil.pixelFormat, layer};
}

id<MTLTexture> MetalDepthSurface::LayeredDepthTexture(std::uint32_t count) const {
    if (planes->depth.textureType != MTLTextureType2DArray || count == 0 || count > planes->layers - layer) {
        throw std::invalid_argument("Metal layered depth view is outside the surface's depth array");
    }
    id<MTLTexture> result = [planes->depth newTextureViewWithPixelFormat:planes->depth.pixelFormat textureType:MTLTextureType2DArray
        levels:NSMakeRange(0, 1) slices:NSMakeRange(layer, count)];
    if (result == nil) throw std::runtime_error("Metal layered depth view creation failed");
    return result;
}

id<MTLTexture> MetalDepthSurface::LayeredStencilTexture(std::uint32_t count) const {
    if (planes->stencil == nil) return nil;
    if (planes->stencil == planes->depth) return nil; // the caller attaches the depth view to both
    if (count == 0 || count > planes->layers - layer) throw std::invalid_argument("Metal layered stencil view is outside the surface's depth array");
    id<MTLTexture> result = [planes->stencil newTextureViewWithPixelFormat:planes->stencil.pixelFormat textureType:MTLTextureType2DArray
        levels:NSMakeRange(0, 1) slices:NSMakeRange(layer, count)];
    if (result == nil) throw std::runtime_error("Metal layered stencil view creation failed");
    return result;
}

const Graphics::DepthTarget& MetalDepthSurface::Target() const { return target; }
const std::shared_ptr<MetalDepthPlanes>& MetalDepthSurface::Planes() const { return planes; }
std::uint32_t MetalDepthSurface::Layer() const { return layer; }

void MetalDepthSurface::Rebind(std::shared_ptr<MetalDepthPlanes> other, std::uint32_t otherLayer) {
    if (other == nullptr || otherLayer >= other->layers) throw std::invalid_argument("Metal depth surface layer is outside its planes");
    planes = std::move(other);
    layer = otherLayer;
    viewGeneration = ~0ull;
    refreshViews();
}

id<MTLTexture> GatherDepthSlices(const MetalDevice& backend, std::span<const std::shared_ptr<MetalDepthSurface>> surfaces,
    const MetalDepthSurface& base, std::uint32_t first, std::uint32_t last, id<MTLTexture> reuse) {
    const auto& shape = base.Target();
    if (last < first || shape.samples != 1) throw std::invalid_argument("Metal depth array slices need a single-sample range");
    const auto sliceBytes = Graphics::DepthSliceBytes(shape.extent, hasD16(shape) ? 2u : 4u);
    std::vector<const MetalDepthSurface*> sources;
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
        sources.push_back(found->get());
    }
    // Consecutive layers of one set of planes: view them in place.
    const auto& planes = base.Planes();
    bool inPlace = planes->depth.textureType == MTLTextureType2DArray;
    for (std::size_t i = 0; inPlace && i < sources.size(); ++i) {
        inPlace = sources[i]->Planes() == planes && sources[i]->Layer() == base.Layer() + first + i;
    }
    if (inPlace) {
        id<MTLTexture> view = [planes->depth newTextureViewWithPixelFormat:planes->depth.pixelFormat textureType:MTLTextureType2DArray
            levels:NSMakeRange(0, 1) slices:NSMakeRange(base.Layer() + first, sources.size())];
        if (view == nil) throw std::runtime_error("Metal depth array view creation failed");
        return view;
    }
    const auto format = planes->depth.pixelFormat;
    const auto layers = static_cast<NSUInteger>(sources.size());
    id<MTLTexture> array = reuse;
    if (array == nil || array.parentTexture != nil || array.textureType != MTLTextureType2DArray || array.pixelFormat != format ||
        array.arrayLength != layers || array.width != shape.extent.width || array.height != shape.extent.height) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
            width:shape.extent.width height:shape.extent.height mipmapped:NO];
        descriptor.textureType = MTLTextureType2DArray;
        descriptor.arrayLength = layers;
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        array = [backend.Device() newTextureWithDescriptor:descriptor];
        if (array == nil) throw std::runtime_error("Metal depth array texture creation failed");
    }
    // Checked before encoding: a throw must not leave an encoder open in a shared command buffer.
    for (NSUInteger i = 0; i < layers; ++i) {
        if (sources[i]->Planes()->depth.pixelFormat != format) throw std::invalid_argument("Metal depth array slices disagree on their pixel format");
    }
    auto commands = backend.CommandBuffer();
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (blit == nil) throw MetalGpuExecutionError("Metal depth array blit encoder creation failed");
    for (NSUInteger i = 0; i < layers; ++i) {
        [blit copyFromTexture:sources[i]->Planes()->depth sourceSlice:sources[i]->Layer() sourceLevel:0 toTexture:array destinationSlice:i destinationLevel:0 sliceCount:1 levelCount:1];
    }
    [blit endEncoding];
    backend.Submit(commands);
    return array;
}

bool MetalDepthSurface::Overlaps(std::uint64_t address, std::uint64_t bytes) const {
    const auto overlaps = [&](std::uint64_t plane, std::uint64_t planeBytes) {
        if (plane == 0 || planeBytes == 0 || bytes == 0) return false;
        return address <= plane ? plane - address < bytes : address - plane < planeBytes;
    };
    return overlaps(target.address, Graphics::DepthSliceBytes(target.extent, hasD16(target) ? 2u : 4u) * target.samples) ||
        overlaps(target.stencilAddress, Graphics::DepthSliceBytes(target.extent, 1u) * target.samples);
}

void RetireDepthSurfaces(std::vector<std::shared_ptr<MetalDepthSurface>>& surfaces, std::uint64_t address, std::uint64_t bytes,
    const MetalDepthSurface* keep) {
    std::erase_if(surfaces, [&](const auto& surface) { return surface.get() != keep && surface->Overlaps(address, bytes); });
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
    if (target.samples > 1 || (target.slice == 0 && target.lastSlice == 0)) {
        auto result = found != surfaces.end() ? *found : std::make_shared<MetalDepthSurface>(backend, target);
        if (found == surfaces.end()) surfaces.push_back(result);
        takePlanes(result);
        return result;
    }
    validate(target);
    // A slice of a depth array: find the planes that hold its slice 0 (any surface of this shape whose
    // address, less its layer's slices, is the array's base), creating or growing them as needed.
    const auto base = arrayBase(target, target.slice);
    const auto layers = std::max(target.slice, target.lastSlice) + 1u;
    std::shared_ptr<MetalDepthPlanes> planes;
    for (const auto& surface : surfaces) {
        if (surface->Planes()->depth.textureType == MTLTextureType2DArray && sameShape(surface->Target(), target) &&
            arrayBase(surface->Target(), surface->Layer()) == base) {
            planes = surface->Planes();
            break;
        }
    }
    if (planes == nullptr) planes = CreateDepthPlanes(backend, target, layers);
    else if (planes->layers < layers) growPlanes(planes, target, layers);
    if (found != surfaces.end()) {
        const auto& surface = *found;
        if (surface->Planes() == planes && surface->Layer() == target.slice) return surface;
        // Rendered before its array was known (or as a slice of another base): move its texels into the
        // array's layer, so every view of these addresses reads one copy.
        if (surface->Planes()->depth.pixelFormat == planes->depth.pixelFormat && surface->Planes()->depth.textureType == MTLTextureType2DArray) {
            auto commands = backend.CommandBuffer();
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            if (blit == nil) throw MetalGpuExecutionError("Metal depth slice adoption blit encoder creation failed");
            [blit copyFromTexture:surface->Planes()->depth sourceSlice:surface->Layer() sourceLevel:0 toTexture:planes->depth
                destinationSlice:target.slice destinationLevel:0 sliceCount:1 levelCount:1];
            if (planes->stencil != nil && planes->stencil != planes->depth) {
                [blit copyFromTexture:surface->Planes()->stencil sourceSlice:surface->Layer() sourceLevel:0 toTexture:planes->stencil
                    destinationSlice:target.slice destinationLevel:0 sliceCount:1 levelCount:1];
            }
            [blit endEncoding];
            backend.Wait(commands);
            surface->Rebind(planes, target.slice);
            return surface;
        }
        surfaces.erase(found);
    }
    auto result = std::make_shared<MetalDepthSurface>(target, planes, target.slice);
    surfaces.push_back(result);
    return result;
}

void MetalDepthSurfaceCache::takePlanes(const std::shared_ptr<MetalDepthSurface>& surface) {
    const auto& target = surface->Target();
    std::vector<std::pair<std::shared_ptr<MetalDepthSurface>, bool>> copies;
    const auto take = [&](std::unordered_map<std::uint64_t, std::weak_ptr<MetalDepthSurface>>& owners, std::uint64_t address, bool stencil) {
        auto& entry = owners[address];
        auto owner = entry.lock();
        entry = surface;
        if (owner == nullptr || owner == surface) return;
        const auto& other = owner->Target();
        // The plane's texels only carry over between surfaces that store it the same way; a
        // multisampled plane keeps its own (blits through a buffer read single-sample textures only).
        if (other.format != target.format || other.samples != 1 || target.samples != 1 ||
            other.extent.width != target.extent.width || other.extent.height != target.extent.height) return;
        if (std::find(surfaces.begin(), surfaces.end(), owner) == surfaces.end()) return;  // dropped: its texels are dead
        copies.emplace_back(std::move(owner), stencil);
    };
    take(depthOwners, target.address, false);
    if (target.stencilAddress != 0) take(stencilOwners, target.stencilAddress, true);
    if (copies.empty()) return;
    auto commands = backend.CommandBuffer();
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (blit == nil) throw MetalGpuExecutionError("Metal depth plane carry-over blit encoder creation failed");
    const MTLSize size = MTLSizeMake(target.extent.width, target.extent.height, 1);
    for (const auto& [owner, stencil] : copies) {
        id<MTLTexture> source = stencil ? owner->Planes()->stencil : owner->Planes()->depth;
        id<MTLTexture> destination = stencil ? surface->Planes()->stencil : surface->Planes()->depth;
        if (source.pixelFormat != MTLPixelFormatDepth32Float_Stencil8) {
            [blit copyFromTexture:source sourceSlice:owner->Layer() sourceLevel:0 toTexture:destination
                destinationSlice:surface->Layer() destinationLevel:0 sliceCount:1 levelCount:1];
            continue;
        }
        // A combined depth/stencil texture: copy only this plane's aspect, through a buffer.
        const auto option = stencil ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
        const NSUInteger rowBytes = target.extent.width * (stencil ? 1u : 4u);
        id<MTLBuffer> staging = [backend.Device() newBufferWithLength:rowBytes * target.extent.height options:MTLResourceStorageModePrivate];
        if (staging == nil) throw std::runtime_error("Metal depth plane carry-over buffer allocation failed");
        [blit copyFromTexture:source sourceSlice:owner->Layer() sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size
            toBuffer:staging destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * target.extent.height options:option];
        [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:rowBytes sourceBytesPerImage:rowBytes * target.extent.height sourceSize:size
            toTexture:destination destinationSlice:surface->Layer() destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0) options:option];
    }
    [blit endEncoding];
    backend.Wait(commands);
}

void MetalDepthSurfaceCache::growPlanes(const std::shared_ptr<MetalDepthPlanes>& planes, const Graphics::DepthTarget& shape, std::uint32_t layers) {
    // New layers start cleared, like a new surface; the old ones keep their texels. Surfaces viewing
    // these planes make their views again (MetalDepthPlanes::generation).
    const auto grown = CreateDepthPlanes(backend, shape, layers);
    auto commands = backend.CommandBuffer();
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (blit == nil) throw MetalGpuExecutionError("Metal depth array growth blit encoder creation failed");
    [blit copyFromTexture:planes->depth sourceSlice:0 sourceLevel:0 toTexture:grown->depth destinationSlice:0 destinationLevel:0
        sliceCount:planes->layers levelCount:1];
    if (planes->stencil != nil && planes->stencil != planes->depth) {
        [blit copyFromTexture:planes->stencil sourceSlice:0 sourceLevel:0 toTexture:grown->stencil destinationSlice:0 destinationLevel:0
            sliceCount:planes->layers levelCount:1];
    }
    [blit endEncoding];
    backend.Wait(commands);
    planes->depth = grown->depth;
    planes->stencil = grown->stencil;
    planes->layers = layers;
    ++planes->generation;
}

void MetalDepthSurfaceCache::Invalidate(std::span<const NativeGuestMemory::BorrowedRange> changed) {
    std::lock_guard lock(mutex);
    std::erase_if(surfaces, [&](const auto& surface) { return surface->OverlapsMappings(changed); });
    std::erase_if(colorSurfaces, [&](const auto& surface) { return surface->OverlapsMappings(changed); });
}

void MetalDepthSurfaceCache::Forget(const std::shared_ptr<MetalDepthSurface>& surface) {
    std::lock_guard lock(mutex);
    std::erase(surfaces, surface);
}

void MetalDepthSurfaceCache::Clear() {
    std::lock_guard lock(mutex);
    surfaces.clear();
    depthOwners.clear();
    stencilOwners.clear();
    colorSurfaces.clear();
}

void DropReplacedDepthSurfaces(std::vector<std::shared_ptr<MetalDepthSurface>>& surfaces, MetalDepthSurfaceCache& cache,
    const Graphics::GuestTextureResource& resource) {
    const auto replaces = [&](const Graphics::DepthTarget& target, bool stencil) {
        try {
            return Graphics::IsBlockCompressed(resource.format) || Graphics::BytesPerElement(resource.format) != (stencil ? 1u : hasD16(target) ? 2u : 4u);
        } catch (const std::exception&) {
            return false; // an unknown format fails where the texture is made
        }
    };
    std::erase_if(surfaces, [&](const auto& surface) {
        const auto& target = surface->Target();
        const bool depthPlane = resource.baseAddress == target.address;
        const bool stencilPlane = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        if ((!depthPlane && !stencilPlane) || !replaces(target, stencilPlane)) return false;
        cache.Forget(surface);
        return true;
    });
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
