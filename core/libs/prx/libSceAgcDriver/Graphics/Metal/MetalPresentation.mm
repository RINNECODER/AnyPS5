#include "MetalPresentation.hpp"
#include "MetalFXUpscaler.hpp"
#include "FrameCapture.hpp"
#include "BindTrace.hpp"
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
    // Display keys cover the console's metadata extent of a 4-byte SW_64KB_R_X surface, as the
    // shared driver's DisplayBufferKeyBytes reads them.
    if (buffer.tilingMode != 0) throw std::runtime_error("Metal scanout: a linear display buffer has no DCC keys");
    const auto count = Graphics::DccKeyCount(Graphics::TextureTileMode::kR64KBX, 4, buffer.width, buffer.height, size);
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
    : backend(backend), presenter(backend.Device(), library),
      fxUpscaler(std::make_unique<MetalFXUpscaler>(backend.Device())) {}

MetalPresentation::~MetalPresentation() {
    scanoutTexture = nil;
}

std::vector<std::byte> MetalPresentation::ReadPixels(const DisplayBuffer& buffer) { return displayPixels(buffer); }

void MetalPresentation::Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
                                void (*gpuReady)(void*), void* completionContext, const std::vector<std::byte>* decoded) {
    if (window.context == nullptr || window.getDrawableSize == nullptr || window.metalLayer == nullptr) {
        throw std::invalid_argument("Metal presentation requires a window, drawable size query, and Metal layer callback");
    }
    if (gpuReady == nullptr || completionContext == nullptr) throw std::invalid_argument("Metal presentation requires a GPU completion callback and context");
    if (window.width == 0 || window.height == 0) throw std::invalid_argument("Metal presentation source extent is empty");
    if (buffer != nullptr && (buffer->width != window.width || buffer->height != window.height)) {
        throw std::invalid_argument("Metal display buffer extent differs from the output");
    }
    if (buffer != nullptr) static_cast<void>(DisplayBufferSize(*buffer));
    if (BindTrace::Active()) {
        BindTrace::Log("present buffer 0x%llx format 0x%llx %ux%u tiling %u dcc 0x%llx",
            static_cast<unsigned long long>(buffer ? buffer->address : 0), static_cast<unsigned long long>(buffer ? buffer->pixelFormat : 0),
            window.width, window.height, buffer ? buffer->tilingMode : 0u, static_cast<unsigned long long>(buffer ? buffer->dccAddress : 0));
    }
    BindTrace::Flip();
    std::unique_lock lock(mutex);
    auto* layer = (__bridge CAMetalLayer*)window.metalLayer(window.context);
    if (layer == nil || ![layer isKindOfClass:CAMetalLayer.class]) throw std::invalid_argument("Metal presentation window callback did not return a CAMetalLayer");
    if (const auto previous = layers.find(window.context); previous != layers.end() && previous->second != layer) {
        backend.Wait(backend.CommandBuffer());
        if (fxUpscaler) fxUpscaler->Reset();
        scanoutTexture = nil;
        scanoutWidth = 0;
        scanoutHeight = 0;
        scanoutFormat = MTLPixelFormatInvalid;
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
    auto& frameCapture = FrameCapture::Shared();
    // Readback of the window drawable needs a sampleable texture; only when capture is enabled.
    layer.framebufferOnly = frameCapture.Enabled() ? NO : YES;
    layer.opaque = buffer != nullptr || opaque;
    layer.drawableSize = CGSizeMake(width, height);
    auto commands = backend.CommandBuffer();
    auto ticket = frameCapture.Begin(window.width, window.height);
    bool captureEncoded = false;
    // A throw before the readback is encoded must not wedge the single in-flight slot.
    struct Slot { FrameCapture& capture; bool active; const bool& encoded; ~Slot() { if (active && !encoded) capture.Abandon(); } }
        slot{frameCapture, ticket.has_value(), captureEncoded};
    // Empty `decoded` pixels: the caller saw a minimized window and read nothing; the window
    // grew since, so this frame is skipped like a minimized one.
    const bool unread = buffer != nullptr && decoded != nullptr && decoded->empty();
    if (width != 0 && height != 0 && !unread) {
        if (buffer != nullptr) {
            const auto pixels = decoded != nullptr ? *decoded : displayPixels(*buffer);
            if (scanoutTexture == nil || scanoutWidth != buffer->width || scanoutHeight != buffer->height || scanoutFormat != layer.pixelFormat) {
                auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:layer.pixelFormat width:buffer->width height:buffer->height mipmapped:NO];
                descriptor.storageMode = MTLStorageModeShared;
                descriptor.usage = MTLTextureUsageShaderRead;
                scanoutTexture = [backend.Device() newTextureWithDescriptor:descriptor];
                if (scanoutTexture == nil) throw std::runtime_error("Metal cannot allocate the decoded scanout texture");
                scanoutWidth = buffer->width;
                scanoutHeight = buffer->height;
                scanoutFormat = layer.pixelFormat;
            }
            auto source = scanoutTexture;
            [source replaceRegion:MTLRegionMake2D(0, 0, buffer->width, buffer->height) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:buffer->width * 4u];
            if (layer.device != backend.Device()) throw std::invalid_argument("Metal presentation requires a layer on its device");
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable != nil) {
                id<MTLTexture> presentationSource = source;
                if (fxUpscaler && fxUpscaler->IsEnabled() && fxUpscaler->IsSupported()) {
                    id<MTLTexture> upscaled = fxUpscaler->Upscale(commands, source, width, height);
                    if (upscaled != nil) {
                        presentationSource = upscaled;
                    }
                }
                presenter.Encode(commands, presentationSource, drawable.texture);
                [commands presentDrawable:drawable];
                if (ticket) { frameCapture.Encode(commands, drawable.texture, *ticket); captureEncoded = true; }
            }
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
                if (ticket) { frameCapture.Encode(commands, drawable.texture, *ticket); captureEncoded = true; }
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
        if (fxUpscaler) fxUpscaler->Reset();
        scanoutTexture = nil;
        scanoutWidth = 0;
        scanoutHeight = 0;
        scanoutFormat = MTLPixelFormatInvalid;
    }
}

}
