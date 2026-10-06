#include "MetalTestSupport.hpp"
#include "MetalPresenter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

namespace {

using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel black{0, 0, 0, 255};
constexpr std::array<Pixel, 4> corners{{{255, 0, 0, 17}, {0, 255, 0, 85}, {0, 0, 255, 170}, {255, 255, 255, 255}}};

bool IsBgra(MTLPixelFormat format) {
    return format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB;
}

std::vector<Pixel> Render(const MetalTests::Context& context, const AgcDriver::Metal::MetalPresenter& presenter,
                          std::uint32_t sourceWidth, std::uint32_t sourceHeight, std::span<const Pixel> pixels,
                          std::uint32_t width, std::uint32_t height, bool linear,
                          MTLPixelFormat sourceFormat = MTLPixelFormatRGBA8Unorm,
                          MTLPixelFormat targetFormat = MTLPixelFormatRGBA8Unorm) {
    auto sourceDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:sourceFormat width:sourceWidth height:sourceHeight mipmapped:NO];
    sourceDescriptor.storageMode = MTLStorageModeShared;
    sourceDescriptor.usage = MTLTextureUsageShaderRead;
    auto source = [context.device newTextureWithDescriptor:sourceDescriptor];
    MetalTests::Require(source != nil, "Presentation source allocation failed");
    std::vector<Pixel> stored(pixels.begin(), pixels.end());
    if (IsBgra(sourceFormat)) for (auto& pixel : stored) std::swap(pixel[0], pixel[2]);
    [source replaceRegion:MTLRegionMake2D(0, 0, sourceWidth, sourceHeight) mipmapLevel:0 withBytes:stored.data() bytesPerRow:sourceWidth * sizeof(Pixel)];
    auto targetDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:targetFormat width:width height:height mipmapped:NO];
    targetDescriptor.storageMode = MTLStorageModePrivate;
    targetDescriptor.usage = MTLTextureUsageRenderTarget;
    auto target = [context.device newTextureWithDescriptor:targetDescriptor];
    MetalTests::Require(target != nil, "Presentation target allocation failed");
    const std::size_t rowBytes = (width * sizeof(Pixel) + 255u) & ~std::size_t{255u};
    auto readback = context.Buffer(rowBytes * height);
    auto commands = [context.queue commandBuffer];
    presenter.Encode(commands, source, target, linear);
    auto blit = [commands blitCommandEncoder];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1) toBuffer:readback destinationOffset:0
               destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes * height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Presentation GPU command failed");
    std::vector<Pixel> result(width * height);
    for (std::uint32_t y = 0; y < height; ++y) std::memcpy(result.data() + y * width, static_cast<const std::byte*>(readback.contents) + y * rowBytes, width * sizeof(Pixel));
    if (IsBgra(targetFormat)) for (auto& pixel : result) std::swap(pixel[0], pixel[2]);
    return result;
}

void Match(std::span<const Pixel> actual, std::span<const Pixel> expected, int tolerance = 0) {
    MetalTests::Require(actual.size() == expected.size(), "Presentation output extent differs");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            MetalTests::Require(std::abs(static_cast<int>(actual[i][channel]) - expected[i][channel]) <= tolerance, "Presentation pixel differs from independent expected output");
        }
    }
}

}

void RunPresentationTests(const MetalTests::Context& context) {
    AgcDriver::Metal::MetalPresenter presenter(context.device, context.library);
    for (const auto sourceFormat : {MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm}) {
        for (const auto targetFormat : {MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm}) {
            Match(Render(context, presenter, 2, 2, corners, 2, 2, false, sourceFormat, targetFormat), corners);
        }
    }
    for (const auto extent : {std::array<std::uint32_t, 2>{4, 4}, {6, 4}, {4, 6}, {7, 4}}) {
        const auto width = extent[0];
        const auto height = extent[1];
        std::vector<Pixel> expected(width * height, black);
        const std::uint32_t xStart = width == 4 ? 0 : 1;
        const std::uint32_t yStart = height == 6 ? 1 : 0;
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) expected[(y + yStart) * width + x + xStart] = corners[(y / 2) * 2 + x / 2];
        }
        Match(Render(context, presenter, 2, 2, corners, width, height, false), expected);
    }
    constexpr std::array<Pixel, 9> filtered{{
        {255, 0, 0, 17}, {128, 128, 0, 51}, {0, 255, 0, 85},
        {128, 0, 128, 94}, {128, 128, 128, 132}, {128, 255, 128, 170},
        {0, 0, 255, 170}, {128, 128, 255, 213}, {255, 255, 255, 255}
    }};
    Match(Render(context, presenter, 2, 2, corners, 3, 3, true), filtered, 1);
    constexpr std::array<Pixel, 1> average{{{128, 128, 128, 132}}};
    Match(Render(context, presenter, 2, 2, corners, 1, 1, true), average, 1);
    constexpr std::array<Pixel, 1> encoded{{{128, 128, 128, 255}}};
    constexpr std::array<Pixel, 1> decoded{{{55, 55, 55, 255}}};
    constexpr std::array<Pixel, 1> linearEncoded{{{188, 188, 188, 255}}};
    Match(Render(context, presenter, 1, 1, encoded, 1, 1, true, MTLPixelFormatRGBA8Unorm_sRGB), decoded, 1);
    Match(Render(context, presenter, 1, 1, encoded, 1, 1, true, MTLPixelFormatRGBA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB), linearEncoded, 1);
    Match(Render(context, presenter, 1, 1, encoded, 1, 1, true, MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatRGBA8Unorm_sRGB), encoded, 1);
}
