#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"

namespace AgcDriver::Metal {

class MetalSampler {
public:
    MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource);
    // `forceDegammaPaired`: the caller has checked that every image this sampler is paired with has an
    // sRGB format, whose view decodes, so a FORCE_DEGAMMA descriptor is accepted (as in the Vulkan driver).
    MetalSampler(id<MTLDevice> device, std::span<const std::uint32_t> words, bool compareEnable = false,
                 bool unnormalizedQualified = false, bool forceDegammaPaired = false);
    id<MTLSamplerState> Handle() const;
    [[nodiscard]] static bool MatchesCapturedDescriptor(id<MTLSamplerState> sampler,
        std::span<const std::uint32_t> words, bool compareEnable, bool unnormalizedQualified);

private:
    // `identity` is the captured guest descriptor identity (nil for an uncaptured sampler).
    MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource, NSData* identity);
    id<MTLSamplerState> sampler;
};

// Validate both directions of the live pair certificate before constructing or accepting native resources.
void ValidatePixelSamplerBindings(const std::vector<ShaderRecompiler::DescriptorBinding>& bindings);
void ValidatePixelSampledView(id<MTLTexture> view, const Graphics::GuestTextureResource& descriptor);

// A null T# (no base address, or type 0): the element of a runtime ABI image heap that the snapshot
// leaves unbound, which the shared driver also leaves unbound.
[[nodiscard]] inline bool NullImageDescriptor(std::span<const std::uint32_t> words) {
    return words.size() == 8 && Graphics::IsNullTextureResource(words);
}

}
