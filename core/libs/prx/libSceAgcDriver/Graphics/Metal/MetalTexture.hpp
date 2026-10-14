#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALTEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALTEXTURE_HPP

#include "MetalDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <span>

namespace AgcDriver::Metal {

// Levels [levelBegin, levelEnd) and layers [layerBegin, layerEnd) of a texture's backing.
struct TextureSubresources {
    std::uint32_t levelBegin = 0;
    std::uint32_t levelEnd = ~0u;
    std::uint32_t layerBegin = 0;
    std::uint32_t layerEnd = ~0u;
};

class MetalTexture {
public:
    MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor, bool compare = false);
    MetalTexture(const MetalDevice& backend, std::span<const std::uint32_t> words, bool compare = false);
    [[nodiscard]] bool CanShareBacking(const Graphics::GuestTextureResource& descriptor) const;
    // CanShareBacking, except that the two descriptors may name different DCC metadata. Only valid
    // when every DCC-bearing side's keys say "uncompressed" (plain texels): MetalShaderResources checks.
    [[nodiscard]] bool CanShareBackingIgnoringDcc(const Graphics::GuestTextureResource& descriptor) const;
    // Why CanShareBacking(descriptor) is false, for error messages.
    [[nodiscard]] std::string ShareMismatch(const Graphics::GuestTextureResource& descriptor) const;
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
    // Encodes Readback(guest, offset) into `commands`, after the work that writes the texture, when
    // the surface converts in place; returns false (encoding nothing) otherwise. The copy-back then
    // completes with that work instead of costing a second command buffer and CPU wait.
    [[nodiscard]] bool EncodeReadback(id<MTLCommandBuffer> commands, id<MTLBuffer> guest, std::size_t offset) const;
    // Levels [levelBegin, levelEnd) and layers [layerBegin, layerEnd) of the backing: the part of
    // the surface a work wrote. The rest of the guest bytes already equal the texture.
    using Subresources = TextureSubresources;
    [[nodiscard]] bool EncodeReadback(id<MTLCommandBuffer> commands, id<MTLBuffer> guest, std::size_t offset, Subresources part) const;
    [[nodiscard]] id<MTLTexture> Texture() const;
    [[nodiscard]] id<MTLTexture> SampledView() const;
    [[nodiscard]] id<MTLTexture> StorageView(bool atomic = false) const;
    [[nodiscard]] bool SupportsStorage() const;
    [[nodiscard]] bool SupportsAtomic() const;
    [[nodiscard]] const Graphics::GuestTextureResource& Descriptor() const;
    [[nodiscard]] const Graphics::SurfaceGeometry& Geometry() const;
    [[nodiscard]] std::size_t GuestBytes() const;

    // Whether Upload(buffer, offset) runs on the GPU in place (otherwise it copies on the CPU).
    [[nodiscard]] bool UploadsInPlace(id<MTLBuffer> buffer, std::size_t offset) const { return inPlace(buffer, offset); }

private:
    friend class MetalShaderResources;
    MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor,
                 bool compare, bool minimumLodLowered);
    [[nodiscard]] std::shared_ptr<MetalTexture> CreateView(const Graphics::GuestTextureResource& descriptor,
                                                        bool compare, bool minimumLodLowered, bool ignoreDcc = false) const;
    [[nodiscard]] bool canShareBacking(const Graphics::GuestTextureResource& descriptor, bool ignoreDcc) const;
    // The native views of one descriptor (createViews).
    struct Views {
        id<MTLTexture> typed;
        id<MTLTexture> sampled;
        id<MTLTexture> storage;
        id<MTLTexture> atomic;
    };
    using ViewKey = std::array<std::uint64_t, 21>;
    struct Backing {
        Graphics::GuestTextureResource descriptor;
        Graphics::SurfaceGeometry geometry;
        std::uint32_t elementBytes;
        id<MTLTexture> texture;
        // Views already made of this backing, by every descriptor field and the compare flag: a cached
        // surface bound through another descriptor got new native views for every draw.
        std::mutex viewsMutex;
        std::vector<std::pair<ViewKey, Views>> views;
    };
    MetalTexture(const MetalDevice& backend, std::shared_ptr<Backing> backing,
                 const Graphics::GuestTextureResource& descriptor, bool compare, bool minimumLodLowered);
    [[nodiscard]] id<MTLTexture> RawSintStorageView() const;
    void createViews(bool compare);
    void makeViews(bool compare);
    // Encodes the guest-tiled <-> linear conversion into `commands` (retile: linear to tiled).
    void encodeTransfer(id<MTLCommandBuffer> commands, id<MTLBuffer> source, NSUInteger sourceOffset,
                        id<MTLBuffer> destination, NSUInteger destinationOffset, bool retile, Subresources part = {}) const;
    // Whether the tiled surface can be read or written in place at `offset` in `buffer`.
    // The GPU paths of Upload/Readback, without the in-place check (the span overloads stage through a rounded buffer).
    // `recycleGuest`: `guest` is a MetalBufferPool staging buffer, returned once the upload ran.
    void uploadFrom(id<MTLBuffer> guest, std::size_t offset, bool recycleGuest = false);
    void readbackInto(id<MTLBuffer> guest, std::size_t offset) const;
    [[nodiscard]] bool inPlace(id<MTLBuffer> buffer, std::size_t offset) const;
    // Encodes a GPU blit between the linear staging buffer and every present level and layer.
    void encodeLinearCopy(id<MTLCommandBuffer> commands, id<MTLBuffer> linear, bool toTexture, Subresources part = {}) const;
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
