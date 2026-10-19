#include "MetalTexture.hpp"
#include "MetalHeapPool.hpp"
#include "GpuTiming.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>

namespace AgcDriver::Metal {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MTLPixelFormat pixelFormat(std::uint32_t format) {
    switch (Graphics::ResolveTextureFormat(format)) {
        case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
        case VK_FORMAT_R8_SRGB: return MTLPixelFormatR8Unorm_sRGB;
        case VK_FORMAT_R8_UINT: return MTLPixelFormatR8Uint;
        case VK_FORMAT_R16_UNORM: return MTLPixelFormatR16Unorm;
        case VK_FORMAT_R16_SNORM: return MTLPixelFormatR16Snorm;
        case VK_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
        case VK_FORMAT_R16_SINT: return MTLPixelFormatR16Sint;
        case VK_FORMAT_R16_SFLOAT: return MTLPixelFormatR16Float;
        case VK_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
        case VK_FORMAT_R8G8_SRGB: return MTLPixelFormatRG8Unorm_sRGB;
        case VK_FORMAT_R8G8_SNORM: return MTLPixelFormatRG8Snorm;
        case VK_FORMAT_R8G8_UINT: return MTLPixelFormatRG8Uint;
        case VK_FORMAT_R8G8_SINT: return MTLPixelFormatRG8Sint;
        case VK_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
        case VK_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
        case VK_FORMAT_R32_SFLOAT: return MTLPixelFormatR32Float;
        case VK_FORMAT_R16G16_UNORM: return MTLPixelFormatRG16Unorm;
        case VK_FORMAT_R16G16_SNORM: return MTLPixelFormatRG16Snorm;
        case VK_FORMAT_R16G16_UINT: return MTLPixelFormatRG16Uint;
        case VK_FORMAT_R16G16_SINT: return MTLPixelFormatRG16Sint;
        case VK_FORMAT_R16G16_SFLOAT: return MTLPixelFormatRG16Float;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return MTLPixelFormatRG11B10Float;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLPixelFormatRGB10A2Unorm;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return MTLPixelFormatRGB10A2Uint;
        case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SNORM: return MTLPixelFormatRGBA8Snorm;
        case VK_FORMAT_R8G8B8A8_UINT: return MTLPixelFormatRGBA8Uint;
        case VK_FORMAT_R8G8B8A8_SINT: return MTLPixelFormatRGBA8Sint;
        case VK_FORMAT_R32G32_UINT: return MTLPixelFormatRG32Uint;
        case VK_FORMAT_R32G32_SINT: return MTLPixelFormatRG32Sint;
        case VK_FORMAT_R32G32_SFLOAT: return MTLPixelFormatRG32Float;
        case VK_FORMAT_R16G16B16A16_UNORM: return MTLPixelFormatRGBA16Unorm;
        case VK_FORMAT_R16G16B16A16_SNORM: return MTLPixelFormatRGBA16Snorm;
        case VK_FORMAT_R16G16B16A16_UINT: return MTLPixelFormatRGBA16Uint;
        case VK_FORMAT_R16G16B16A16_SINT: return MTLPixelFormatRGBA16Sint;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
        case VK_FORMAT_R32G32B32A32_UINT: return MTLPixelFormatRGBA32Uint;
        case VK_FORMAT_R32G32B32A32_SINT: return MTLPixelFormatRGBA32Sint;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
        case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return MTLPixelFormatRGB9E5Float;
        case VK_FORMAT_R5G6B5_UNORM_PACK16: return MTLPixelFormatB5G6R5Unorm;
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return MTLPixelFormatBGR5A1Unorm;
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return MTLPixelFormatABGR4Unorm;
        // Guest format 136 keeps R in bits 3:0 and A in 15:12; the view swizzle below reorders ABGR4 channels.
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16: return MTLPixelFormatABGR4Unorm;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return MTLPixelFormatBC1_RGBA;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return MTLPixelFormatBC1_RGBA_sRGB;
        case VK_FORMAT_BC2_UNORM_BLOCK: return MTLPixelFormatBC2_RGBA;
        case VK_FORMAT_BC2_SRGB_BLOCK: return MTLPixelFormatBC2_RGBA_sRGB;
        case VK_FORMAT_BC3_UNORM_BLOCK: return MTLPixelFormatBC3_RGBA;
        case VK_FORMAT_BC3_SRGB_BLOCK: return MTLPixelFormatBC3_RGBA_sRGB;
        case VK_FORMAT_BC4_UNORM_BLOCK: return MTLPixelFormatBC4_RUnorm;
        case VK_FORMAT_BC4_SNORM_BLOCK: return MTLPixelFormatBC4_RSnorm;
        case VK_FORMAT_BC5_UNORM_BLOCK: return MTLPixelFormatBC5_RGUnorm;
        case VK_FORMAT_BC5_SNORM_BLOCK: return MTLPixelFormatBC5_RGSnorm;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: return MTLPixelFormatBC6H_RGBUfloat;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return MTLPixelFormatBC6H_RGBFloat;
        case VK_FORMAT_BC7_UNORM_BLOCK: return MTLPixelFormatBC7_RGBAUnorm;
        case VK_FORMAT_BC7_SRGB_BLOCK: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
        case VK_FORMAT_D16_UNORM: return MTLPixelFormatDepth16Unorm;
        case VK_FORMAT_D32_SFLOAT: return MTLPixelFormatDepth32Float;
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
        default: throw std::runtime_error("Metal texture has no supported pixel format for guest format " + std::to_string(format));
    }
}

MTLTextureSwizzle swizzle(std::uint8_t selector, std::uint32_t guestFormat) {
    if (guestFormat == 136 && selector >= 4 && selector <= 7) {
        constexpr MTLTextureSwizzle reordered[4]{MTLTextureSwizzleAlpha, MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed};
        return reordered[selector - 4u];
    }
    switch (selector) {
        case 0: return MTLTextureSwizzleZero;
        case 1: return MTLTextureSwizzleOne;
        case 4: return MTLTextureSwizzleRed;
        case 5: return MTLTextureSwizzleGreen;
        case 6: return MTLTextureSwizzleBlue;
        case 7: return MTLTextureSwizzleAlpha;
        default: throw std::runtime_error("Metal texture has an invalid guest component selector");
    }
}

MTLTextureType textureType(Graphics::TextureDimension dimension) {
    switch (dimension) {
        case Graphics::TextureDimension::k1D: return MTLTextureType2D;
        case Graphics::TextureDimension::k2D: return MTLTextureType2D;
        case Graphics::TextureDimension::k2DArray:
        case Graphics::TextureDimension::k1DArray:
        case Graphics::TextureDimension::kCube: return MTLTextureType2DArray;
        case Graphics::TextureDimension::k3D: return MTLTextureType3D;
    }
    throw std::runtime_error("Metal texture has an unknown guest dimension");
}

bool atomicFormat(std::uint32_t format) {
    const auto native = Graphics::ResolveTextureFormat(format);
    return native == VK_FORMAT_R32_UINT || native == VK_FORMAT_R32_SINT || native == VK_FORMAT_R32_SFLOAT;
}

bool packed16(std::uint32_t format) {
    const auto native = Graphics::ResolveTextureFormat(format);
    return native == VK_FORMAT_R5G6B5_UNORM_PACK16 || native == VK_FORMAT_A1R5G5B5_UNORM_PACK16 ||
        native == VK_FORMAT_R4G4B4A4_UNORM_PACK16 || native == VK_FORMAT_B4G4R4A4_UNORM_PACK16;
}

MTLPixelFormat storageFormat(std::uint32_t format) {
    switch (Graphics::ResolveTextureFormat(format)) {
        case VK_FORMAT_R8_SRGB: return MTLPixelFormatR8Unorm;
        case VK_FORMAT_R8G8_SRGB: return MTLPixelFormatRG8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm;
        default: return pixelFormat(format);
    }
}

std::uint32_t formatFamily(std::uint32_t format) {
    switch (Graphics::ResolveTextureFormat(format)) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8_UINT: return 1;
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT: return 2;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT: return 3;
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT: return 4;
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT: return 5;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 6;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB: return 7;
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT: return 8;
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 9;
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 10;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return 11;
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK: return 12;
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK: return 13;
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK: return 14;
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK: return 15;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: return 16;
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK: return 17;
        default: return 0x10000u + static_cast<std::uint32_t>(pixelFormat(format));
    }
}

