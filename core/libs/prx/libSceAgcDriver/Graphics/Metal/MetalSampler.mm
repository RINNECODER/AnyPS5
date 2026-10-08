#include "MetalSampler.hpp"
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <set>
#include <cstring>
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#import <objc/runtime.h>

namespace AgcDriver::Metal {
namespace {

MTLSamplerMinMagFilter Filter(VkFilter filter) {
    switch (filter) {
        case VK_FILTER_NEAREST: return MTLSamplerMinMagFilterNearest;
        case VK_FILTER_LINEAR: return MTLSamplerMinMagFilterLinear;
        default: throw std::invalid_argument("Metal sampler filter is unsupported");
    }
}

MTLSamplerMipFilter MipFilter(VkSamplerMipmapMode filter) {
    switch (filter) {
        case VK_SAMPLER_MIPMAP_MODE_NEAREST: return MTLSamplerMipFilterNearest;
        case VK_SAMPLER_MIPMAP_MODE_LINEAR: return MTLSamplerMipFilterLinear;
        default: throw std::invalid_argument("Metal sampler mip filter is unsupported");
    }
}

MTLSamplerAddressMode AddressMode(VkSamplerAddressMode mode) {
    switch (mode) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT: return MTLSamplerAddressModeRepeat;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
        default: throw std::invalid_argument("Metal sampler address mode is unsupported");
    }
}

MTLSamplerBorderColor BorderColor(VkBorderColor color) {
    switch (color) {
        case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:
        case VK_BORDER_COLOR_INT_TRANSPARENT_BLACK: return MTLSamplerBorderColorTransparentBlack;
        case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
        case VK_BORDER_COLOR_INT_OPAQUE_BLACK: return MTLSamplerBorderColorOpaqueBlack;
        case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
        case VK_BORDER_COLOR_INT_OPAQUE_WHITE: return MTLSamplerBorderColorOpaqueWhite;
        default: throw std::invalid_argument("Metal sampler border color is unsupported");
    }
}

MTLCompareFunction Compare(VkCompareOp operation) {
    switch (operation) {
        case VK_COMPARE_OP_NEVER: return MTLCompareFunctionNever;
        case VK_COMPARE_OP_LESS: return MTLCompareFunctionLess;
        case VK_COMPARE_OP_EQUAL: return MTLCompareFunctionEqual;
        case VK_COMPARE_OP_LESS_OR_EQUAL: return MTLCompareFunctionLessEqual;
        case VK_COMPARE_OP_GREATER: return MTLCompareFunctionGreater;
        case VK_COMPARE_OP_NOT_EQUAL: return MTLCompareFunctionNotEqual;
        case VK_COMPARE_OP_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
        case VK_COMPARE_OP_ALWAYS: return MTLCompareFunctionAlways;
        default: throw std::invalid_argument("Metal sampler compare operation is unsupported");
    }
}

char capturedSamplerAssociation;

Graphics::GuestSamplerResource Decode(std::span<const std::uint32_t> words, bool compareEnable, bool qualified) {
    auto resource = Graphics::DecodeSamplerResource(words, qualified);
    resource.compareEnable = compareEnable;
    return resource;
}

}

