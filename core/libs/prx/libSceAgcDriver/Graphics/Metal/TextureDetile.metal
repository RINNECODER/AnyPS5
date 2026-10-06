#include <metal_stdlib>
using namespace metal;

constant uint ELEMENT_BYTES [[function_constant(0)]];
constant uint BLOCK_BYTES [[function_constant(1)]];
constant uint TILE_FAMILY [[function_constant(2)]];
constant uint RETILE [[function_constant(3)]];
constant uint EQ0 [[function_constant(4)]];
constant uint EQ1 [[function_constant(5)]];
constant uint EQ2 [[function_constant(6)]];
constant uint EQ3 [[function_constant(7)]];
constant uint EQ4 [[function_constant(8)]];
constant uint EQ5 [[function_constant(9)]];
constant uint EQ6 [[function_constant(10)]];
constant uint EQ7 [[function_constant(11)]];
constant uint EQ8 [[function_constant(12)]];
constant uint EQ9 [[function_constant(13)]];
constant uint EQ10 [[function_constant(14)]];
constant uint EQ11 [[function_constant(15)]];
constant uint EQ12 [[function_constant(16)]];
constant uint EQ13 [[function_constant(17)]];
constant uint EQ14 [[function_constant(18)]];
constant uint EQ15 [[function_constant(19)]];
constant uint BLOCK_WIDTH [[function_constant(20)]];
constant uint BLOCK_HEIGHT [[function_constant(21)]];

struct TextureDetileParameters {
    uint srcBase;
    uint dstBase;
    uint width;
    uint height;
    uint pitchBytes;
    uint blocksPerRow;
    uint tail;
    uint tailX;
    uint tailY;
    uint elementBytes;
    uint slice;
    uint rangeBegin;
    uint rangeEnd;
    uint tiledBase;
    uint linearBase;
    uint columnBegin;
    uint rowBegin;
};

uint equationBit(uint mask, uint2 p, constant TextureDetileParameters& params) {
    uint selected = (p.x & (mask & 0xfffu)) ^ ((p.y << 12) & (mask & 0xfff000u)) ^ ((params.slice << 24) & (mask & 0xff000000u));
    return popcount(selected) & 1u;
}

uint equationOffset(uint2 p, constant TextureDetileParameters& params) {
    const uint eq[16] = {EQ0, EQ1, EQ2, EQ3, EQ4, EQ5, EQ6, EQ7, EQ8, EQ9, EQ10, EQ11, EQ12, EQ13, EQ14, EQ15};
    uint offset = 0u;
    for (uint bit = 0u; bit < 16u; ++bit) {
        offset |= equationBit(eq[bit], p, params) << bit;
    }
    return offset;
}

uint standardOffset(uint x, uint y) {
    switch (ELEMENT_BYTES) {
        case 1u:
            return ((y << 4) & 0x1f0u) ^ ((y << 5) & 0x400u) ^ (x & 0x00fu) ^
                   ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
        case 2u:
            return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
                   ((x << 1) & 0x00eu) ^ ((x << 4) & 0x080u) ^
                   ((x << 5) & 0x200u) ^ ((x << 6) & 0x800u);
        case 4u:
            return ((y << 4) & 0x070u) ^ ((y << 5) & 0x100u) ^ ((y << 6) & 0x400u) ^
                   ((x << 2) & 0x00cu) ^ ((x << 5) & 0x080u) ^
                   ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
        case 8u:
            return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
                   ((x << 3) & 0x008u) ^ ((x << 5) & 0x0c0u) ^
                   ((x << 6) & 0x200u) ^ ((x << 7) & 0x800u);
        default:
            return ((y << 4) & 0x030u) ^ ((y << 6) & 0x100u) ^ ((y << 7) & 0x400u) ^
                   ((x << 6) & 0x0c0u) ^ ((x << 7) & 0x200u) ^ ((x << 8) & 0x800u);
    }
}

uint standard64Extra(uint x, uint y) {
    switch (ELEMENT_BYTES) {
        case 1u:
            return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
                   ((y << 6) & 0x1000u) ^ ((y << 7) & 0x4000u);
        case 2u:
            return ((x << 7) & 0x2000u) ^ ((x << 8) & 0x8000u) ^
                   ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
        case 4u:
            return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
                   ((y << 7) & 0x1000u) ^ ((y << 8) & 0x4000u);
        case 8u:
            return ((x << 8) & 0x2000u) ^ ((x << 9) & 0x8000u) ^
                   ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
        default:
            return ((x << 9) & 0x2000u) ^ ((x << 10) & 0x8000u) ^
                   ((y << 8) & 0x1000u) ^ ((y << 9) & 0x4000u);
    }
}