void validateDescriptor(const Graphics::GuestTextureResource& descriptor, bool compare, bool minimumLodLowered) {
    require(Graphics::EffectiveMinLod(descriptor) == 0 || minimumLodLowered, "Metal texture minimum LOD view clamp is not supported");
    require(descriptor.width != 0 && descriptor.height != 0 && descriptor.mipCount != 0 && descriptor.mipCount <= 16, "Metal texture has invalid dimensions or mip count");
    require(descriptor.baseLevel <= descriptor.lastLevel && descriptor.lastLevel < descriptor.mipCount, "Metal texture view mip range lies outside the surface");
    require((descriptor.dimension != Graphics::TextureDimension::k1D && descriptor.dimension != Graphics::TextureDimension::k1DArray) ||
        descriptor.height == 1, "Metal 1D texture has a non-unit height");
    const bool xor4KB = descriptor.tileMode == Graphics::TextureTileMode::kS4KBX || descriptor.tileMode == Graphics::TextureTileMode::kD4KBX;
    require(Graphics::XorSwizzleMode(descriptor.tileMode) == 0 || (descriptor.baseAddress & (xor4KB ? 0xfffu : 0xffffu)) == 0,
        "Metal texture XOR base contains an unsupported pipe or bank XOR");
    const auto format = Graphics::ResolveTextureFormat(descriptor.format);
    require(!compare || format == VK_FORMAT_R16_UNORM || format == VK_FORMAT_R32_SFLOAT,
        "Metal comparison sampling requires R16 unorm or R32 float texture data");
    require(!compare || descriptor.dimension != Graphics::TextureDimension::k3D,
        "Metal comparison sampling does not support 3D textures");
}

// `part` describes the first layers of the array surface `whole`: same per-layer layout, fewer layers.
bool layerPrefix(const Graphics::SurfaceGeometry& whole, const Graphics::SurfaceGeometry& part) {
    if (part.layers >= whole.layers || whole.thick || part.thick || whole.imageDepth > 1 || part.imageDepth > 1 ||
        part.imageLayers != part.layers || whole.imageLayers != whole.layers || part.layerBytes != whole.layerBytes ||
        part.sliceLinearBytes != whole.sliceLinearBytes || part.blockDepth != whole.blockDepth || part.guestBytes > whole.guestBytes ||
        part.mips.size() != whole.mips.size()) return false;
    for (std::size_t i = 0; i < part.mips.size(); ++i) {
        const auto& x = whole.mips[i];
        const auto& y = part.mips[i];
        if (std::tie(x.tiledOffset, x.tiledSize, x.linearOffset, x.linearSize, x.width, x.height, x.blocksPerRow, x.pitchBytes, x.tail, x.tailX, x.tailY) !=
            std::tie(y.tiledOffset, y.tiledSize, y.linearOffset, y.linearSize, y.width, y.height, y.blocksPerRow, y.pitchBytes, y.tail, y.tailX, y.tailY)) return false;
    }
    return true;
}

