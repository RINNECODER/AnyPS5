#include "MetalTexture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MTLPixelFormat pixelFormat(std::uint32_t format) {
    switch (Graphics::ResolveTextureFormat(format)) {
        case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
        case VK_FORMAT_R8_UINT: return MTLPixelFormatR8Uint;
        case VK_FORMAT_R16_UNORM: return MTLPixelFormatR16Unorm;
        case VK_FORMAT_R16_SNORM: return MTLPixelFormatR16Snorm;
        case VK_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
        case VK_FORMAT_R16_SINT: return MTLPixelFormatR16Sint;
        case VK_FORMAT_R16_SFLOAT: return MTLPixelFormatR16Float;
        case VK_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
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
        case Graphics::TextureDimension::k1D: return MTLTextureType1D;
        case Graphics::TextureDimension::k2D: return MTLTextureType2D;
        case Graphics::TextureDimension::k2DArray:
        case Graphics::TextureDimension::kCube: return MTLTextureType2DArray;
        case Graphics::TextureDimension::k3D: return MTLTextureType3D;
    }
    throw std::runtime_error("Metal texture has an unknown guest dimension");
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

MetalTexture::MetalTexture(const MetalDevice& backend, std::span<const std::uint32_t> words)
    : MetalTexture(backend, Graphics::DecodeTextureResource(words)) {}

MetalTexture::MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor)
    : backend(backend), descriptor(descriptor), elementBytes(Graphics::BytesPerElement(descriptor.format)) {
    require(!Graphics::IsBlockCompressed(descriptor.format), "Metal texture BC compression is not supported");
    require(descriptor.dccAddress == 0, "Metal texture DCC metadata is not supported");
    require(Graphics::XorSwizzleMode(descriptor.tileMode) == 0, "Metal texture XOR tile modes are not supported");
    require(Graphics::EffectiveMinLod(descriptor) == 0, "Metal texture minimum LOD view clamp is not supported");
    require(std::has_single_bit(elementBytes) && elementBytes <= 16, "Metal texture element format is not supported");
    require(descriptor.width != 0 && descriptor.height != 0 && descriptor.mipCount != 0 && descriptor.mipCount <= 16, "Metal texture has invalid dimensions or mip count");
    require(descriptor.baseLevel <= descriptor.lastLevel && descriptor.lastLevel < descriptor.mipCount, "Metal texture view mip range lies outside the surface");
    require(descriptor.dimension != Graphics::TextureDimension::k1D || descriptor.height == 1, "Metal 1D texture has a non-unit height");
    require(descriptor.dimension != Graphics::TextureDimension::k1D || descriptor.mipCount == 1, "Metal 1D texture mip chains require shader dimension lowering and are not supported");
    geometry = Graphics::DescribeSurface(descriptor);
    bufferSize(geometry.guestBytes);
    bufferSize(geometry.sliceLinearBytes * geometry.layers);
    require(descriptor.baseArray < geometry.imageLayers, "Metal texture view base array lies outside the surface");
    auto native = [MTLTextureDescriptor new];
    native.textureType = textureType(descriptor.dimension);
    native.pixelFormat = pixelFormat(descriptor.format);
    native.width = descriptor.width;
    native.height = descriptor.height;
    native.depth = geometry.imageDepth;
    native.arrayLength = geometry.imageLayers;
    native.mipmapLevelCount = descriptor.mipCount;
    native.storageMode = MTLStorageModeShared;
    native.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    texture = [backend.Device() newTextureWithDescriptor:native];
    require(texture != nil, "Metal cannot allocate the guest texture format or dimensions");
    const auto levels = NSMakeRange(descriptor.baseLevel, descriptor.lastLevel - descriptor.baseLevel + 1u);
    const auto slices = NSMakeRange(descriptor.baseArray, geometry.imageLayers - descriptor.baseArray);
    const auto components = MTLTextureSwizzleChannelsMake(swizzle(descriptor.dstSelX), swizzle(descriptor.dstSelY), swizzle(descriptor.dstSelZ), swizzle(descriptor.dstSelW));
    sampledView = [texture newTextureViewWithPixelFormat:native.pixelFormat textureType:native.textureType levels:levels slices:slices swizzle:components];
    storageView = [texture newTextureViewWithPixelFormat:native.pixelFormat textureType:native.textureType levels:levels slices:slices];
    require(sampledView != nil && storageView != nil, "Metal cannot create the guest texture mip, array, or swizzle view");
}

void MetalTexture::transfer(id<MTLBuffer> source, id<MTLBuffer> destination, bool retile) const {
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
            const auto region = MTLRegionMake3D(0, 0, geometry.CopyDepth(layer), mip.width, mip.height, 1);
            const auto* source = bytes + geometry.LinearLayerOffset(layer) + mip.linearOffset;
            [texture replaceRegion:region mipmapLevel:level slice:geometry.CopyLayer(layer) withBytes:source bytesPerRow:mip.pitchBytes bytesPerImage:mip.linearSize];
        }
    }
}

void MetalTexture::Readback(std::span<std::byte> guestBytes) const {
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
            const auto region = MTLRegionMake3D(0, 0, geometry.CopyDepth(layer), mip.width, mip.height, 1);
            auto* destination = bytes + geometry.LinearLayerOffset(layer) + mip.linearOffset;
            [texture getBytes:destination bytesPerRow:mip.pitchBytes bytesPerImage:mip.linearSize fromRegion:region mipmapLevel:level slice:geometry.CopyLayer(layer)];
        }
    }
    transfer(linear, tiled, true);
    std::memcpy(guestBytes.data(), tiled.contents, guestBytes.size());
}

id<MTLTexture> MetalTexture::Texture() const { return texture; }
id<MTLTexture> MetalTexture::SampledView() const { return sampledView; }
id<MTLTexture> MetalTexture::StorageView() const { return storageView; }
const Graphics::GuestTextureResource& MetalTexture::Descriptor() const { return descriptor; }
const Graphics::SurfaceGeometry& MetalTexture::Geometry() const { return geometry; }
std::size_t MetalTexture::GuestBytes() const { return static_cast<std::size_t>(geometry.guestBytes); }

}
