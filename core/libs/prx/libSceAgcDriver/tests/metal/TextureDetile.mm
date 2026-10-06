#include "MetalTestSupport.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

using MetalTests::Context;
using MetalTests::Require;

struct Parameters {
    uint32_t srcBase = 16;
    uint32_t dstBase = 20;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitchBytes = 0;
    uint32_t blocksPerRow = 0;
    uint32_t tail = 0;
    uint32_t tailX = 0;
    uint32_t tailY = 0;
    uint32_t elementBytes = 0;
    uint32_t slice = 0;
    uint32_t rangeBegin = 0;
    uint32_t rangeEnd = UINT32_MAX;
    uint32_t tiledBase = 0;
    uint32_t linearBase = 0;
    uint32_t columnBegin = 0;
    uint32_t rowBegin = 0;
};

struct Fixture {
    std::array<uint32_t, 22> constants{};
    Parameters parameters;
    uint32_t blockWidth = 0;
    uint32_t blockHeight = 0;
    bool interleavedEquation = false;
    MTLSize grid;
};

constexpr std::array<std::array<int, 16>, 5> standardAddressBits{{
    {{0, 1, 2, 3, 12, 13, 14, 15, 16, 4, 17, 5, 18, 6, 19, 7}},
    {{-1, 0, 1, 2, 12, 13, 14, 3, 15, 4, 16, 5, 17, 6, 18, 7}},
    {{-1, -1, 0, 1, 12, 13, 14, 2, 15, 3, 16, 4, 17, 5, 18, 6}},
    {{-1, -1, -1, 0, 12, 13, 1, 2, 14, 3, 15, 4, 16, 5, 17, 6}},
    {{-1, -1, -1, -1, 12, 13, 0, 1, 14, 2, 15, 3, 16, 4, 17, 5}}
}};

uint32_t StandardAddress(uint32_t x, uint32_t y, uint32_t elementBytes, uint32_t blockBytes) {
    unsigned sizeIndex = 0;
    while ((1u << sizeIndex) != elementBytes) ++sizeIndex;
    uint32_t address = 0;
    for (unsigned bit = 0; (1u << bit) < blockBytes; ++bit) {
        const int coordinateBit = standardAddressBits[sizeIndex][bit];
        if (coordinateBit < 0) continue;
        const uint32_t coordinate = coordinateBit < 12 ? x : y;
        const unsigned sourceBit = coordinateBit < 12 ? coordinateBit : coordinateBit - 12;
        if ((coordinate >> sourceBit) & 1u) address |= 1u << bit;
    }
    return address;
}

uint32_t FixtureAddress(const Fixture& fixture, uint32_t x, uint32_t y) {
    const auto& p = fixture.parameters;
    if (fixture.constants[2] == 0) return y * p.pitchBytes + x * p.elementBytes;
    const uint32_t swizzleX = x + (p.tail ? p.tailX : 0);
    const uint32_t swizzleY = y + (p.tail ? p.tailY : 0);
    uint32_t address;
    if (fixture.constants[2] == 2 && fixture.interleavedEquation) {
        address = 0;
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t xBit = (swizzleX >> bit) & 1u;
            uint32_t yBit = (swizzleY >> bit) & 1u;
            if (bit == 0) xBit ^= p.slice & 1u;
            if (bit == 7) yBit ^= (p.slice >> 7) & 1u;
            address += xBit * (1u << (2 * bit)) + yBit * (1u << (2 * bit + 1));
        }
    } else if (fixture.constants[2] == 2) {
        address = (((swizzleX & 1u) ^ ((p.slice >> 1) & 1u)) * 4u) +
                  ((swizzleY & 1u) * 8u) + ((p.slice & 1u) * 16u) +
                  (((swizzleX >> 1) & 1u) * 32u) +
                  (((swizzleY >> 1) & 1u) * 64u) + (((p.slice >> 1) & 1u) * 128u);
    } else {
        address = StandardAddress(swizzleX, swizzleY, p.elementBytes, fixture.constants[1]);
    }
    if (!p.tail) {
        address += ((y / fixture.blockHeight) * p.blocksPerRow + x / fixture.blockWidth) * fixture.constants[1];
    }
    return address;
}