bool sameGeometry(const Graphics::SurfaceGeometry& a, const Graphics::SurfaceGeometry& b) {
    if (std::tie(a.layers, a.imageLayers, a.imageDepth, a.guestBytes, a.sliceLinearBytes, a.thick, a.blockDepth, a.layerBytes) !=
        std::tie(b.layers, b.imageLayers, b.imageDepth, b.guestBytes, b.sliceLinearBytes, b.thick, b.blockDepth, b.layerBytes) ||
        a.mips.size() != b.mips.size()) return false;
    for (std::size_t i = 0; i < a.mips.size(); ++i) {
        const auto& x = a.mips[i];
        const auto& y = b.mips[i];
        if (std::tie(x.tiledOffset, x.tiledSize, x.linearOffset, x.linearSize, x.width, x.height, x.blocksPerRow, x.pitchBytes, x.tail, x.tailX, x.tailY) !=
            std::tie(y.tiledOffset, y.tiledSize, y.linearOffset, y.linearSize, y.width, y.height, y.blocksPerRow, y.pitchBytes, y.tail, y.tailX, y.tailY)) return false;
    }
    return true;
}

struct Parameters {
    std::uint32_t srcBase;
    std::uint32_t dstBase;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitchBytes;
    std::uint32_t blocksPerRow;
    std::uint32_t tail;
    std::uint32_t tailX;
    std::uint32_t tailY;
    std::uint32_t elementBytes;
    std::uint32_t slice;
    std::uint32_t rangeBegin;
    std::uint32_t rangeEnd;
    std::uint32_t tiledBase;
    std::uint32_t linearBase;
    std::uint32_t columnBegin;
    std::uint32_t rowBegin;
    std::uint32_t pipeBankXor;
};

std::size_t bufferSize(std::uint64_t bytes) {
    require(bytes != 0 && bytes <= UINT32_MAX - 3u, "Metal texture buffer exceeds 32-bit detile addressing");
    return (static_cast<std::size_t>(bytes) + 3u) & ~std::size_t(3u);
}

}

MetalTexture::MetalTexture(const MetalDevice& backend, std::span<const std::uint32_t> words, bool compare)
    : MetalTexture(backend, Graphics::DecodeTextureResource(words), compare) {}

MetalTexture::MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor, bool compare)
    : MetalTexture(backend, descriptor, compare, false) {}

MetalTexture::MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor,
    bool compare, bool minimumLodLowered)
    : backend(backend), descriptor(descriptor), backing(std::make_shared<Backing>()) {
    validateDescriptor(descriptor, compare, minimumLodLowered);
    const bool compressed = Graphics::IsBlockCompressed(descriptor.format);
    require(!compressed || backend.Device().supportsBCTextureCompression, "Metal device does not support BC compressed textures");
    backing->descriptor = descriptor;
    backing->elementBytes = Graphics::BytesPerElement(descriptor.format);
    require(std::has_single_bit(backing->elementBytes) && backing->elementBytes <= 16, "Metal texture element format is not supported");
    backing->geometry = Graphics::DescribeSurface(descriptor);
    bufferSize(backing->geometry.guestBytes);
    bufferSize(backing->geometry.sliceLinearBytes * backing->geometry.layers);
    require(descriptor.baseArray < backing->geometry.imageLayers, "Metal texture view base array lies outside the surface");
    auto native = [MTLTextureDescriptor new];
    native.textureType = textureType(descriptor.dimension);
    native.pixelFormat = atomicFormat(descriptor.format) ? MTLPixelFormatR32Uint : storageFormat(descriptor.format);
    native.width = descriptor.width;
    native.height = descriptor.height;
    native.depth = backing->geometry.imageDepth;
    native.arrayLength = backing->geometry.imageLayers;
    native.mipmapLevelCount = descriptor.mipCount;
    native.storageMode = MTLStorageModeShared;
    native.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (!compressed && !packed16(descriptor.format)) native.usage |= MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    if (atomicFormat(descriptor.format)) native.usage |= MTLTextureUsageShaderAtomic;
    backing->texture = MetalHeapPool::Enabled() ? MetalHeapPool::Shared().SuballocateTexture(backend.Device(), native) : nil;
    if (backing->texture == nil) {
        backing->texture = [backend.Device() newTextureWithDescriptor:native];
    }
    require(backing->texture != nil, "Metal cannot allocate the guest texture format or dimensions");
    createViews(compare);
}

MetalTexture::MetalTexture(const MetalDevice& backend, std::shared_ptr<Backing> backing,
    const Graphics::GuestTextureResource& descriptor, bool compare, bool minimumLodLowered)
    : backend(backend), descriptor(descriptor), backing(std::move(backing)) {
    validateDescriptor(descriptor, compare, minimumLodLowered);
    createViews(compare);
}

namespace {

static_assert(sizeof(Graphics::GuestTextureResource) == 88, "GuestTextureResource changed: update the texture view key");

std::array<std::uint64_t, 21> viewKey(const Graphics::GuestTextureResource& d, bool compare) {
    return {d.baseAddress, d.width, d.height, d.depthOrLastArray, d.baseArray, d.mipCount, d.baseLevel,
        static_cast<std::uint64_t>(d.tileMode), static_cast<std::uint64_t>(d.dimension), d.format,
        std::uint64_t{d.dstSelX} | std::uint64_t{d.dstSelY} << 8u | std::uint64_t{d.dstSelZ} << 16u | std::uint64_t{d.dstSelW} << 24u,
        d.pipeBankXor, d.bcSwizzle, d.lastLevel, d.dccAddress, d.dccAlphaOnMsb, d.dccPipeAligned, d.minLod,
        d.allocatedMipCount, d.samples, compare ? 1u : 0u};
}

// Views kept per backing; a surface is rarely bound through more descriptors than this.
constexpr std::size_t MaxBackingViews = 32;

}

