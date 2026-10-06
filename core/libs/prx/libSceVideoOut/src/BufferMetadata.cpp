#include "prx/libSceVideoOut/include/BufferMetadata.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

AgcDriver::DisplayBuffer DescribeVideoOutBuffer(const VideoOutBuffer& buffer, const BufferAttributeGroup& group) {
    const auto& attribute = group.attribute;
    if (attribute.reserved0 != 0 || attribute.pad0 != 0 || attribute.reserved1[0] != 0 || attribute.reserved1[1] != 0 || attribute.reserved1[2] != 0) throw std::runtime_error("VideoOut: reserved buffer attribute bits are set");
    if (attribute.tiling_mode > 1 || (attribute.tiling_mode == 0 && attribute.pitch_in_pixel != 0) || attribute.aspect_ratio != 0 || (attribute.option != VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_NONE && attribute.option != VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY)) throw std::runtime_error("VideoOut: unsupported tiling " + std::to_string(attribute.tiling_mode) + ", pitch " + std::to_string(attribute.pitch_in_pixel) + ", aspect ratio " + std::to_string(attribute.aspect_ratio) + " or buffer option " + std::to_string(attribute.option));
    const auto hex = [](std::uint64_t value) {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
        return std::string(text);
    };
    const bool compressed = group.category == VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED;
    if (!compressed && group.category != VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED) throw std::runtime_error("VideoOut: unknown buffer category " + std::to_string(group.category));
    if (!compressed && (buffer.metadataAddress != 0 || attribute.dcc_control != 0 || attribute.dcc_cb_register_clear_color != 0)) throw std::runtime_error("VideoOut: uncompressed buffer with DCC metadata " + hex(buffer.metadataAddress) + ", dcc_control " + hex(attribute.dcc_control) + " or DCC clear color " + hex(attribute.dcc_cb_register_clear_color));
    if (compressed && buffer.metadataAddress == 0) throw std::runtime_error("VideoOut: DCC-compressed buffer " + hex(buffer.dataAddress) + " without DCC metadata");
    if (compressed && attribute.tiling_mode != 0) throw std::runtime_error("VideoOut: DCC-compressed buffer " + hex(buffer.dataAddress) + " with linear tiling");
    if (compressed && (attribute.dcc_control & ~VIDEO_OUT_DCC_CONTROL_BLOCK_LAYOUT) != 0) throw std::runtime_error("VideoOut: unsupported DCC control " + hex(attribute.dcc_control) + " (only the block size and independence fields " + hex(VIDEO_OUT_DCC_CONTROL_BLOCK_LAYOUT) + " are known)");
    const AgcDriver::DisplayBuffer result{buffer.dataAddress, attribute.pixel_format, attribute.width, attribute.height, attribute.tiling_mode, attribute.pitch_in_pixel, compressed ? buffer.metadataAddress : 0, compressed ? attribute.dcc_cb_register_clear_color : 0};
    static_cast<void>(AgcDriverDisplayBufferSize_nid_postfix(result));
    return result;
}