MetalSampler::MetalSampler(id<MTLDevice> device, std::span<const std::uint32_t> words, bool compareEnable,
    bool unnormalizedQualified)
    : MetalSampler(device, Decode(words, compareEnable, unnormalizedQualified), true) {
    std::array<std::uint32_t, 6> identity{};
    std::copy(words.begin(), words.end(), identity.begin());
    identity[4] = compareEnable;
    identity[5] = unnormalizedQualified;
    auto captured = [NSData dataWithBytes:identity.data() length:sizeof(identity)];
    objc_setAssociatedObject(sampler, &capturedSamplerAssociation, captured, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

MetalSampler::MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource)
    : MetalSampler(device, resource, false) {}

MetalSampler::MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource, bool captured) {
    if (device == nil) throw std::invalid_argument("Metal sampler requires a device");
    if (resource.unnormalizedCoordinates && (!captured || resource.compareEnable ||
        resource.minFilter != resource.magFilter || resource.anisotropyEnable || resource.maxAnisotropy != 1.0f ||
        resource.minLod != 0.0f || resource.maxLod != 0.0f || resource.lodBias != 0.0f ||
        resource.mipmapMode != VK_SAMPLER_MIPMAP_MODE_NEAREST)) {
        throw std::invalid_argument("Metal pixel sampler requires a captured qualified descriptor and the native pixel filter/LOD contract");
    }
    if (!std::isfinite(resource.minLod) || !std::isfinite(resource.maxLod) || resource.minLod < 0.0f || resource.maxLod < resource.minLod) {
        throw std::invalid_argument("Metal sampler LOD range is invalid");
    }
    if (!std::isfinite(resource.maxAnisotropy) || resource.maxAnisotropy < 1.0f || resource.maxAnisotropy > 16.0f ||
        (resource.anisotropyEnable && std::floor(resource.maxAnisotropy) != resource.maxAnisotropy)) {
        throw std::invalid_argument("Metal sampler anisotropy exceeds its supported integer range");
    }
    if (!std::isfinite(resource.lodBias) || resource.lodBias < -16.0f || resource.lodBias >= 16.0f ||
        std::round(resource.lodBias * 64.0f) != resource.lodBias * 64.0f) {
        throw std::invalid_argument("Metal sampler LOD bias is not representable in S4.6");
    }
    auto descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.minFilter = Filter(resource.minFilter);
    descriptor.magFilter = Filter(resource.magFilter);
    descriptor.mipFilter = resource.unnormalizedCoordinates ? MTLSamplerMipFilterNotMipmapped : MipFilter(resource.mipmapMode);
    const auto address = [&](VkSamplerAddressMode mode) {
        if (!resource.unnormalizedCoordinates) return AddressMode(mode);
        if (mode == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) return MTLSamplerAddressModeClampToEdge;
        if (mode == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER &&
            (resource.borderColor == VK_BORDER_COLOR_INT_TRANSPARENT_BLACK ||
             resource.borderColor == VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK)) return MTLSamplerAddressModeClampToZero;
        throw std::invalid_argument("Metal pixel sampler requires edge or transparent-zero addressing");
    };
    descriptor.sAddressMode = address(resource.addressModeU);
    descriptor.tAddressMode = address(resource.addressModeV);
    descriptor.rAddressMode = resource.unnormalizedCoordinates ? MTLSamplerAddressModeClampToEdge : address(resource.addressModeW);
    descriptor.borderColor = BorderColor(resource.borderColor);
    const auto compare = Compare(resource.compareOp);
    descriptor.compareFunction = resource.compareEnable ? compare : MTLCompareFunctionNever;
    descriptor.maxAnisotropy = resource.anisotropyEnable ? static_cast<NSUInteger>(resource.maxAnisotropy) : 1u;
    descriptor.normalizedCoordinates = resource.unnormalizedCoordinates ? NO : YES;
    descriptor.supportArgumentBuffers = YES;
    descriptor.lodMinClamp = resource.minLod;
    descriptor.lodMaxClamp = resource.maxLod;
    if (resource.lodBias != 0.0f) {
        if (@available(macOS 26.0, *)) {
            if (![device supportsFamily:MTLGPUFamilyApple10]) {
                throw std::invalid_argument("Metal sampler LOD bias requires macOS 26 and Apple10 GPU family");
            }
            descriptor.lodBias = resource.lodBias;
        } else {
            throw std::invalid_argument("Metal sampler LOD bias requires macOS 26 and Apple10 GPU family");
        }
    }
    sampler = [device newSamplerStateWithDescriptor:descriptor];
    if (sampler == nil) throw std::runtime_error("Metal sampler creation failed");
}

id<MTLSamplerState> MetalSampler::Handle() const {
    return sampler;
}

bool MetalSampler::MatchesCapturedDescriptor(id<MTLSamplerState> sampler, std::span<const std::uint32_t> words,
    bool compareEnable, bool unnormalizedQualified) {
    if (sampler == nil || words.size() != 4) return false;
    NSData* captured = objc_getAssociatedObject(sampler, &capturedSamplerAssociation);
    if (![captured isKindOfClass:[NSData class]] || captured.length != 6 * sizeof(std::uint32_t)) return false;
    std::array<std::uint32_t, 6> identity{};
    std::copy(words.begin(), words.end(), identity.begin());
    identity[4] = compareEnable;
    identity[5] = unnormalizedQualified;
    return std::memcmp(captured.bytes, identity.data(), sizeof(identity)) == 0;
}