void MetalTexture::createViews(bool compare) {
    // The views depend only on the backing, this descriptor and `compare`: reuse the ones made before.
    const auto key = viewKey(descriptor, compare);
    {
        std::lock_guard lock(backing->viewsMutex);
        for (const auto& [known, views] : backing->views) {
            if (known != key) continue;
            typedTexture = views.typed;
            sampledView = views.sampled;
            storageView = views.storage;
            atomicView = views.atomic;
            return;
        }
    }
    makeViews(compare);
    std::lock_guard lock(backing->viewsMutex);
    if (backing->views.size() >= MaxBackingViews) backing->views.erase(backing->views.begin());
    backing->views.push_back({key, Views{typedTexture, sampledView, storageView, atomicView}});
}

void MetalTexture::makeViews(bool compare) {
    const auto& geometry = backing->geometry;
    require(descriptor.baseArray < geometry.imageLayers, "Metal texture view base array lies outside the surface");
    const auto nativeFormat = pixelFormat(descriptor.format);
    if (nativeFormat == MTLPixelFormatR8Unorm_sRGB || nativeFormat == MTLPixelFormatRG8Unorm_sRGB) {
        require([backend.Device() supportsFamily:MTLGPUFamilyApple2], "Metal R8/RG8 sRGB texture views require an Apple GPU");
    }
    const auto nativeType = backing->texture.textureType;
    typedTexture = nativeFormat == backing->texture.pixelFormat ? backing->texture :
        [backing->texture newTextureViewWithPixelFormat:nativeFormat];
    require(typedTexture != nil, "Metal cannot create the guest texture format view");
    const auto levels = NSMakeRange(descriptor.baseLevel, descriptor.lastLevel - descriptor.baseLevel + 1u);
    // A view of fewer layers than its backing (CanShareBacking) still ends at its own last layer.
    const auto layers = descriptor.depthOrLastArray == backing->descriptor.depthOrLastArray ? geometry.imageLayers :
        std::min(geometry.imageLayers, Graphics::DescribeSurface(descriptor).imageLayers);
    require(descriptor.baseArray < layers, "Metal texture view base array lies outside its layers");
    const auto slices = NSMakeRange(descriptor.baseArray, layers - descriptor.baseArray);
    if (compare) {
        sampledView = [backing->texture newTextureViewWithPixelFormat:nativeFormat textureType:nativeType levels:levels slices:slices];
    } else {
        const auto components = MTLTextureSwizzleChannelsMake(swizzle(descriptor.dstSelX, descriptor.format), swizzle(descriptor.dstSelY, descriptor.format), swizzle(descriptor.dstSelZ, descriptor.format), swizzle(descriptor.dstSelW, descriptor.format));
        sampledView = [backing->texture newTextureViewWithPixelFormat:nativeFormat textureType:nativeType levels:levels slices:slices swizzle:components];
    }
    require(sampledView != nil, "Metal cannot create the guest texture mip, array, or swizzle view");
    if (SupportsStorage()) {
        storageView = [backing->texture newTextureViewWithPixelFormat:storageFormat(descriptor.format) textureType:nativeType levels:NSMakeRange(descriptor.baseLevel, 1) slices:slices];
        require(storageView != nil, "Metal cannot create the guest storage texture view");
    }
    if (SupportsAtomic()) {
        atomicView = [backing->texture newTextureViewWithPixelFormat:MTLPixelFormatR32Uint textureType:nativeType levels:NSMakeRange(descriptor.baseLevel, 1) slices:slices];
        require(atomicView != nil, "Metal cannot create the guest atomic texture view");
    }
}

bool MetalTexture::CanShareBacking(const Graphics::GuestTextureResource& resource) const {
    return canShareBacking(resource, false);
}

bool MetalTexture::CanShareBackingIgnoringDcc(const Graphics::GuestTextureResource& resource) const {
    return canShareBacking(resource, true);
}

bool MetalTexture::canShareBacking(const Graphics::GuestTextureResource& resource, bool ignoreDcc) const {
    const auto& original = backing->descriptor;
    if (std::tie(original.baseAddress, original.width, original.height, original.mipCount, original.tileMode, original.dimension, original.pipeBankXor) !=
        std::tie(resource.baseAddress, resource.width, resource.height, resource.mipCount, resource.tileMode, resource.dimension, resource.pipeBankXor)) return false;
    if (!ignoreDcc && std::tie(original.dccAddress, original.dccAlphaOnMsb, original.dccPipeAligned) !=
        std::tie(resource.dccAddress, resource.dccAlphaOnMsb, resource.dccPipeAligned)) return false;
    const auto originalAllocation = original.allocatedMipCount == 0 ? original.mipCount : original.allocatedMipCount;
    const auto allocation = resource.allocatedMipCount == 0 ? resource.mipCount : resource.allocatedMipCount;
    if (originalAllocation != allocation || formatFamily(original.format) != formatFamily(resource.format)) return false;
    if (original.depthOrLastArray == resource.depthOrLastArray) return sameGeometry(backing->geometry, Graphics::DescribeSurface(resource));
    // A view of the first layers of an array surface (a per-slice pass binds "array k-k" with k + 1
    // layers): those layers lie at the same guest offsets in the whole surface's backing.
    return resource.depthOrLastArray < original.depthOrLastArray && (ignoreDcc || original.dccAddress == 0) &&
        layerPrefix(backing->geometry, Graphics::DescribeSurface(resource));
}

