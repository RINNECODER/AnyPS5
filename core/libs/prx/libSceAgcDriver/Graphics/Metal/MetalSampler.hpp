#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"

namespace AgcDriver::Metal {

class MetalSampler {
public:
    MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource);
    MetalSampler(id<MTLDevice> device, std::span<const std::uint32_t> words, bool compareEnable = false,
                 bool unnormalizedQualified = false);
    id<MTLSamplerState> Handle() const;
    [[nodiscard]] static bool MatchesCapturedDescriptor(id<MTLSamplerState> sampler,
        std::span<const std::uint32_t> words, bool compareEnable, bool unnormalizedQualified);

private:
    MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource, bool captured);
    id<MTLSamplerState> sampler;
};

// Validate both directions of the live pair certificate before constructing or accepting native resources.
void ValidatePixelSamplerBindings(const std::vector<ShaderRecompiler::DescriptorBinding>& bindings);
void ValidatePixelSampledView(id<MTLTexture> view, const Graphics::GuestTextureResource& descriptor);

// An empty T# (no base address): the element of a runtime ABI image heap that the snapshot leaves
// unbound, which the shared driver also leaves unbound.
[[nodiscard]] inline bool NullImageDescriptor(std::span<const std::uint32_t> words) {
    return words.size() == 8 && words[0] == 0u && (words[1] & 0xffu) == 0u;
}

}
