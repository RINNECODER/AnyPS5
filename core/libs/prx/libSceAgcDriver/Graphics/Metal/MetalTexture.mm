#include "MetalTexture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include <algorithm>
#include <bit>
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
        default: throw std::runtime_error("Metal texture has no supported pixel format for guest format " + std::to_string(format));
    }
}

MTLTextureSwizzle swizzle(std::uint8_t selector) {
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
        native == VK_FORMAT_R4G4B4A4_UNORM_PACK16;
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
    require(Graphics::XorSwizzleMode(descriptor.tileMode) == 0 || (descriptor.baseAddress & 0xffffu) == 0,
        "Metal texture XOR base contains an unsupported pipe or bank XOR");
    const auto format = Graphics::ResolveTextureFormat(descriptor.format);
    require(!compare || format == VK_FORMAT_R16_UNORM || format == VK_FORMAT_R32_SFLOAT,
        "Metal comparison sampling requires R16 unorm or R32 float texture data");
    require(!compare || descriptor.dimension != Graphics::TextureDimension::k3D,
        "Metal comparison sampling does not support 3D textures");
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
    backing->texture = [backend.Device() newTextureWithDescriptor:native];
    require(backing->texture != nil, "Metal cannot allocate the guest texture format or dimensions");
    createViews(compare);
}

MetalTexture::MetalTexture(const MetalDevice& backend, std::shared_ptr<Backing> backing,
    const Graphics::GuestTextureResource& descriptor, bool compare, bool minimumLodLowered)
    : backend(backend), descriptor(descriptor), backing(std::move(backing)) {
    validateDescriptor(descriptor, compare, minimumLodLowered);
    createViews(compare);
}

void MetalTexture::createViews(bool compare) {
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
    const auto slices = NSMakeRange(descriptor.baseArray, geometry.imageLayers - descriptor.baseArray);
    if (compare) {
        sampledView = [backing->texture newTextureViewWithPixelFormat:nativeFormat textureType:nativeType levels:levels slices:slices];
    } else {
        const auto components = MTLTextureSwizzleChannelsMake(swizzle(descriptor.dstSelX), swizzle(descriptor.dstSelY), swizzle(descriptor.dstSelZ), swizzle(descriptor.dstSelW));
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
    const auto& original = backing->descriptor;
    if (std::tie(original.baseAddress, original.width, original.height, original.depthOrLastArray, original.mipCount, original.tileMode, original.dimension, original.dccAddress, original.dccAlphaOnMsb, original.dccPipeAligned) !=
        std::tie(resource.baseAddress, resource.width, resource.height, resource.depthOrLastArray, resource.mipCount, resource.tileMode, resource.dimension, resource.dccAddress, resource.dccAlphaOnMsb, resource.dccPipeAligned)) return false;
    const auto originalAllocation = original.allocatedMipCount == 0 ? original.mipCount : original.allocatedMipCount;
    const auto allocation = resource.allocatedMipCount == 0 ? resource.mipCount : resource.allocatedMipCount;
    return originalAllocation == allocation && formatFamily(original.format) == formatFamily(resource.format) &&
        sameGeometry(backing->geometry, Graphics::DescribeSurface(resource));
}

std::shared_ptr<MetalTexture> MetalTexture::CreateView(const Graphics::GuestTextureResource& resource, bool compare) const {
    return CreateView(resource, compare, false);
}

std::shared_ptr<MetalTexture> MetalTexture::CreateView(const Graphics::GuestTextureResource& resource,
    bool compare, bool minimumLodLowered) const {
    require(CanShareBacking(resource), "Metal texture view is incompatible with the captured backing");
    return std::shared_ptr<MetalTexture>(new MetalTexture(backend, backing, resource, compare, minimumLodLowered));
}

void MetalTexture::transfer(id<MTLBuffer> source, id<MTLBuffer> destination, bool retile) const {
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    const auto elementBytes = backing->elementBytes;
    std::array<std::uint32_t, 22> constants{elementBytes, 0, descriptor.tileMode == Graphics::TextureTileMode::kLinear ? 0u : 1u, retile ? 1u : 0u};
    if (constants[2] != 0) constants[1] = Graphics::ThinBlockLayout(descriptor.tileMode, elementBytes)[0];
    if (geometry.thick) {
        const auto mode = descriptor.tileMode == Graphics::TextureTileMode::kStandard4KB ? 0x105u : 0x109u;
        const auto* equation = Graphics::FindTextureSwizzleEquation(mode, elementBytes);
        require(equation != nullptr, "Metal texture has no thick swizzle equation");
        constants[2] = 2;
        std::copy(equation->bits.begin(), equation->bits.end(), constants.begin() + 4);
        const auto extent = Graphics::ThickBlockExtent(descriptor.tileMode, elementBytes);
        constants[20] = extent[0];
        constants[21] = extent[1];
    } else if (Graphics::XorSwizzleMode(descriptor.tileMode) != 0) {
        const auto* equation = Graphics::FindTextureSwizzleEquation(Graphics::XorSwizzleMode(descriptor.tileMode), elementBytes);
        require(equation != nullptr, "Metal texture has no XOR swizzle equation");
        constants[2] = 2;
        std::copy(equation->bits.begin(), equation->bits.end(), constants.begin() + 4);
    }
    auto commands = backend.CommandBuffer();
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = 0; layer < geometry.layers; ++layer) {
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
            backend.Encode(commands, @"TextureDetile", @[source, destination], &p, sizeof(p), MTLSizeMake(mip.width, mip.height, 1), &constants);
        }
    }
    backend.Wait(commands);
}

