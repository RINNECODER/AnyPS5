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

}