void Verify(const Context& context, Fixture fixture, uint32_t retile) {
    fixture.constants[3] = retile;
    const auto& p = fixture.parameters;
    uint32_t largestTiled = 0;
    for (uint32_t y = 0; y < p.height; ++y) {
        for (uint32_t x = 0; x < p.width; ++x) largestTiled = std::max(largestTiled, FixtureAddress(fixture, x, y));
    }
    const size_t tiledSize = std::max<size_t>(largestTiled + p.elementBytes, fixture.constants[1]);
    const size_t linearSize = p.height * p.pitchBytes;
    const size_t inputSize = (retile ? linearSize : tiledSize) + p.srcBase + 64;
    const size_t outputSize = (retile ? tiledSize : linearSize) + p.dstBase + 64;
    id<MTLBuffer> input = context.Buffer(inputSize);
    id<MTLBuffer> output = context.Buffer(outputSize, 0xa7);
    auto* inputBytes = static_cast<uint8_t*>(input.contents);
    for (size_t i = 0; i < inputSize; ++i) inputBytes[i] = static_cast<uint8_t>((i * 73u + (i / 251u) * 19u + 11u) & 255u);
    std::vector<uint8_t> expected(outputSize, 0xa7);
    size_t moved = 0;
    for (uint32_t gy = 0; gy < fixture.grid.height; ++gy) {
        for (uint32_t gx = 0; gx < fixture.grid.width; ++gx) {
            const uint32_t x = gx + p.columnBegin;
            const uint32_t y = gy + p.rowBegin;
            if (x >= p.width || y >= p.height) continue;
            const uint32_t tiled = FixtureAddress(fixture, x, y);
            if (tiled < p.rangeBegin || tiled >= p.rangeEnd) continue;
            const uint32_t linear = y * p.pitchBytes + x * p.elementBytes;
            Require(tiled >= p.tiledBase && linear >= p.linearBase, "texture fixture window underflows");
            const uint32_t source = p.srcBase + (retile ? linear - p.linearBase : tiled - p.tiledBase);
            const uint32_t destination = p.dstBase + (retile ? tiled - p.tiledBase : linear - p.linearBase);
            Require(source + p.elementBytes <= inputSize && destination + p.elementBytes <= outputSize, "texture fixture address exceeds buffer");
            std::memcpy(expected.data() + destination, inputBytes + source, p.elementBytes);
            ++moved;
        }
    }
    Require(moved != 0, "texture fixture must exercise element copies");
    context.Dispatch(@"TextureDetile", @[input, output], &p, sizeof(p), fixture.grid, &fixture.constants);
    const auto* actual = static_cast<const uint8_t*>(output.contents);
    if (std::memcmp(actual, expected.data(), outputSize) != 0) {
        for (size_t i = 0; i < outputSize; ++i) {
            if (actual[i] != expected[i]) {
                std::cerr << "Texture mismatch: element=" << p.elementBytes << " block=" << fixture.constants[1]
                          << " family=" << fixture.constants[2] << " retile=" << retile << " slice=" << p.slice
                          << " tail=" << p.tail << " window=" << p.columnBegin << ',' << p.rowBegin
                          << " byte=" << i << " actual=" << unsigned(actual[i]) << " expected=" << unsigned(expected[i]) << '\n';
                break;
            }
        }
        Require(false, "texture output differs from independent address fixture or modifies padding");
    }
}

Fixture StandardFixture(uint32_t elementBytes, uint32_t blockBytes) {
    Fixture fixture;
    fixture.constants[0] = elementBytes;
    fixture.constants[1] = blockBytes;
    fixture.constants[2] = 1;
    const std::array<uint32_t, 5> widths{16, 16, 8, 8, 4};
    unsigned sizeIndex = 0;
    while ((1u << sizeIndex) != elementBytes) ++sizeIndex;
    fixture.blockWidth = widths[sizeIndex] * (blockBytes == 256 ? 1 : blockBytes == 4096 ? 4 : 16);
    fixture.blockHeight = blockBytes / (fixture.blockWidth * elementBytes);
    auto& p = fixture.parameters;
    p.elementBytes = elementBytes;
    p.width = fixture.blockWidth + 3;
    p.height = fixture.blockHeight + 3;
    p.pitchBytes = (p.width + 7) * elementBytes;
    p.blocksPerRow = 3;
    fixture.grid = MTLSizeMake(p.width + 3, p.height + 2, 1);
    return fixture;
}

}

