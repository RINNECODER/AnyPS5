#include "MetalTestSupport.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

struct Parameters {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t blocksPerRow;
    std::uint32_t mode;
};

constexpr std::uint32_t sentinel = 0xa5a5a5a5u;
constexpr std::array<std::uint32_t, 15> packedPixels{
    0x00000000u, 0x00000003u, 0x00000004u, 0x000003ffu, 0x00000c00u,
    0x00001000u, 0x000ffc00u, 0x00300000u, 0x00400000u, 0x3ff00000u,
    0x3fffffffu, 0x40000000u, 0x80000000u, 0xc0000000u, 0x80c02004u
};
constexpr std::array<std::uint32_t, 15> unpackedPixels{
    0xff000000u, 0xff000000u, 0xff000001u, 0xff0000ffu, 0xff000000u,
    0xff000100u, 0xff00ff00u, 0xff000000u, 0xff010000u, 0xffff0000u,
    0xffffffffu, 0xff000000u, 0xff000000u, 0xff000000u, 0xff030201u
};

std::size_t SwizzledIndex(std::uint32_t x, std::uint32_t y, std::uint32_t blocksPerRow) {
    constexpr std::array<std::uint32_t, 7> xBits{1u, 2u, 32u, 64u, 2176u, 512u, 8448u};
    constexpr std::array<std::uint32_t, 7> yBits{4u, 8u, 16u, 1088u, 128u, 256u, 4608u};
    std::size_t offset = 0;
    for (std::size_t bit = 0; bit < xBits.size(); ++bit) {
        if (x & (1u << bit)) offset ^= xBits[bit];
        if (y & (1u << bit)) offset ^= yBits[bit];
    }
    return ((y / 128u) * blocksPerRow + x / 128u) * 16384u + offset;
}

std::uint32_t SourcePixel(std::size_t index, bool packed) {
    return packed ? packedPixels[index % packedPixels.size()] : static_cast<std::uint32_t>(index) * 0x01020305u + 0x40201008u;
}

std::uint32_t ExpectedPixel(std::size_t index, std::uint32_t mode) {
    std::uint32_t pixel = (mode & 8u) ? unpackedPixels[index % unpackedPixels.size()] : SourcePixel(index, false);
    if (mode & 2u) {
        std::array<std::uint8_t, 4> channels;
        std::memcpy(channels.data(), &pixel, sizeof(pixel));
        std::swap(channels[0], channels[2]);
        std::memcpy(&pixel, channels.data(), sizeof(pixel));
    }
    return pixel;
}

void CheckMode(const MetalTests::Context& context, std::uint32_t width, std::uint32_t height, std::uint32_t mode) {
    const Parameters params{width, height, (width + 127u) / 128u, mode};
    const std::size_t activePixels = static_cast<std::size_t>(width) * height;
    const std::size_t tiledPixels = (mode & 4u) ? static_cast<std::size_t>(params.blocksPerRow) * ((height + 127u) / 128u) * 16384u : activePixels;
    std::vector<std::uint32_t> expectedTiled(tiledPixels + 64u, sentinel);
    std::vector<std::uint32_t> expectedLinear(activePixels + 64u, sentinel);
    id<MTLBuffer> tiled = context.Buffer(expectedTiled.size() * sizeof(std::uint32_t), 0xa5);
    id<MTLBuffer> linear = context.Buffer(expectedLinear.size() * sizeof(std::uint32_t), 0xa5);
    auto* tiledWords = static_cast<std::uint32_t*>(tiled.contents);
    auto* linearWords = static_cast<std::uint32_t*>(linear.contents);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t linearIndex = static_cast<std::size_t>(y) * width + x;
            const std::size_t tiledIndex = (mode & 4u) ? SwizzledIndex(x, y, params.blocksPerRow) : linearIndex;
            const auto source = SourcePixel(linearIndex, (mode & 8u) != 0u);
            if (mode & 1u) {
                linearWords[linearIndex] = source;
                expectedLinear[linearIndex] = source;
                expectedTiled[tiledIndex] = ExpectedPixel(linearIndex, mode);
            } else {
                tiledWords[tiledIndex] = source;
                expectedTiled[tiledIndex] = source;
                expectedLinear[linearIndex] = ExpectedPixel(linearIndex, mode);
            }
        }
    }
    context.Dispatch(@"ColorTransfer", @[tiled, linear], &params, sizeof(params), MTLSizeMake(width + 7u, height + 7u, 1u));
    MetalTests::Require(std::equal(expectedTiled.begin(), expectedTiled.end(), tiledWords), "Metal color transfer changed tiled pixels, source bytes, or padding");
    MetalTests::Require(std::equal(expectedLinear.begin(), expectedLinear.end(), linearWords), "Metal color transfer changed linear pixels, source bytes, or padding");
}