void ValidatePixelSamplerBindings(const std::vector<ShaderRecompiler::DescriptorBinding>& bindings) {
    using namespace ShaderRecompiler;
    const auto reject = [] { throw std::invalid_argument("Metal pixel sampler live descriptor proof is missing, malformed, or mismatched"); };
    const auto findSource = [&](DescriptorRole role, std::uint32_t source) {
        std::pair<const DescriptorBinding*, std::uint32_t> result{nullptr, 0};
        for (const auto& binding : bindings) {
            if (binding.role != role) continue;
            for (std::uint32_t i = 0; i < binding.resourceSources.size(); ++i) {
                if (binding.resourceSources[i] != source) continue;
                if (result.first != nullptr || i >= binding.count) reject();
                result = {&binding, i};
            }
        }
        if (result.first == nullptr) reject();
        return result;
    };
    for (const auto& binding : bindings) {
        const bool sampler = binding.role == DescriptorRole::GuestSamplers;
        if (!sampler && binding.role != DescriptorRole::GuestImages) continue;
        const auto& flags = sampler ? binding.samplerUnnormalized : binding.imageUnnormalized;
        const auto& proofs = sampler ? binding.samplerPixelProof : binding.imagePixelProof;
        const auto words = sampler ? 4u : 8u;
        if ((!flags.empty() && flags.size() != binding.count) ||
            (!proofs.empty() && proofs.size() != binding.count) ||
            (!binding.resourceSources.empty() && binding.resourceSources.size() != binding.count)) reject();
        for (std::uint32_t i = 0; i < binding.count; ++i) {
            const bool pixel = !flags.empty() && flags[i];
            const bool hasProof = !proofs.empty() && !proofs[i].empty();
            if (pixel != hasProof || (sampler && binding.guestDescriptor.size() == static_cast<std::size_t>(binding.count) * 4u &&
                pixel != ((binding.guestDescriptor[i * 4u] & (1u << 15u)) != 0))) reject();
            if (!pixel) continue;
            if (binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * words ||
                binding.resourceSources.size() != binding.count ||
                binding.kind != (sampler ? DescriptorKind::Sampler : DescriptorKind::SampledImage) ||
                (sampler && i < binding.samplerDepthCompare.size() && binding.samplerDepthCompare[i]) ||
                (!sampler && ((i < binding.imageWritten.size() && binding.imageWritten[i]) ||
                    (i < binding.imageAtomic.size() && binding.imageAtomic[i]) ||
                    (i < binding.imageDepthCompare.size() && binding.imageDepthCompare[i])))) reject();
            std::uint64_t useCount = 0;
            std::set<std::uint32_t> peers;
            for (const auto& proof : proofs[i]) {
                if (proof.liveUseMask != 1u || proof.liveUseCount == 0 || proof.samplerLiveUseCount == 0 ||
                    proof.liveUseCount > proof.samplerLiveUseCount ||
                    !peers.insert(sampler ? proof.imageSource : proof.samplerSource).second ||
                    binding.resourceSources[i] != (sampler ? proof.samplerSource : proof.imageSource)) reject();
                const auto [image, imageIndex] = findSource(DescriptorRole::GuestImages, proof.imageSource);
                const auto [samplerBinding, samplerIndex] = findSource(DescriptorRole::GuestSamplers, proof.samplerSource);
                if (image->imageUnnormalized.size() != image->count || !image->imageUnnormalized[imageIndex] ||
                    image->imagePixelProof.size() != image->count ||
                    samplerBinding->samplerUnnormalized.size() != samplerBinding->count || !samplerBinding->samplerUnnormalized[samplerIndex] ||
                    samplerBinding->samplerPixelProof.size() != samplerBinding->count ||
                    image->guestDescriptor.size() != static_cast<std::size_t>(image->count) * 8u ||
                    samplerBinding->guestDescriptor.size() != static_cast<std::size_t>(samplerBinding->count) * 4u ||
                    !std::equal(proof.imageDescriptor.begin(), proof.imageDescriptor.end(), image->guestDescriptor.begin() + imageIndex * 8u) ||
                    !std::equal(proof.samplerDescriptor.begin(), proof.samplerDescriptor.end(), samplerBinding->guestDescriptor.begin() + samplerIndex * 4u) ||
                    std::count(image->imagePixelProof[imageIndex].begin(), image->imagePixelProof[imageIndex].end(), proof) != 1 ||
                    std::count(samplerBinding->samplerPixelProof[samplerIndex].begin(), samplerBinding->samplerPixelProof[samplerIndex].end(), proof) != 1) reject();
                if (sampler) useCount += proof.liveUseCount;
            }
            if (sampler && std::any_of(proofs[i].begin(), proofs[i].end(), [&](const auto& proof) {
                return useCount != proof.samplerLiveUseCount;
            })) reject();
        }
    }
}