void RunTextureDetileTests(const Context& context) {
    for (const uint32_t elementBytes : {1u, 2u, 4u, 8u, 16u}) {
        for (const uint32_t blockBytes : {256u, 4096u, 65536u}) {
            const auto fixture = StandardFixture(elementBytes, blockBytes);
            Verify(context, fixture, 0);
            Verify(context, fixture, 1);
        }
        auto linear = StandardFixture(elementBytes, 256);
        linear.constants[2] = 0;
        linear.parameters.width = 11;
        linear.parameters.height = 5;
        linear.parameters.pitchBytes = 16 * elementBytes;
        linear.parameters.columnBegin = 1;
        linear.parameters.rowBegin = 1;
        linear.parameters.rangeBegin = 17 * elementBytes;
        linear.parameters.rangeEnd = 43 * elementBytes;
        linear.parameters.tiledBase = 16 * elementBytes;
        linear.parameters.linearBase = 16 * elementBytes;
        linear.parameters.srcBase = elementBytes < 4 ? elementBytes : 16;
        linear.parameters.dstBase = elementBytes < 4 ? elementBytes : 20;
        linear.grid = MTLSizeMake(7, 4, 1);
        Verify(context, linear, 0);
        Verify(context, linear, 1);

        auto tail = StandardFixture(elementBytes, 4096);
        tail.parameters.width = 3;
        tail.parameters.height = 3;
        tail.parameters.tail = 1;
        tail.parameters.tailX = 5;
        tail.parameters.tailY = 7;
        tail.grid = MTLSizeMake(6, 5, 1);
        Verify(context, tail, 0);
        Verify(context, tail, 1);
    }
    for (const uint32_t blockBytes : {256u, 4096u, 65536u}) {
        auto window = StandardFixture(4, blockBytes);
        auto& p = window.parameters;
        p.columnBegin = window.blockWidth;
        p.rowBegin = 1;
        p.rangeBegin = blockBytes;
        p.rangeEnd = blockBytes + 32;
        p.tiledBase = blockBytes;
        p.linearBase = p.pitchBytes;
        window.grid = MTLSizeMake(8, 7, 1);
        Verify(context, window, 0);
        Verify(context, window, 1);
    }
    Fixture equation;
    equation.constants[0] = 4;
    equation.constants[1] = 256;
    equation.constants[2] = 2;
    equation.constants[6] = (1u << 0) | (1u << 25);
    equation.constants[7] = 1u << 12;
    equation.constants[8] = 1u << 24;
    equation.constants[9] = 1u << 1;
    equation.constants[10] = 1u << 13;
    equation.constants[11] = 1u << 25;
    equation.constants[20] = equation.blockWidth = 4;
    equation.constants[21] = equation.blockHeight = 4;
    equation.parameters.elementBytes = 4;
    equation.parameters.width = 7;
    equation.parameters.height = 7;
    equation.parameters.pitchBytes = 40;
    equation.parameters.blocksPerRow = 3;
    equation.grid = MTLSizeMake(9, 9, 1);
    for (uint32_t slice = 0; slice < 4; ++slice) {
        equation.parameters.slice = slice;
        Verify(context, equation, 0);
        Verify(context, equation, 1);
    }
    equation.parameters.tail = 1;
    equation.parameters.tailX = 1;
    equation.parameters.tailY = 1;
    equation.parameters.width = 2;
    equation.parameters.height = 2;
    equation.parameters.columnBegin = 1;
    equation.parameters.rowBegin = 1;
    equation.parameters.rangeBegin = 128;
    equation.parameters.rangeEnd = 256;
    equation.grid = MTLSizeMake(3, 3, 1);
    Verify(context, equation, 0);
    Verify(context, equation, 1);

    Fixture largeEquation;
    largeEquation.constants[0] = largeEquation.parameters.elementBytes = 1;
    largeEquation.constants[1] = 65536;
    largeEquation.constants[2] = 2;
    largeEquation.interleavedEquation = true;
    for (unsigned bit = 0; bit < 8; ++bit) {
        largeEquation.constants[4 + 2 * bit] = 1u << bit;
        largeEquation.constants[5 + 2 * bit] = 1u << (bit + 12);
    }
    largeEquation.constants[4] |= 1u << 24;
    largeEquation.constants[19] |= 1u << 31;
    largeEquation.constants[20] = largeEquation.blockWidth = 256;
    largeEquation.constants[21] = largeEquation.blockHeight = 256;
    largeEquation.parameters.width = 259;
    largeEquation.parameters.height = 259;
    largeEquation.parameters.pitchBytes = 264;
    largeEquation.parameters.blocksPerRow = 3;
    largeEquation.grid = MTLSizeMake(260, 260, 1);
    for (uint32_t slice : {0u, 128u, 129u}) {
        largeEquation.parameters.slice = slice;
        Verify(context, largeEquation, 0);
        Verify(context, largeEquation, 1);
    }
}
