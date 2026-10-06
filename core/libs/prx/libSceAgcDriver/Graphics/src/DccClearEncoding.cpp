#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

constexpr std::uint64_t KeyBytes = 256;

bool AllKeysEqual(const std::uint8_t* keys, std::size_t count, std::uint8_t first) {
    static const bool byteScan = std::getenv("APS5_NO_DCC_WORD_SCAN") != nullptr;
    if (byteScan) return std::all_of(keys, keys + count, [&](std::uint8_t key) { return key == first; });
    const std::uint64_t pattern = 0x0101010101010101ull * first;
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        std::uint64_t word;
        std::memcpy(&word, keys + i, sizeof(word));
        if (word != pattern) return false;
    }
    for (; i < count; ++i) {
        if (keys[i] != first) return false;
    }
    return true;
}

enum class Kind { Unorm, Snorm, Uint, Sint, Float };

struct Layout {
    std::uint32_t channels;
    std::uint32_t bits[4];
    Kind kind;
};

bool LayoutFor(VkFormat format, Layout& layout) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: layout = {1, {8}, Kind::Unorm}; return true;
        case VK_FORMAT_R8_UINT: layout = {1, {8}, Kind::Uint}; return true;
        case VK_FORMAT_R16_UNORM: layout = {1, {16}, Kind::Unorm}; return true;
        case VK_FORMAT_R16_SNORM: layout = {1, {16}, Kind::Snorm}; return true;
        case VK_FORMAT_R16_UINT: layout = {1, {16}, Kind::Uint}; return true;
        case VK_FORMAT_R16_SINT: layout = {1, {16}, Kind::Sint}; return true;
        case VK_FORMAT_R16_SFLOAT: layout = {1, {16}, Kind::Float}; return true;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SRGB: layout = {2, {8, 8}, Kind::Unorm}; return true;
        case VK_FORMAT_R8G8_SNORM: layout = {2, {8, 8}, Kind::Snorm}; return true;
        case VK_FORMAT_R8G8_UINT: layout = {2, {8, 8}, Kind::Uint}; return true;
        case VK_FORMAT_R8G8_SINT: layout = {2, {8, 8}, Kind::Sint}; return true;
        case VK_FORMAT_R32_UINT: layout = {1, {32}, Kind::Uint}; return true;
        case VK_FORMAT_R32_SINT: layout = {1, {32}, Kind::Sint}; return true;
        case VK_FORMAT_R32_SFLOAT: layout = {1, {32}, Kind::Float}; return true;
        case VK_FORMAT_R16G16_UNORM: layout = {2, {16, 16}, Kind::Unorm}; return true;
        case VK_FORMAT_R16G16_SNORM: layout = {2, {16, 16}, Kind::Snorm}; return true;
        case VK_FORMAT_R16G16_UINT: layout = {2, {16, 16}, Kind::Uint}; return true;
        case VK_FORMAT_R16G16_SINT: layout = {2, {16, 16}, Kind::Sint}; return true;
        case VK_FORMAT_R16G16_SFLOAT: layout = {2, {16, 16}, Kind::Float}; return true;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: layout = {4, {10, 10, 10, 2}, Kind::Unorm}; return true;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: layout = {4, {10, 10, 10, 2}, Kind::Uint}; return true;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: layout = {4, {8, 8, 8, 8}, Kind::Unorm}; return true;
        case VK_FORMAT_R8G8B8A8_SNORM: layout = {4, {8, 8, 8, 8}, Kind::Snorm}; return true;
        case VK_FORMAT_R8G8B8A8_UINT: layout = {4, {8, 8, 8, 8}, Kind::Uint}; return true;
        case VK_FORMAT_R8G8B8A8_SINT: layout = {4, {8, 8, 8, 8}, Kind::Sint}; return true;
        case VK_FORMAT_R32G32_UINT: layout = {2, {32, 32}, Kind::Uint}; return true;
        case VK_FORMAT_R32G32_SINT: layout = {2, {32, 32}, Kind::Sint}; return true;
        case VK_FORMAT_R32G32_SFLOAT: layout = {2, {32, 32}, Kind::Float}; return true;
        case VK_FORMAT_R16G16B16A16_UNORM: layout = {4, {16, 16, 16, 16}, Kind::Unorm}; return true;
        case VK_FORMAT_R16G16B16A16_SNORM: layout = {4, {16, 16, 16, 16}, Kind::Snorm}; return true;
        case VK_FORMAT_R16G16B16A16_UINT: layout = {4, {16, 16, 16, 16}, Kind::Uint}; return true;
        case VK_FORMAT_R16G16B16A16_SINT: layout = {4, {16, 16, 16, 16}, Kind::Sint}; return true;
        case VK_FORMAT_R16G16B16A16_SFLOAT: layout = {4, {16, 16, 16, 16}, Kind::Float}; return true;
        case VK_FORMAT_R32G32B32A32_UINT: layout = {4, {32, 32, 32, 32}, Kind::Uint}; return true;
        case VK_FORMAT_R32G32B32A32_SINT: layout = {4, {32, 32, 32, 32}, Kind::Sint}; return true;
        case VK_FORMAT_R32G32B32A32_SFLOAT: layout = {4, {32, 32, 32, 32}, Kind::Float}; return true;
        default: return false;
    }
}