void ValidatePixelSampledView(id<MTLTexture> view, const Graphics::GuestTextureResource& descriptor) {
    if ((descriptor.dimension != Graphics::TextureDimension::k1D && descriptor.dimension != Graphics::TextureDimension::k2D) ||
        descriptor.baseLevel != 0 || descriptor.lastLevel != 0 || descriptor.mipCount != 1 ||
        (descriptor.allocatedMipCount != 0 && descriptor.allocatedMipCount != 1) ||
        descriptor.baseArray != 0 || descriptor.depthOrLastArray != 0 || Graphics::EffectiveMinLod(descriptor) != 0) {
        throw std::invalid_argument("Metal pixel sampled descriptor requires a direct single-layer mip-zero view with zero minimum LOD");
    }
    // Logical 1D already uses a texture2D with height one in the native backend.
    if (view == nil || view.textureType != MTLTextureType2D || view.mipmapLevelCount != 1 ||
        view.arrayLength != 1 || view.depth != 1 || view.sampleCount != 1 ||
        view.width != descriptor.width || view.height != descriptor.height ||
        (descriptor.dimension == Graphics::TextureDimension::k1D && view.height != 1)) {
        throw std::invalid_argument("Metal pixel sampler actual selected texture view is not direct single-layer single-level 1D/2D");
    }
    for (id<MTLTexture> selected = view; selected.parentTexture != nil; selected = selected.parentTexture) {
        if (selected.parentRelativeLevel != 0 || selected.parentRelativeSlice != 0) {
            throw std::invalid_argument("Metal pixel sampler actual selected texture view has a nonzero base level or layer");
        }
    }
    MTLPixelFormat format;
    switch (Graphics::ResolveTextureFormat(descriptor.format)) {
        case VK_FORMAT_R8_UNORM: format = MTLPixelFormatR8Unorm; break;
        case VK_FORMAT_R8_SRGB: format = MTLPixelFormatR8Unorm_sRGB; break;
        case VK_FORMAT_R16_UNORM: format = MTLPixelFormatR16Unorm; break;
        case VK_FORMAT_R16_SNORM: format = MTLPixelFormatR16Snorm; break;
        case VK_FORMAT_R16_SFLOAT: format = MTLPixelFormatR16Float; break;
        case VK_FORMAT_R8G8_UNORM: format = MTLPixelFormatRG8Unorm; break;
        case VK_FORMAT_R8G8_SRGB: format = MTLPixelFormatRG8Unorm_sRGB; break;
        case VK_FORMAT_R8G8_SNORM: format = MTLPixelFormatRG8Snorm; break;
        case VK_FORMAT_R32_SFLOAT: format = MTLPixelFormatR32Float; break;
        case VK_FORMAT_R16G16_UNORM: format = MTLPixelFormatRG16Unorm; break;
        case VK_FORMAT_R16G16_SNORM: format = MTLPixelFormatRG16Snorm; break;
        case VK_FORMAT_R16G16_SFLOAT: format = MTLPixelFormatRG16Float; break;
        case VK_FORMAT_R8G8B8A8_UNORM: format = MTLPixelFormatRGBA8Unorm; break;
        case VK_FORMAT_R8G8B8A8_SNORM: format = MTLPixelFormatRGBA8Snorm; break;
        case VK_FORMAT_R8G8B8A8_SRGB: format = MTLPixelFormatRGBA8Unorm_sRGB; break;
        case VK_FORMAT_R32G32_SFLOAT: format = MTLPixelFormatRG32Float; break;
        case VK_FORMAT_R16G16B16A16_UNORM: format = MTLPixelFormatRGBA16Unorm; break;
        case VK_FORMAT_R16G16B16A16_SNORM: format = MTLPixelFormatRGBA16Snorm; break;
        case VK_FORMAT_R16G16B16A16_SFLOAT: format = MTLPixelFormatRGBA16Float; break;
        case VK_FORMAT_R32G32B32A32_SFLOAT: format = MTLPixelFormatRGBA32Float; break;
        default: throw std::invalid_argument("Metal pixel sampler selected format requires unsupported integer, packed, depth, or conversion sampling");
    }
    if (view.pixelFormat != format) {
        throw std::invalid_argument("Metal pixel sampler actual selected texture format differs from the captured descriptor");
    }
    const auto swizzle = [](std::uint8_t selector) {
        switch (selector) {
            case 0: return MTLTextureSwizzleZero;
            case 1: return MTLTextureSwizzleOne;
            case 4: return MTLTextureSwizzleRed;
            case 5: return MTLTextureSwizzleGreen;
            case 6: return MTLTextureSwizzleBlue;
            case 7: return MTLTextureSwizzleAlpha;
            default: throw std::invalid_argument("Metal pixel sampled descriptor has an invalid component selector");
        }
    };
    const auto selected = view.swizzle;
    if (selected.red != swizzle(descriptor.dstSelX) || selected.green != swizzle(descriptor.dstSelY) ||
        selected.blue != swizzle(descriptor.dstSelZ) || selected.alpha != swizzle(descriptor.dstSelW)) {
        throw std::invalid_argument("Metal pixel sampler actual selected texture swizzle differs from the captured descriptor");
    }
}

}
