#pragma once

#import <Metal/Metal.h>
#include <cstdint>
#include <memory>

namespace AgcDriver::Metal {

[[nodiscard]] bool MetalFxTemporalEnabled();
[[nodiscard]] bool MetalFxTemporalSupported(id<MTLDevice> device);
[[nodiscard]] bool MetalFxSpatialEnabled();
[[nodiscard]] bool MetalFxSpatialSupported(id<MTLDevice> device);

class MetalFXUpscaler {
public:
    explicit MetalFXUpscaler(id<MTLDevice> device);
    ~MetalFXUpscaler();
    MetalFXUpscaler(const MetalFXUpscaler&) = delete;
    MetalFXUpscaler& operator=(const MetalFXUpscaler&) = delete;

    [[nodiscard]] bool IsSupported() const;
    [[nodiscard]] bool IsEnabled() const;

    id<MTLTexture> Upscale(id<MTLCommandBuffer> commands, id<MTLTexture> source,
                           std::uint32_t outputWidth, std::uint32_t outputHeight);
    void Reset();

private:
    id<MTLDevice> device;
    id scaler = nil;
    id<MTLTexture> upscaledTexture = nil;
    std::uint32_t currentInputWidth = 0;
    std::uint32_t currentInputHeight = 0;
    std::uint32_t currentOutputWidth = 0;
    std::uint32_t currentOutputHeight = 0;
    MTLPixelFormat currentFormat = MTLPixelFormatInvalid;
    bool isSpatial = false;
    bool needsReset = true;
};

}
