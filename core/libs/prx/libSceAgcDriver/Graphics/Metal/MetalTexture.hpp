#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALTEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALTEXTURE_HPP

#include "MetalDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <cstddef>
#include <memory>
#include <span>

namespace AgcDriver::Metal {

class MetalTexture {
public:
    MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor, bool compare = false);
    MetalTexture(const MetalDevice& backend, std::span<const std::uint32_t> words, bool compare = false);
    [[nodiscard]] bool CanShareBacking(const Graphics::GuestTextureResource& descriptor) const;
    [[nodiscard]] std::shared_ptr<MetalTexture> CreateView(const Graphics::GuestTextureResource& descriptor, bool compare = false) const;
    void Upload(std::span<const std::byte> guestBytes);
    void Readback(std::span<std::byte> guestBytes) const;
    // The same, from or into the guest surface at `offset` in a GPU buffer (a snapshot mirror). The
    // GPU converts in place, without staging the surface through a CPU copy, when the extent allows.
    void Upload(id<MTLBuffer> guest, std::size_t offset);
    void Readback(id<MTLBuffer> guest, std::size_t offset) const;
    // Upload(buffer, offset) commits without waiting (the span overload waits): work committed
    // afterwards on the same queue runs after it. This waits for it and reports a GPU failure;
    // Readback calls it too.
    void FinishUpload() const;
    [[nodiscard]] id<MTLTexture> Texture() const;
    [[nodiscard]] id<MTLTexture> SampledView() const;
    [[nodiscard]] id<MTLTexture> StorageView(bool atomic = false) const;
    [[nodiscard]] bool SupportsStorage() const;
    [[nodiscard]] bool SupportsAtomic() const;
    [[nodiscard]] const Graphics::GuestTextureResource& Descriptor() const;
    [[nodiscard]] const Graphics::SurfaceGeometry& Geometry() const;
    [[nodiscard]] std::size_t GuestBytes() const;

private:
    friend class MetalShaderResources;
    MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor,
                 bool compare, bool minimumLodLowered);
    [[nodiscard]] std::shared_ptr<MetalTexture> CreateView(const Graphics::GuestTextureResource& descriptor,
                                                        bool compare, bool minimumLodLowered) const;
    struct Backing {
        Graphics::GuestTextureResource descriptor;
        Graphics::SurfaceGeometry geometry;
        std::uint32_t elementBytes;
        id<MTLTexture> texture;
    };
    MetalTexture(const MetalDevice& backend, std::shared_ptr<Backing> backing,
                 const Graphics::GuestTextureResource& descriptor, bool compare, bool minimumLodLowered);
    [[nodiscard]] id<MTLTexture> RawSintStorageView() const;
    void createViews(bool compare);
    // Encodes the guest-tiled <-> linear conversion into `commands` (retile: linear to tiled).
    void encodeTransfer(id<MTLCommandBuffer> commands, id<MTLBuffer> source, NSUInteger sourceOffset,
                        id<MTLBuffer> destination, NSUInteger destinationOffset, bool retile) const;
    // Whether the tiled surface can be read or written in place at `offset` in `buffer`.
    [[nodiscard]] bool inPlace(id<MTLBuffer> buffer, std::size_t offset) const;
    // Encodes a GPU blit between the linear staging buffer and every present level and layer.
    void encodeLinearCopy(id<MTLCommandBuffer> commands, id<MTLBuffer> linear, bool toTexture) const;
    const MetalDevice& backend;
    Graphics::GuestTextureResource descriptor;
    std::shared_ptr<Backing> backing;
    id<MTLTexture> typedTexture;
    id<MTLTexture> sampledView;
    mutable id<MTLCommandBuffer> pendingUpload = nil;
    id<MTLTexture> storageView;
    id<MTLTexture> atomicView;
};

}

#endif