void CheckSwizzleGolden(const MetalTests::Context& context) {
    struct Address {
        std::uint32_t x;
        std::uint32_t y;
        std::uint32_t word;
    };
    constexpr std::array<Address, 20> addresses{{
        {0u, 0u, 0u}, {1u, 0u, 1u}, {2u, 0u, 2u}, {4u, 0u, 32u},
        {8u, 0u, 64u}, {16u, 0u, 2176u}, {32u, 0u, 512u}, {64u, 0u, 8448u},
        {0u, 1u, 4u}, {0u, 2u, 8u}, {0u, 4u, 16u}, {0u, 8u, 1088u},
        {0u, 16u, 128u}, {0u, 32u, 256u}, {0u, 64u, 4608u},
        {8u, 8u, 1024u}, {16u, 16u, 2048u}, {128u, 0u, 16384u},
        {0u, 128u, 32768u}, {129u, 128u, 49153u}
    }};
    const Parameters params{130u, 129u, 2u, 5u};
    id<MTLBuffer> tiled = context.Buffer(4u * 65536u, 0xa5);
    id<MTLBuffer> linear = context.Buffer(params.width * params.height * sizeof(std::uint32_t));
    auto* linearWords = static_cast<std::uint32_t*>(linear.contents);
    for (std::size_t i = 0; i < static_cast<std::size_t>(params.width) * params.height; ++i) linearWords[i] = static_cast<std::uint32_t>(i + 1u);
    context.Dispatch(@"ColorTransfer", @[tiled, linear], &params, sizeof(params), MTLSizeMake(136u, 136u, 1u));
    const auto* tiledWords = static_cast<const std::uint32_t*>(tiled.contents);
    for (const auto& address : addresses) {
        MetalTests::Require(tiledWords[address.word] == address.y * params.width + address.x + 1u, "Metal color transfer differs from 64KB swizzle golden addresses");
    }
}

void CheckInactiveExtent(const MetalTests::Context& context) {
    for (const auto& extent : std::array<std::array<std::uint32_t, 2>, 3>{{{0u, 0u}, {0u, 9u}, {9u, 0u}}}) {
        for (std::uint32_t mode = 0; mode < 16u; ++mode) {
            const Parameters params{extent[0], extent[1], 1u, mode};
            id<MTLBuffer> tiled = context.Buffer(256u, 0xa5);
            id<MTLBuffer> linear = context.Buffer(256u, 0xa5);
            context.Dispatch(@"ColorTransfer", @[tiled, linear], &params, sizeof(params), MTLSizeMake(16u, 16u, 1u));
            const auto* tiledBytes = static_cast<const std::uint8_t*>(tiled.contents);
            const auto* linearBytes = static_cast<const std::uint8_t*>(linear.contents);
            MetalTests::Require(std::all_of(tiledBytes, tiledBytes + 256u, [](auto byte) { return byte == 0xa5; }), "Metal color transfer wrote a tiled buffer with an inactive extent");
            MetalTests::Require(std::all_of(linearBytes, linearBytes + 256u, [](auto byte) { return byte == 0xa5; }), "Metal color transfer wrote a linear buffer with an inactive extent");
        }
    }
}

}

void RunColorTransferTests(const MetalTests::Context& context) {
    for (std::uint32_t mode = 0; mode < 16u; ++mode) {
        CheckMode(context, 5u, 3u, mode);
        CheckMode(context, 130u, 129u, mode);
    }
    CheckSwizzleGolden(context);
    CheckInactiveExtent(context);
}
