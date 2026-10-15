#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"

namespace AgcDriver::Graphics {

bool DccAlphaOnMsb(VkFormat format, std::uint32_t componentSwap) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SRGB:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT:
        return componentSwap == 3;
    default:
        return componentSwap != 2 && componentSwap != 3;
    }
}

std::uint64_t DepthSliceBytes(VkExtent2D extent, std::uint32_t bytesPerTexel) {
    const std::uint32_t blockWidth = bytesPerTexel == 4 ? 128u : 256u;
    const std::uint32_t blockHeight = bytesPerTexel == 1 ? 256u : 128u;
    const auto width = static_cast<std::uint64_t>((extent.width + blockWidth - 1) / blockWidth * blockWidth);
    const auto height = static_cast<std::uint64_t>((extent.height + blockHeight - 1) / blockHeight * blockHeight);
    return width * height * bytesPerTexel;
}

std::uint64_t DepthPlaneBytes(VkExtent2D extent, std::uint32_t bytesPerTexel, std::uint32_t samples) {
    if (samples <= 1) return DepthSliceBytes(extent, bytesPerTexel);
    std::uint32_t pixelBits = 16;  // log2 of the 64 KiB block
    for (std::uint32_t value = bytesPerTexel; value > 1; value >>= 1) --pixelBits;
    for (std::uint32_t value = samples; value > 1; value >>= 1) --pixelBits;
    // Width takes the odd bit, as in the single-sample blocks (256x128 for 2-byte texels).
    const std::uint32_t blockWidth = 1u << ((pixelBits + 1) / 2);
    const std::uint32_t blockHeight = 1u << (pixelBits / 2);
    const auto width = static_cast<std::uint64_t>((extent.width + blockWidth - 1) / blockWidth * blockWidth);
    const auto height = static_cast<std::uint64_t>((extent.height + blockHeight - 1) / blockHeight * blockHeight);
    return width * height * bytesPerTexel * samples;
}

}
