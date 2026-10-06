#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALTEXTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALTEXTURE_HPP

#include "MetalDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include <cstddef>
#include <span>

namespace AgcDriver::Metal {

class MetalTexture {
public:
    MetalTexture(const MetalDevice& backend, const Graphics::GuestTextureResource& descriptor);
    MetalTexture(const MetalDevice& backend, std::span<const std::uint32_t> words);
    void Upload(std::span<const std::byte> guestBytes);
    void Readback(std::span<std::byte> guestBytes) const;
    [[nodiscard]] id<MTLTexture> Texture() const;
    [[nodiscard]] id<MTLTexture> SampledView() const;
    [[nodiscard]] id<MTLTexture> StorageView() const;
    [[nodiscard]] const Graphics::GuestTextureResource& Descriptor() const;
    [[nodiscard]] const Graphics::SurfaceGeometry& Geometry() const;
    [[nodiscard]] std::size_t GuestBytes() const;

private:
    void transfer(id<MTLBuffer> source, id<MTLBuffer> destination, bool retile) const;
    const MetalDevice& backend;
    Graphics::GuestTextureResource descriptor;
    Graphics::SurfaceGeometry geometry;
    std::uint32_t elementBytes;
    id<MTLTexture> texture;
    id<MTLTexture> sampledView;
    id<MTLTexture> storageView;
};

}

#endif
