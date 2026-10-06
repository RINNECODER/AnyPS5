#include "MetalSampler.hpp"
#include <cmath>
#include <stdexcept>

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

Graphics::GuestSamplerResource Decode(std::span<const std::uint32_t> words, bool compareEnable) {
    auto resource = Graphics::DecodeSamplerResource(words);
    resource.compareEnable = compareEnable;
    return resource;
}

}

MetalSampler::MetalSampler(id<MTLDevice> device, std::span<const std::uint32_t> words, bool compareEnable)
    : MetalSampler(device, Decode(words, compareEnable)) {}

MetalSampler::MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource) {
    if (device == nil) throw std::invalid_argument("Metal sampler requires a device");
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
    descriptor.mipFilter = MipFilter(resource.mipmapMode);
    descriptor.sAddressMode = AddressMode(resource.addressModeU);
    descriptor.tAddressMode = AddressMode(resource.addressModeV);
    descriptor.rAddressMode = AddressMode(resource.addressModeW);
    descriptor.borderColor = BorderColor(resource.borderColor);
    const auto compare = Compare(resource.compareOp);
    descriptor.compareFunction = resource.compareEnable ? compare : MTLCompareFunctionNever;
    descriptor.maxAnisotropy = resource.anisotropyEnable ? static_cast<NSUInteger>(resource.maxAnisotropy) : 1u;
    descriptor.normalizedCoordinates = YES;
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

}
