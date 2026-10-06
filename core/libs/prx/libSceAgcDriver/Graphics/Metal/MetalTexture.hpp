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
    void transfer(id<MTLBuffer> source, id<MTLBuffer> destination, bool retile) const;
    const MetalDevice& backend;
    Graphics::GuestTextureResource descriptor;
    std::shared_ptr<Backing> backing;
    id<MTLTexture> typedTexture;
    id<MTLTexture> sampledView;
    id<MTLTexture> storageView;
    id<MTLTexture> atomicView;
};

}

#endif
