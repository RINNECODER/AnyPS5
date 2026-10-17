#include "MetalFXUpscaler.hpp"
#import <MetalFX/MetalFX.h>
#include <cstdlib>
#include <cstring>

namespace AgcDriver::Metal {

bool MetalFxTemporalEnabled() {
    const char* value = std::getenv("ANYPS5_METAL_FX_TEMPORAL");
    if (value == nullptr) return false;
    if (*value == '1' && *(value + 1) == '\0') return true;
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0 ||
        std::strcmp(value, "on") == 0 || std::strcmp(value, "ON") == 0 ||
        std::strcmp(value, "yes") == 0 || std::strcmp(value, "YES") == 0) return true;
    return false;
}

bool MetalFxTemporalSupported(id<MTLDevice> device) {
    if (device == nil) return false;
    if (@available(macOS 13.0, *)) {
        Class descClass = NSClassFromString(@"MTLFXTemporalScalerDescriptor");
        if (descClass != nil && [descClass respondsToSelector:@selector(supportsDevice:)]) {
            return [MTLFXTemporalScalerDescriptor supportsDevice:device];
        }
    }
    return false;
}

MetalFXUpscaler::MetalFXUpscaler(id<MTLDevice> device) : device(device) {}

MetalFXUpscaler::~MetalFXUpscaler() {
    scaler = nil;
    upscaledTexture = nil;
}

bool MetalFXUpscaler::IsSupported() const {
    return MetalFxTemporalSupported(device);
}

bool MetalFXUpscaler::IsEnabled() const {
    return MetalFxTemporalEnabled();
}

void MetalFXUpscaler::Reset() {
    needsReset = true;
}

id<MTLTexture> MetalFXUpscaler::Upscale(id<MTLCommandBuffer> commands, id<MTLTexture> source,
                                        std::uint32_t outputWidth, std::uint32_t outputHeight) {
    if (!IsEnabled() || !IsSupported() || commands == nil || source == nil || outputWidth == 0 || outputHeight == 0) {
        return nil;
    }
    const auto inputWidth = static_cast<std::uint32_t>(source.width);
    const auto inputHeight = static_cast<std::uint32_t>(source.height);
    if (currentInputWidth != inputWidth || currentInputHeight != inputHeight ||
        currentOutputWidth != outputWidth || currentOutputHeight != outputHeight ||
        currentFormat != source.pixelFormat || scaler == nil || upscaledTexture == nil) {
        @try {
            MTLFXTemporalScalerDescriptor* desc = [MTLFXTemporalScalerDescriptor new];
            desc.colorTextureFormat = source.pixelFormat;
            desc.depthTextureFormat = MTLPixelFormatInvalid;
            desc.motionTextureFormat = MTLPixelFormatInvalid;
            desc.outputTextureFormat = source.pixelFormat;
            desc.inputWidth = inputWidth;
            desc.inputHeight = inputHeight;
            desc.outputWidth = outputWidth;
            desc.outputHeight = outputHeight;
            desc.autoExposureEnabled = YES;

            id<MTLFXTemporalScaler> newScaler = [desc newTemporalScalerWithDevice:device];
            if (newScaler == nil) return nil;

            MTLTextureDescriptor* outDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:source.pixelFormat
                                                                                               width:outputWidth
                                                                                              height:outputHeight
                                                                                           mipmapped:NO];
            outDesc.storageMode = MTLStorageModePrivate;
            outDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
            id<MTLTexture> newTexture = [device newTextureWithDescriptor:outDesc];
            if (newTexture == nil) return nil;

            scaler = newScaler;
            upscaledTexture = newTexture;
            currentInputWidth = inputWidth;
            currentInputHeight = inputHeight;
            currentOutputWidth = outputWidth;
            currentOutputHeight = outputHeight;
            currentFormat = source.pixelFormat;
            needsReset = true;
        } @catch (NSException*) {
            scaler = nil;
            upscaledTexture = nil;
            return nil;
        }
    }
    @try {
        id<MTLFXTemporalScaler> activeScaler = (id<MTLFXTemporalScaler>)scaler;
        activeScaler.colorTexture = source;
        activeScaler.outputTexture = upscaledTexture;
        activeScaler.inputContentWidth = inputWidth;
        activeScaler.inputContentHeight = inputHeight;
        activeScaler.reset = needsReset;
        needsReset = false;
        [activeScaler encodeToCommandBuffer:commands];
        return upscaledTexture;
    } @catch (NSException*) {
        return nil;
    }
}

}