std::string MetalTexture::ShareMismatch(const Graphics::GuestTextureResource& resource) const {
    const auto& original = backing->descriptor;
    std::string why;
    const auto field = [&](const char* name, std::uint64_t a, std::uint64_t b) {
        if (a != b) why += std::string(why.empty() ? "" : ", ") + name + " " + std::to_string(a) + " vs " + std::to_string(b);
    };
    field("address", original.baseAddress, resource.baseAddress);
    field("width", original.width, resource.width);
    field("height", original.height, resource.height);
    field("last array", original.depthOrLastArray, resource.depthOrLastArray);
    field("mips", original.mipCount, resource.mipCount);
    field("allocated mips", original.allocatedMipCount, resource.allocatedMipCount);
    field("tile", static_cast<std::uint64_t>(original.tileMode), static_cast<std::uint64_t>(resource.tileMode));
    field("dimension", static_cast<std::uint64_t>(original.dimension), static_cast<std::uint64_t>(resource.dimension));
    field("dcc", original.dccAddress, resource.dccAddress);
    field("pipeBankXor", original.pipeBankXor, resource.pipeBankXor);
    field("format family", formatFamily(original.format), formatFamily(resource.format));
    if (why.empty()) why = CanShareBacking(resource) ? "shareable" : "surface geometry differs";
    return why;
}

std::shared_ptr<MetalTexture> MetalTexture::CreateView(const Graphics::GuestTextureResource& resource, bool compare) const {
    return CreateView(resource, compare, false);
}

std::shared_ptr<MetalTexture> MetalTexture::CreateView(const Graphics::GuestTextureResource& resource,
    bool compare, bool minimumLodLowered, bool ignoreDcc) const {
    require(canShareBacking(resource, ignoreDcc), "Metal texture view is incompatible with the captured backing");
    return std::shared_ptr<MetalTexture>(new MetalTexture(backend, backing, resource, compare, minimumLodLowered));
}

std::array<std::uint32_t, 22> MetalTexture::transferConstants(bool retile) const {
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    const auto elementBytes = backing->elementBytes;
    std::array<std::uint32_t, 22> constants{elementBytes, 0, descriptor.tileMode == Graphics::TextureTileMode::kLinear ? 0u : 1u, retile ? 1u : 0u};
    if (constants[2] != 0) constants[1] = Graphics::ThinBlockLayout(descriptor.tileMode, elementBytes)[0];
    if (geometry.thick) {
        // Same thick equation choice as the Vulkan TextureDetiler: SW_4KB_S, SW_64KB_S or SW_64KB_S_X.
        const auto mode = descriptor.tileMode == Graphics::TextureTileMode::kStandard4KB ? 0x105u :
            descriptor.tileMode == Graphics::TextureTileMode::kStandard64KB ? 0x109u :
            descriptor.tileMode == Graphics::TextureTileMode::kS64KBX ? 0x119u : 0u;
        require(mode != 0, "Metal texture thick 3D layout requires SW_4KB_S, SW_64KB_S or SW_64KB_S_X");
        const auto* equation = Graphics::FindTextureSwizzleEquation(mode, elementBytes);
        require(equation != nullptr, "Metal texture has no thick swizzle equation");
        constants[2] = 2;
        std::copy(equation->bits.begin(), equation->bits.end(), constants.begin() + 4);
        const auto extent = Graphics::ThickBlockExtent(descriptor.tileMode, elementBytes);
        constants[20] = extent[0];
        constants[21] = extent[1];
    } else if (const auto mode = Graphics::EquationSwizzleMode(descriptor.tileMode); mode != 0) {
        // The display (D) modes have no XOR but still need their own equation; the S family's standard
        // offset in TextureDetile.metal would scramble them.
        const auto* equation = Graphics::FindTextureSwizzleEquation(mode, elementBytes);
        require(equation != nullptr, "Metal texture has no swizzle equation for its tile mode");
        constants[2] = 2;
        std::copy(equation->bits.begin(), equation->bits.end(), constants.begin() + 4);
    }
    return constants;
}

void MetalTexture::encodeTransfer(id<MTLCommandBuffer> commands, id<MTLBuffer> source, NSUInteger sourceOffset,
    id<MTLBuffer> destination, NSUInteger destinationOffset, bool retile, Subresources part) const {
    const std::array<NSUInteger, 2> offsets{sourceOffset, destinationOffset};
    const auto& geometry = backing->geometry;
    const auto elementBytes = backing->elementBytes;
    const auto constants = transferConstants(retile);
    // Every level and layer in one compute encoder (an encoder each was most of a frame's encoders).
    id<MTLComputeCommandEncoder> encoder = nil;
    try {
        for (std::uint32_t level = part.levelBegin; level < geometry.mips.size() && level < part.levelEnd; ++level) {
            const auto& mip = geometry.mips[level];
            for (std::uint32_t layer = part.layerBegin; layer < geometry.layers && layer < part.layerEnd; ++layer) {
                if (!geometry.HasLayer(level, layer)) continue;
                const auto tiled = geometry.GuestLayerOffset(layer) + mip.tiledOffset;
                const auto linear = geometry.LinearLayerOffset(layer) + mip.linearOffset;
                Parameters p{};
                p.srcBase = static_cast<std::uint32_t>(retile ? linear : tiled);
                p.dstBase = static_cast<std::uint32_t>(retile ? tiled : linear);
                p.width = mip.width;
                p.height = mip.height;
                p.pitchBytes = mip.pitchBytes;
                p.blocksPerRow = mip.blocksPerRow;
                p.tail = mip.tail;
                p.tailX = mip.tailX;
                p.tailY = mip.tailY;
                p.elementBytes = elementBytes;
                p.slice = layer;
                p.rangeEnd = UINT32_MAX;
                p.pipeBankXor = backing->descriptor.pipeBankXor;
                if (mip.width == 0 || mip.height == 0) continue;
                if (encoder == nil) encoder = backend.ComputeEncoder(commands, retile ? @"readback retile" : @"upload detile");
                backend.Dispatch(encoder, @"TextureDetile", @[source, destination], &p, sizeof(p), MTLSizeMake(mip.width, mip.height, 1), &constants, offsets);
            }
        }
    } catch (...) {
        if (encoder != nil) [encoder endEncoding];
        throw;
    }
    if (encoder != nil) [encoder endEncoding];
}

