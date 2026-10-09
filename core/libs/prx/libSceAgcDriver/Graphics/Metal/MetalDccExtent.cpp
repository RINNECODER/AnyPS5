// The console's DCC key extent for the native Metal build. The shared definitions live in
// Graphics/src/DccMetadata.cpp, which the Metal build does not compile (its key reads and stores
// go through the Vulkan recorder); these two are pure and must stay identical to them.
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <algorithm>

namespace AgcDriver::Graphics {

std::size_t DccKeyCount(TextureTileMode tileMode, std::uint32_t elementBytes, std::uint32_t width, std::uint32_t height, std::uint64_t surfaceBytes) {
    const auto keys = DccKeyBytes(surfaceBytes);
    if (tileMode != TextureTileMode::kR64KBX || elementBytes != 4) return keys;
    constexpr std::uint64_t MetaBlockEdge = 512;
    constexpr std::uint64_t MetaBlockBytes = 4096;
    const auto console = (width + MetaBlockEdge - 1) / MetaBlockEdge * ((height + MetaBlockEdge - 1) / MetaBlockEdge) * MetaBlockBytes;
    return std::max(keys, static_cast<std::size_t>(console));
}

std::size_t DccKeyCount(const GuestTextureResource& surface, std::uint64_t surfaceBytes) {
    const bool single = surface.mipCount == 1 && surface.baseArray == 0 && surface.depthOrLastArray == 0 && (surface.dimension == TextureDimension::k2D || surface.dimension == TextureDimension::k2DArray);
    if (!single || !surface.dccPipeAligned || surface.tileMode != TextureTileMode::kR64KBX) return DccKeyBytes(surfaceBytes);
    return DccKeyCount(surface.tileMode, BytesPerElement(surface.format), surface.width, surface.height, surfaceBytes);
}

}