// "1" in a channel of this kind and width.
std::uint64_t One(Kind kind, std::uint32_t bits) {
    switch (kind) {
        case Kind::Unorm: case Kind::Uint: return bits >= 64 ? ~0ull : (1ull << bits) - 1u;
        case Kind::Snorm: case Kind::Sint: return (1ull << (bits - 1u)) - 1u;
        case Kind::Float: return bits == 16 ? 0x3c00u : 0x3f800000u;
    }
    return 0;
}

}

const char* DccKeysName(DccKeys keys) {
    switch (keys) {
        case DccKeys::Uncompressed: return "uncompressed";
        case DccKeys::Clear0000: return "0000";
        case DccKeys::Clear0001: return "0001";
        case DccKeys::Clear1110: return "1110";
        case DccKeys::Clear1111: return "1111";
        case DccKeys::ClearRegister: return "register";
        case DccKeys::Mixed: return "mixed";
        case DccKeys::Unreadable: return "unreadable";
    }
    return "?";
}

std::size_t DccKeyBytes(std::uint64_t surfaceBytes) {
    return static_cast<std::size_t>(surfaceBytes / KeyBytes);
}

DccKeys ClassifyDccKeys(std::span<const std::byte> keys) {
    if (keys.empty()) return DccKeys::Unreadable;
    const auto first = std::to_integer<std::uint8_t>(keys.front());
    if (!AllKeysEqual(reinterpret_cast<const std::uint8_t*>(keys.data()) + 1, keys.size() - 1, first)) return DccKeys::Mixed;
    switch (first) {
        case 0x00: return DccKeys::Clear0000;
        case 0x40: return DccKeys::Clear0001;
        case 0x80: return DccKeys::Clear1110;
        case 0xc0: return DccKeys::Clear1111;
        case 0x20: return DccKeys::ClearRegister;
        case 0xff: return DccKeys::Uncompressed;
        default: return DccKeys::Mixed;
    }
}

bool IsDccClear(DccKeys keys) {
    return keys == DccKeys::Clear0000 || keys == DccKeys::Clear0001 || keys == DccKeys::Clear1110 || keys == DccKeys::Clear1111 || keys == DccKeys::ClearRegister;
}

bool FillDccClear(VkFormat format, DccKeys keys, bool alphaOnMsb, std::span<std::byte> bytes) {
    if (keys == DccKeys::Clear0000) {
        std::fill(bytes.begin(), bytes.end(), std::byte{0});
        return true;
    }
    Layout layout{};
    if (!IsDccClear(keys) || keys == DccKeys::ClearRegister || !LayoutFor(format, layout)) return false;
    const bool color = keys == DccKeys::Clear1110 || keys == DccKeys::Clear1111;
    const bool alpha = keys == DccKeys::Clear0001 || keys == DccKeys::Clear1111;
    const int alphaChannel = layout.channels == 3 ? -1 : alphaOnMsb ? static_cast<int>(layout.channels) - 1 : 0;
    std::uint32_t elementBits = 0;
    for (std::uint32_t channel = 0; channel < layout.channels; ++channel) elementBits += layout.bits[channel];
    std::vector<std::byte> element(elementBits / 8u);
    std::uint32_t bit = 0;
    for (std::uint32_t channel = 0; channel < layout.channels; ++channel) {
        const bool set = static_cast<int>(channel) == alphaChannel ? alpha : color;
        const auto value = set ? One(layout.kind, layout.bits[channel]) : 0u;
        for (std::uint32_t i = 0; i < layout.bits[channel]; ++i, ++bit) {
            if (((value >> i) & 1u) != 0) element[bit / 8u] |= static_cast<std::byte>(1u << (bit % 8u));
        }
    }
    for (std::size_t offset = 0; offset + element.size() <= bytes.size(); offset += element.size()) std::memcpy(bytes.data() + offset, element.data(), element.size());
    return true;
}

}