uint blockOffset(uint2 p, constant TextureDetileParameters& params) {
    if (TILE_FAMILY == 2u) {
        return equationOffset(p, params);
    }
    uint offset = standardOffset(p.x, p.y);
    if (BLOCK_BYTES > 4096u) {
        offset ^= standard64Extra(p.x, p.y);
    }
    return offset & (BLOCK_BYTES - 1u);
}

uint2 blockExtent() {
    if (BLOCK_WIDTH != 0u) {
        return uint2(BLOCK_WIDTH, BLOCK_HEIGHT);
    }
    uint width;
    if (BLOCK_BYTES <= 256u) {
        width = ELEMENT_BYTES <= 2u ? 16u : (ELEMENT_BYTES <= 8u ? 8u : 4u);
    } else if (BLOCK_BYTES <= 4096u) {
        width = ELEMENT_BYTES <= 2u ? 64u : (ELEMENT_BYTES <= 8u ? 32u : 16u);
    } else {
        width = ELEMENT_BYTES <= 2u ? 256u : (ELEMENT_BYTES <= 8u ? 128u : 64u);
    }
    return uint2(width, BLOCK_BYTES / (width * ELEMENT_BYTES));
}

void copyElement(uint src, uint dst, device const uint* inputBuffer, device uint* outputBuffer) {
    if (ELEMENT_BYTES >= 4u) {
        for (uint i = 0u; i < ELEMENT_BYTES; i += 4u) {
            outputBuffer[(dst + i) >> 2] = inputBuffer[(src + i) >> 2];
        }
    } else {
        uint mask = ELEMENT_BYTES == 1u ? 0xffu : 0xffffu;
        uint value = (inputBuffer[src >> 2] >> ((src & 3u) * 8u)) & mask;
        uint shift = (dst & 3u) * 8u;
        atomic_fetch_and_explicit(reinterpret_cast<device atomic_uint*>(outputBuffer) + (dst >> 2), ~(mask << shift), memory_order_relaxed);
        atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(outputBuffer) + (dst >> 2), value << shift, memory_order_relaxed);
    }
}

kernel void TextureDetile(device const uint* inputBuffer [[buffer(0)]], device uint* outputBuffer [[buffer(1)]], constant TextureDetileParameters& params [[buffer(2)]], uint2 position [[thread_position_in_grid]]) {
    uint2 p = position + uint2(params.columnBegin, params.rowBegin);
    if (p.x >= params.width || p.y >= params.height) {
        return;
    }

    uint linearOffset = p.y * params.pitchBytes + p.x * ELEMENT_BYTES;

    if (TILE_FAMILY == 0u) {
        if (linearOffset < params.rangeBegin || linearOffset >= params.rangeEnd) {
            return;
        }
        if (RETILE != 0u) {
            copyElement(params.srcBase + linearOffset - params.linearBase, params.dstBase + linearOffset - params.tiledBase, inputBuffer, outputBuffer);
        } else {
            copyElement(params.srcBase + linearOffset - params.tiledBase, params.dstBase + linearOffset - params.linearBase, inputBuffer, outputBuffer);
        }
        return;
    }

    uint2 swizzle = p;
    uint2 block = uint2(0u);
    if (params.tail != 0u) {
        swizzle += uint2(params.tailX, params.tailY);
    } else {
        block = p / blockExtent();
    }

    uint blockIndex = block.y * params.blocksPerRow + block.x;
    uint tiledOffset = blockIndex * BLOCK_BYTES + blockOffset(swizzle, params);
    if (tiledOffset < params.rangeBegin || tiledOffset >= params.rangeEnd) {
        return;
    }
    tiledOffset -= params.tiledBase;
    linearOffset -= params.linearBase;

    if (RETILE != 0u) {
        copyElement(params.srcBase + linearOffset, params.dstBase + tiledOffset, inputBuffer, outputBuffer);
    } else {
        copyElement(params.srcBase + tiledOffset, params.dstBase + linearOffset, inputBuffer, outputBuffer);
    }
}