namespace {

// The unsigned format of one element's raw bits (one component per 32 bits; R8/R16 below a word).
MTLPixelFormat rawFormat(std::uint32_t elementBytes) {
    switch (elementBytes) {
        case 1: return MTLPixelFormatR8Uint;
        case 2: return MTLPixelFormatR16Uint;
        case 4: return MTLPixelFormatR32Uint;
        case 8: return MTLPixelFormatRG32Uint;
        case 16: return MTLPixelFormatRGBA32Uint;
        default: return MTLPixelFormatInvalid;
    }
}

bool fusedTransferEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_METAL_FUSED_TRANSFER");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

struct TransferParameters {
    Parameters tile;
    std::uint32_t textureSlice;
    std::uint32_t level;
};

}

id<MTLTexture> MetalTexture::rawView() const {
    std::lock_guard lock(backing->viewsMutex);
    if (backing->rawViewMade) return backing->rawView;
    backing->rawViewMade = true;
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    id<MTLTexture> texture = backing->texture;
    const auto format = rawFormat(backing->elementBytes);
    if (!fusedTransferEnabled() || format == MTLPixelFormatInvalid || Graphics::IsBlockCompressed(descriptor.format) ||
        geometry.thick || geometry.imageDepth > 1 || texture.sampleCount != 1 ||
        (texture.textureType != MTLTextureType2D && texture.textureType != MTLTextureType2DArray) ||
        (texture.usage & MTLTextureUsagePixelFormatView) == 0) return nil;
    // Each level's elements must be its texels: the fused pass addresses the texture by element.
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        if (level >= texture.mipmapLevelCount || mip.width != std::max(descriptor.width >> level, 1u) ||
            mip.height != std::max(descriptor.height >> level, 1u)) return nil;
    }
    if (geometry.layers > texture.arrayLength) return nil;
    backing->rawView = [texture newTextureViewWithPixelFormat:format textureType:MTLTextureType2DArray
        levels:NSMakeRange(0, texture.mipmapLevelCount) slices:NSMakeRange(0, texture.arrayLength)];
    return backing->rawView;
}

bool MetalTexture::FusesTransfer(bool upload) const {
    if (upload && (backing->texture.usage & MTLTextureUsageShaderWrite) == 0) return false;
    return rawView() != nil;
}

void MetalTexture::encodeFused(id<MTLComputeCommandEncoder> encoder, id<MTLBuffer> guest, NSUInteger offset, bool retile, Subresources part) const {
    id<MTLTexture> view = rawView();
    require(view != nil, "Metal texture does not fuse its transfers");
    const auto& geometry = backing->geometry;
    const auto constants = transferConstants(retile);
    const std::array<NSUInteger, 1> offsets{offset};
    for (std::uint32_t level = part.levelBegin; level < geometry.mips.size() && level < part.levelEnd; ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = part.layerBegin; layer < geometry.layers && layer < part.layerEnd; ++layer) {
            if (!geometry.HasLayer(level, layer) || mip.width == 0 || mip.height == 0) continue;
            const auto tiled = geometry.GuestLayerOffset(layer) + mip.tiledOffset;
            TransferParameters p{};
            p.tile.srcBase = static_cast<std::uint32_t>(tiled);
            p.tile.dstBase = static_cast<std::uint32_t>(tiled);
            p.tile.width = mip.width;
            p.tile.height = mip.height;
            p.tile.pitchBytes = mip.pitchBytes;
            p.tile.blocksPerRow = mip.blocksPerRow;
            p.tile.tail = mip.tail;
            p.tile.tailX = mip.tailX;
            p.tile.tailY = mip.tailY;
            p.tile.elementBytes = backing->elementBytes;
            p.tile.slice = layer;
            p.tile.rangeEnd = UINT32_MAX;
            p.tile.pipeBankXor = backing->descriptor.pipeBankXor;
            p.textureSlice = geometry.CopyLayer(layer);
            p.level = level;
            backend.Dispatch(encoder, retile ? @"TextureRetileFromTexture" : @"TextureDetileToTexture", @[guest], &p, sizeof(p),
                MTLSizeMake(mip.width, mip.height, 1), &constants, offsets, view);
        }
    }
}