void MetalTexture::Upload(std::span<const std::byte> guestBytes) {
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    id<MTLTexture> texture = backing->texture;
    require(guestBytes.size() == geometry.guestBytes, "Metal texture upload byte extent differs from guest surface");
    auto tiled = backend.Buffer(bufferSize(geometry.guestBytes));
    auto linear = backend.Buffer(bufferSize(geometry.sliceLinearBytes * geometry.layers));
    std::memset(tiled.contents, 0, tiled.length);
    std::memset(linear.contents, 0, linear.length);
    std::memcpy(tiled.contents, guestBytes.data(), guestBytes.size());
    transfer(tiled, linear, false);
    const auto* bytes = static_cast<const std::byte*>(linear.contents);
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = 0; layer < geometry.layers; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto region = MTLRegionMake3D(0, 0, geometry.CopyDepth(layer), std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1);
            const auto* source = bytes + geometry.LinearLayerOffset(layer) + mip.linearOffset;
            [texture replaceRegion:region mipmapLevel:level slice:geometry.CopyLayer(layer) withBytes:source bytesPerRow:mip.pitchBytes bytesPerImage:mip.linearSize];
        }
    }
}

void MetalTexture::Readback(std::span<std::byte> guestBytes) const {
    const auto& descriptor = backing->descriptor;
    const auto& geometry = backing->geometry;
    id<MTLTexture> texture = backing->texture;
    require(guestBytes.size() == geometry.guestBytes, "Metal texture readback byte extent differs from guest surface");
    auto synchronize = backend.CommandBuffer();
    backend.Wait(synchronize);
    auto tiled = backend.Buffer(bufferSize(geometry.guestBytes));
    auto linear = backend.Buffer(bufferSize(geometry.sliceLinearBytes * geometry.layers));
    std::memset(tiled.contents, 0, tiled.length);
    std::memcpy(tiled.contents, guestBytes.data(), guestBytes.size());
    std::memset(linear.contents, 0, linear.length);
    auto* bytes = static_cast<std::byte*>(linear.contents);
    for (std::uint32_t level = 0; level < geometry.mips.size(); ++level) {
        const auto& mip = geometry.mips[level];
        for (std::uint32_t layer = 0; layer < geometry.layers; ++layer) {
            if (!geometry.HasLayer(level, layer)) continue;
            const auto region = MTLRegionMake3D(0, 0, geometry.CopyDepth(layer), std::max(descriptor.width >> level, 1u), std::max(descriptor.height >> level, 1u), 1);
            auto* destination = bytes + geometry.LinearLayerOffset(layer) + mip.linearOffset;
            [texture getBytes:destination bytesPerRow:mip.pitchBytes bytesPerImage:mip.linearSize fromRegion:region mipmapLevel:level slice:geometry.CopyLayer(layer)];
        }
    }
    transfer(linear, tiled, true);
    std::memcpy(guestBytes.data(), tiled.contents, guestBytes.size());
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
