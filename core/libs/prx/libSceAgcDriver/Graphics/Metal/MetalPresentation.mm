#include "MetalPresentation.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace AgcDriver::Metal {
namespace {

std::vector<std::byte> displayPixels(const DisplayBuffer& buffer) {
    const auto size = DisplayBufferSize(buffer);
    GuestMemory::ReadSiteScope scanout(GuestMemory::ReadSite::Scanout);
    if (buffer.dccAddress == 0) return DisplayPqHdr(buffer.pixelFormat) ? ReadDisplayBufferPqHdr(buffer) : ReadDisplayBuffer(buffer);
    const auto count = Graphics::DccKeyBytes(size);
    if (count == 0) throw std::runtime_error("Metal scanout DCC metadata has no key bytes");
    std::vector<std::byte> bytes(count);
    GuestMemory::Read(buffer.dccAddress, bytes);
    const auto keys = Graphics::ClassifyDccKeys(bytes);
    if (keys == Graphics::DccKeys::Uncompressed) return ReadDisplayBuffer(buffer);
    if (!Graphics::IsDccClear(keys)) {
        throw std::runtime_error(std::string("Metal scanout cannot present ") + Graphics::DccKeysName(keys) + " DCC keys");
    }
    const auto pixel = DisplayBufferClearPixel(buffer, keys);
    std::vector<std::byte> result(static_cast<std::size_t>(buffer.width) * buffer.height * pixel.size());
    for (std::size_t offset = 0; offset < result.size(); offset += pixel.size()) {
        std::memcpy(result.data() + offset, pixel.data(), pixel.size());
    }
    return result;
}

}

MetalPresentation::MetalPresentation(const MetalDevice& backend, id<MTLLibrary> library)
    : backend(backend), presenter(backend.Device(), library) {}

void MetalPresentation::Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
                                void (*gpuReady)(void*), void* completionContext) {
    if (window.context == nullptr || window.getDrawableSize == nullptr || window.metalLayer == nullptr) {
        throw std::invalid_argument("Metal presentation requires a window, drawable size query, and Metal layer callback");
    }
    if (gpuReady == nullptr || completionContext == nullptr) throw std::invalid_argument("Metal presentation requires a GPU completion callback and context");
    if (window.width == 0 || window.height == 0) throw std::invalid_argument("Metal presentation source extent is empty");
    if (buffer != nullptr && (buffer->width != window.width || buffer->height != window.height)) {
        throw std::invalid_argument("Metal display buffer extent differs from the output");
    }
    if (buffer != nullptr) static_cast<void>(DisplayBufferSize(*buffer));
    std::unique_lock lock(mutex);
    auto* layer = (__bridge CAMetalLayer*)window.metalLayer(window.context);
    if (layer == nil || ![layer isKindOfClass:CAMetalLayer.class]) throw std::invalid_argument("Metal presentation window callback did not return a CAMetalLayer");
    if (const auto previous = layers.find(window.context); previous != layers.end() && previous->second != layer) {
        backend.Wait(backend.CommandBuffer());
    }
    layers[window.context] = layer;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    window.getDrawableSize(window.context, &width, &height);
    layer.device = backend.Device();
    // Bufferless black/blank clears retain the active presentation transfer state.
    // An actual SDR buffer resets it; a first clear keeps the layer's SDR default.
    if (buffer != nullptr) {
        const bool pqHdr = DisplayPqHdr(buffer->pixelFormat);
        CGColorSpaceRef pqColorSpace = pqHdr ? CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ) : nullptr;
        if (pqHdr && pqColorSpace == nullptr) throw std::runtime_error("Metal cannot create the qualified BT.2100 PQ color space");
        layer.pixelFormat = pqHdr ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
        layer.colorspace = pqColorSpace;
        if (pqColorSpace != nullptr) CGColorSpaceRelease(pqColorSpace);
        layer.wantsExtendedDynamicRangeContent = pqHdr;
        // No target mastering/content-light metadata is established. The matching PQ
        // color space preserves its transfer identity; do not invent tone-map metadata.
        layer.EDRMetadata = nil;
    }
    layer.framebufferOnly = YES;
    layer.opaque = buffer != nullptr || opaque;
    layer.drawableSize = CGSizeMake(width, height);
    auto commands = backend.CommandBuffer();
    if (width != 0 && height != 0) {
        if (buffer != nullptr) {
            const auto pixels = displayPixels(*buffer);
            auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layer.pixelFormat width:buffer->width height:buffer->height mipmapped:NO];
            descriptor.storageMode = MTLStorageModeShared;
            descriptor.usage = MTLTextureUsageShaderRead;
            auto source = [backend.Device() newTextureWithDescriptor:descriptor];
            if (source == nil) throw std::runtime_error("Metal cannot allocate the decoded scanout texture");
            [source replaceRegion:MTLRegionMake2D(0, 0, buffer->width, buffer->height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:buffer->width * 4u];
            static_cast<void>(presenter.Present(commands, source, layer));
        } else {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable != nil) {
                auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, opaque ? 1 : 0);
                auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) throw std::runtime_error("Metal cannot encode a display clear");
                [encoder endEncoding];
                [commands presentDrawable:drawable];
            }
        }
    }
    backend.Wait(commands);
    lock.unlock();
    gpuReady(completionContext);
}

void MetalPresentation::ReleaseWindow(void* context) {
    std::lock_guard lock(mutex);
    if (const auto found = layers.find(context); found != layers.end()) {
        backend.Wait(backend.CommandBuffer());
        layers.erase(found);
    }
}

}
