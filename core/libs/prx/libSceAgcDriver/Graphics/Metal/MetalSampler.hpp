#pragma once

#import <Metal/Metal.h>
#include "prx/libSceAgcDriver/Graphics/include/GuestSamplerResource.hpp"

namespace AgcDriver::Metal {

class MetalSampler {
public:
    MetalSampler(id<MTLDevice> device, const Graphics::GuestSamplerResource& resource);
    MetalSampler(id<MTLDevice> device, std::span<const std::uint32_t> words, bool compareEnable = false);
    id<MTLSamplerState> Handle() const;

private:
    id<MTLSamplerState> sampler;
};

}