// The copies stay on the GPU: replaceRegion/getBytes convert to the texture's internal layout on the
// CPU inside the Metal driver, which on the Rosetta direct path runs translated (a third of the queue
// worker's time went there for surfaces uploaded and read back around each draw or dispatch).
void MetalTexture::encodeLinearCopy(id<MTLCommandBuffer> commands, id<MTLBuffer> linear, bool toTexture, Subresources part) const {
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    id<MTLTexture> texture = backing->texture;
    id<MTLBlitCommandEncoder> blit = MetalEncoders::Blit(commands);
    if (GpuTiming::Enabled() && blit != nil) blit.label = toTexture ? @"upload linear copy" : @"readback linear copy";
    require(blit != nil, "Metal texture blit encoder creation failed");
    const bool volume = texture.textureType == MTLTextureType3D;
    for (std::uint32_t level = part.levelBegin; level < geometry.mips.size() && level < part.levelEnd; ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = part.layerBegin; layer < geometry.layers && layer < part.layerEnd; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto size = MTLSizeMake(std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1);
            const auto origin = MTLOriginMake(0, 0, geometry.CopyDepth(layer));
            const auto offset = geometry.LinearLayerOffset(layer) + mip.linearOffset;
            const NSUInteger bytesPerImage = volume ? mip.linearSize : 0;
            if (toTexture) {
                [blit copyFromBuffer:linear sourceOffset:offset sourceBytesPerRow:mip.pitchBytes sourceBytesPerImage:bytesPerImage sourceSize:size
                           toTexture:texture destinationSlice:geometry.CopyLayer(layer) destinationLevel:level destinationOrigin:origin];
            } else {
                [blit copyFromTexture:texture sourceSlice:geometry.CopyLayer(layer) sourceLevel:level sourceOrigin:origin sourceSize:size
                             toBuffer:linear destinationOffset:offset destinationBytesPerRow:mip.pitchBytes destinationBytesPerImage:bytesPerImage];
            }
        }
    }
    [blit endEncoding];
}

void MetalTexture::recordLinearUpload(id<MTLBuffer> linear, MetalEncoders::UploadBlitRecord& record) const {
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    id<MTLTexture> texture = backing->texture;
    record.sourceBuffer = linear;
    record.destinationTexture = texture;
    const bool volume = texture.textureType == MTLTextureType3D;
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = 0; layer < geometry.layers; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto size = MTLSizeMake(std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1);
            const auto origin = MTLOriginMake(0, 0, geometry.CopyDepth(layer));
            const auto offset = geometry.LinearLayerOffset(layer) + mip.linearOffset;
            const NSUInteger bytesPerImage = volume ? mip.linearSize : 0;
            record.steps.push_back({offset, mip.pitchBytes, bytesPerImage, size,
                geometry.CopyLayer(layer), level, origin});
        }
    }
}

bool MetalTexture::inPlace(id<MTLBuffer> buffer, std::size_t offset) const {
    // The detile addresses whole words, so the surface must be word-aligned and a whole number of words.
    const auto extent = bufferSize(backing->geometry.guestBytes);
    return buffer != nil && buffer.device == backend.Device() && offset % 4 == 0 && extent == backing->geometry.guestBytes &&
        offset <= buffer.length && extent <= buffer.length - offset;
}

void MetalTexture::Upload(std::span<const std::byte> guestBytes) {
    const auto& geometry = backing->geometry;
    require(guestBytes.size() == geometry.guestBytes, "Metal texture upload byte extent differs from guest surface");
    // A recycled staging buffer (fully rewritten here), back to the pool once the upload has run.
    auto tiled = MetalBufferPool::Shared().Take(backend.Device(), bufferSize(geometry.guestBytes));
    std::memcpy(tiled.contents, guestBytes.data(), guestBytes.size());
    std::memset(static_cast<std::byte*>(tiled.contents) + guestBytes.size(), 0, tiled.length - guestBytes.size());
    // Work that samples the texture runs after the upload.
    uploadFrom(tiled, 0, true);
}

void MetalTexture::Upload(id<MTLBuffer> guest, std::size_t offset) {
    const auto& geometry = backing->geometry;
    if (!inPlace(guest, offset)) {
        require(guest != nil && offset <= guest.length && geometry.guestBytes <= guest.length - offset,
            "Metal texture upload source does not hold the guest surface");
        Upload(std::span(static_cast<const std::byte*>(guest.contents) + offset, static_cast<std::size_t>(geometry.guestBytes)));
        return;
    }
    uploadFrom(guest, offset);
}

void MetalTexture::uploadFrom(id<MTLBuffer> guest, std::size_t offset, bool recycleGuest) {
    GpuTiming::TagScope tag(GpuTiming::MakeTag(GpuTiming::Kind::Upload, descriptor.baseAddress));
    const auto& geometry = backing->geometry;
    // The queue runs uploads in commit order: no need to wait for an earlier one of this texture.
    pendingUpload = nil;
    auto commands = backend.CommandBuffer();
    if (recycleGuest) MetalBufferPool::Shared().GiveWhenCompleted(guest, commands);
    if (FusesTransfer(true)) {
        // One compute encoder writes every level and layer straight into the texture.
        id<MTLComputeCommandEncoder> encoder = backend.ComputeEncoder(commands, @"upload detile to texture");
        try {
            encodeFused(encoder, guest, offset, false);
        } catch (...) {
            [encoder endEncoding];
            throw;
        }
        [encoder endEncoding];
    } else {
        // The blit reads only texels the detile wrote, so the (recycled) linear buffer needs no clearing.
        auto linear = backend.TransientBuffer(bufferSize(geometry.sliceLinearBytes * geometry.layers), commands);
        encodeTransfer(commands, guest, offset, linear, 0, false);
        MetalEncoders::UploadBlitRecord record;
        recordLinearUpload(linear, record);
        MetalEncoders::EnqueueUploadBlit(commands, std::move(record));
    }
    // The draw or dispatch that samples the texture is committed later on the same queue (or is
    // encoded after this into the same, batched, command buffer), so it runs after this; the
    // command buffer keeps the staging buffers alive until then.
    backend.Submit(commands);
    pendingUpload = commands;
}

void MetalTexture::FinishUpload() const {
    if (pendingUpload == nil) return;
    id<MTLCommandBuffer> commands = pendingUpload;
    pendingUpload = nil;
    GpuTiming::Scope timing(GpuTiming::Kind::Upload);
    backend.Wait(commands);
}

