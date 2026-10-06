#pragma once

#include "SceTypes.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"

static constexpr int VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED = 0;
static constexpr int VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_COMPRESSED = 1;
static constexpr std::uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_NONE = 0;
static constexpr std::uint64_t VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_STRICT_COLORIMETRY = 8;
static constexpr std::uint32_t VIDEO_OUT_DCC_CONTROL_BLOCK_LAYOUT = 0x10026c;

struct VideoOutBuffer {
    int groupIndex = -1;
    uint64_t dataAddress = 0;
    uint64_t metadataAddress = 0;

    bool Occupied() const { return groupIndex >= 0; }
};

struct BufferAttributeGroup {
    VideoOutBufferAttribute2 attribute{};
    int category = VIDEO_OUT_BUFFER_ATTRIBUTE_CATEGORY_UNCOMPRESSED;
    bool occupied = false;
};

AgcDriver::DisplayBuffer DescribeVideoOutBuffer(const VideoOutBuffer& buffer, const BufferAttributeGroup& group);