void MetalTexture::Readback(std::span<std::byte> guestBytes) const {
    const auto& geometry = backing->geometry;
    require(guestBytes.size() == geometry.guestBytes, "Metal texture readback byte extent differs from guest surface");
    // A recycled staging buffer (fully rewritten here); readbackInto waits for the GPU.
    auto tiled = MetalBufferPool::Shared().Take(backend.Device(), bufferSize(geometry.guestBytes));
    std::memcpy(tiled.contents, guestBytes.data(), guestBytes.size());
    std::memset(static_cast<std::byte*>(tiled.contents) + guestBytes.size(), 0, tiled.length - guestBytes.size());
    readbackInto(tiled, 0);
    std::memcpy(guestBytes.data(), tiled.contents, guestBytes.size());
    MetalBufferPool::Shared().Give(tiled);
}

void MetalTexture::Readback(id<MTLBuffer> guest, std::size_t offset) const {
    const auto& geometry = backing->geometry;
    if (!inPlace(guest, offset)) {
        require(guest != nil && offset <= guest.length && geometry.guestBytes <= guest.length - offset,
            "Metal texture readback destination does not hold the guest surface");
        Readback(std::span(static_cast<std::byte*>(guest.contents) + offset, static_cast<std::size_t>(geometry.guestBytes)));
        return;
    }
    readbackInto(guest, offset);
}

void MetalTexture::readbackInto(id<MTLBuffer> guest, std::size_t offset) const {
    GpuTiming::TagScope tag(GpuTiming::MakeTag(GpuTiming::Kind::Readback, descriptor.baseAddress));
    const auto& geometry = backing->geometry;
    FinishUpload();
    // The queue runs command buffers in commit order, so the copy sees the texture's earlier writes;
    // the retile rewrites only the surface's texels in place.
    auto commands = backend.CommandBuffer();
    if (!EncodeReadback(commands, guest, offset, {})) {
        // Not in place (a staging buffer of the span overload is): through the linear buffer. The
        // retile reads only the elements the blit wrote, so it needs no clearing either.
        auto linear = MetalBufferPool::Shared().Take(backend.Device(), bufferSize(geometry.sliceLinearBytes * geometry.layers));
        MetalBufferPool::Shared().GiveWhenCompleted(linear, commands);
        encodeLinearCopy(commands, linear, false);
        encodeTransfer(commands, linear, 0, guest, offset, true);
    }
    GpuTiming::Scope timing(GpuTiming::Kind::Readback);
    backend.Wait(commands);
}

bool MetalTexture::EncodeReadback(id<MTLCommandBuffer> commands, id<MTLBuffer> guest, std::size_t offset) const {
    return EncodeReadback(commands, guest, offset, {});
}

bool MetalTexture::EncodeReadback(id<MTLCommandBuffer> commands, id<MTLBuffer> guest, std::size_t offset, Subresources part) const {
    if (!inPlace(guest, offset)) return false;
    // Uploads are committed, or encoded into the same batched command buffer, before this runs.
    if (FusesTransfer(false)) {
        id<MTLComputeCommandEncoder> encoder = backend.ComputeEncoder(commands, @"readback retile from texture");
        try {
            encodeFused(encoder, guest, offset, true, part);
        } catch (...) {
            [encoder endEncoding];
            throw;
        }
        [encoder endEncoding];
        return true;
    }
    // The retile reads only the elements the blit wrote, so the recycled linear buffer needs no clearing.
    auto linear = backend.TransientBuffer(bufferSize(backing->geometry.sliceLinearBytes * backing->geometry.layers), commands);
    encodeLinearCopy(commands, linear, false, part);
    encodeTransfer(commands, linear, 0, guest, offset, true, part);
    return true;
}

bool MetalTexture::EncodeReadback(id<MTLComputeCommandEncoder> encoder, id<MTLBuffer> guest, std::size_t offset, Subresources part) const {
    if (!inPlace(guest, offset) || !FusesTransfer(false)) return false;
    encodeFused(encoder, guest, offset, true, part);
    return true;
}

id<MTLTexture> MetalTexture::Texture() const { return typedTexture; }
id<MTLTexture> MetalTexture::SampledView() const { return sampledView; }
id<MTLTexture> MetalTexture::StorageView(bool atomic) const {
    require(descriptor.minLod <= descriptor.baseLevel * 256u, "guest storage texture descriptor clamps its minimum LOD above the level it addresses, which is not implemented");
    require(SupportsStorage(), "Metal texture format does not support native storage or render writes");
    require(!atomic || SupportsAtomic(), "Metal texture atomics require one 32-bit component");
    return atomic ? atomicView : storageView;
}
id<MTLTexture> MetalTexture::RawSintStorageView() const {
    static_cast<void>(StorageView());
    require(Graphics::ResolveTextureFormat(descriptor.format) == VK_FORMAT_R32_SINT,
        "Metal raw signed storage view requires one signed 32-bit component");
    require(atomicView != nil && atomicView.pixelFormat == MTLPixelFormatR32Uint,
        "Metal raw signed storage view requires an unsigned native format view");
    return atomicView;
}
bool MetalTexture::SupportsStorage() const { return !Graphics::IsBlockCompressed(descriptor.format) && !packed16(descriptor.format); }
bool MetalTexture::SupportsAtomic() const { return atomicFormat(descriptor.format); }
const Graphics::GuestTextureResource& MetalTexture::Descriptor() const { return descriptor; }
const Graphics::SurfaceGeometry& MetalTexture::Geometry() const { return backing->geometry; }
std::size_t MetalTexture::GuestBytes() const { return static_cast<std::size_t>(backing->geometry.guestBytes); }

}
