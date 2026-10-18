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
    uint pipeBankXor;
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
        return equationOffset(p, params) ^ params.pipeBankXor;
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
    if (ELEMENT_BYTES == 16u) {
        *reinterpret_cast<device packed_uint4*>(reinterpret_cast<device char*>(outputBuffer) + dst) =
            *reinterpret_cast<device const packed_uint4*>(reinterpret_cast<device const char*>(inputBuffer) + src);
    } else if (ELEMENT_BYTES == 8u) {
        *reinterpret_cast<device packed_uint2*>(reinterpret_cast<device char*>(outputBuffer) + dst) =
            *reinterpret_cast<device const packed_uint2*>(reinterpret_cast<device const char*>(inputBuffer) + src);
    } else if (ELEMENT_BYTES == 4u) {
        outputBuffer[dst >> 2] = inputBuffer[src >> 2];
    } else if (ELEMENT_BYTES > 16u) {
        uint i = 0u;
        for (; i + 16u <= ELEMENT_BYTES; i += 16u) {
            *reinterpret_cast<device packed_uint4*>(reinterpret_cast<device char*>(outputBuffer) + dst + i) =
                *reinterpret_cast<device const packed_uint4*>(reinterpret_cast<device const char*>(inputBuffer) + src + i);
        }
        for (; i < ELEMENT_BYTES; i += 4u) {
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

// The tiled and linear byte offsets of element `p` (relative to tiledBase and linearBase), or
// false when the element lies outside the copied range.
bool elementOffsets(uint2 p, constant TextureDetileParameters& params, thread uint& tiledOffset, thread uint& linearOffset) {
    linearOffset = p.y * params.pitchBytes + p.x * ELEMENT_BYTES;
    if (TILE_FAMILY == 0u) {
        if (linearOffset < params.rangeBegin || linearOffset >= params.rangeEnd) {
            return false;
        }
        tiledOffset = linearOffset - params.tiledBase;
        linearOffset -= params.linearBase;
        return true;
    }

    uint2 swizzle = p;
    uint2 block = uint2(0u);
    if (params.tail != 0u) {
        swizzle += uint2(params.tailX, params.tailY);
    } else {
        block = p / blockExtent();
    }

    uint blockIndex = block.y * params.blocksPerRow + block.x;
    tiledOffset = blockIndex * BLOCK_BYTES + blockOffset(swizzle, params);
    if (tiledOffset < params.rangeBegin || tiledOffset >= params.rangeEnd) {
        return false;
    }
    tiledOffset -= params.tiledBase;
    linearOffset -= params.linearBase;
    return true;
}

kernel void TextureDetile(device const uint* inputBuffer [[buffer(0)]], device uint* outputBuffer [[buffer(1)]], constant TextureDetileParameters& params [[buffer(2)]], uint2 position [[thread_position_in_grid]]) {
    uint2 p = position + uint2(params.columnBegin, params.rowBegin);
    if (p.x >= params.width || p.y >= params.height) {
        return;
    }
    uint tiledOffset;
    uint linearOffset;
    if (!elementOffsets(p, params, tiledOffset, linearOffset)) {
        return;
    }
    if (RETILE != 0u) {
        copyElement(params.srcBase + linearOffset, params.dstBase + tiledOffset, inputBuffer, outputBuffer);
    } else {
        copyElement(params.srcBase + tiledOffset, params.dstBase + linearOffset, inputBuffer, outputBuffer);
    }
}

// The fused paths: the texture itself is read or written through a view whose unsigned format
// has one component per 32 bits of the element (R8Uint and R16Uint for 1- and 2-byte elements),
// so the raw texel bits move without the linear staging buffer and the blit around the detile.
// `slice` stays the guest layer the swizzle equation XORs in; `textureSlice` and `level` address
// the view (a 2D array of every level and layer of the backing).
struct TextureTransferParameters {
    TextureDetileParameters tile;
    uint textureSlice;
    uint level;
};

uint4 readElement(uint byteOffset, device const uint* buffer) {
    if (ELEMENT_BYTES == 16u) {
        return uint4(*reinterpret_cast<device const packed_uint4*>(reinterpret_cast<device const char*>(buffer) + byteOffset));
    } else if (ELEMENT_BYTES == 8u) {
        auto val = *reinterpret_cast<device const packed_uint2*>(reinterpret_cast<device const char*>(buffer) + byteOffset);
        return uint4(val.x, val.y, 0u, 0u);
    } else if (ELEMENT_BYTES == 4u) {
        return uint4(buffer[byteOffset >> 2], 0u, 0u, 0u);
    } else if (ELEMENT_BYTES > 16u) {
        return uint4(*reinterpret_cast<device const packed_uint4*>(reinterpret_cast<device const char*>(buffer) + byteOffset));
    }
    uint mask = ELEMENT_BYTES == 1u ? 0xffu : 0xffffu;
    return uint4((buffer[byteOffset >> 2] >> ((byteOffset & 3u) * 8u)) & mask, 0u, 0u, 0u);
}

void writeElement(uint byteOffset, uint4 value, device uint* buffer) {
    if (ELEMENT_BYTES == 16u) {
        *reinterpret_cast<device packed_uint4*>(reinterpret_cast<device char*>(buffer) + byteOffset) = (packed_uint4)value;
        return;
    } else if (ELEMENT_BYTES == 8u) {
        *reinterpret_cast<device packed_uint2*>(reinterpret_cast<device char*>(buffer) + byteOffset) = (packed_uint2)value.xy;
        return;
    } else if (ELEMENT_BYTES == 4u) {
        buffer[byteOffset >> 2] = value.x;
        return;
    } else if (ELEMENT_BYTES > 16u) {
        *reinterpret_cast<device packed_uint4*>(reinterpret_cast<device char*>(buffer) + byteOffset) = (packed_uint4)value;
        return;
    }
    uint mask = ELEMENT_BYTES == 1u ? 0xffu : 0xffffu;
    uint shift = (byteOffset & 3u) * 8u;
    atomic_fetch_and_explicit(reinterpret_cast<device atomic_uint*>(buffer) + (byteOffset >> 2), ~(mask << shift), memory_order_relaxed);
    atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(buffer) + (byteOffset >> 2), (value.x & mask) << shift, memory_order_relaxed);
}

// Texture to guest-tiled bytes (a copy-back), in one pass.
kernel void TextureRetileFromTexture(texture2d_array<uint, access::read> source [[texture(0)]], device uint* outputBuffer [[buffer(0)]],
    constant TextureTransferParameters& params [[buffer(1)]], uint2 position [[thread_position_in_grid]]) {
    uint2 p = position + uint2(params.tile.columnBegin, params.tile.rowBegin);
    if (p.x >= params.tile.width || p.y >= params.tile.height) {
        return;
    }
    uint tiledOffset;
    uint linearOffset;
    if (!elementOffsets(p, params.tile, tiledOffset, linearOffset)) {
        return;
    }
    writeElement(params.tile.dstBase + tiledOffset, source.read(p, params.textureSlice, params.level), outputBuffer);
}

// Guest-tiled bytes to the texture (an upload), in one pass.
kernel void TextureDetileToTexture(device const uint* inputBuffer [[buffer(0)]], texture2d_array<uint, access::write> destination [[texture(0)]],
    constant TextureTransferParameters& params [[buffer(1)]], uint2 position [[thread_position_in_grid]]) {
    uint2 p = position + uint2(params.tile.columnBegin, params.tile.rowBegin);
    if (p.x >= params.tile.width || p.y >= params.tile.height) {
        return;
    }
    uint tiledOffset;
    uint linearOffset;
    if (!elementOffsets(p, params.tile, tiledOffset, linearOffset)) {
        return;
    }
    destination.write(readElement(params.tile.srcBase + tiledOffset, inputBuffer), p, params.textureSlice, params.level);
}

struct TiledSamplingParameters {
    TextureDetileParameters tile;
    uint filterMode;
};

inline float4 UnpackRgba8(uint value) {
    return float4(
        float(value & 0xffu) * (1.0f / 255.0f),
        float((value >> 8u) & 0xffu) * (1.0f / 255.0f),
        float((value >> 16u) & 0xffu) * (1.0f / 255.0f),
        float((value >> 24u) & 0xffu) * (1.0f / 255.0f)
    );
}

inline float4 SampleTiledRgba8Point(device const uint* data, uint2 p, constant TextureDetileParameters& params) {
    if (p.x >= params.width || p.y >= params.height) {
        return float4(0.0f);
    }
    uint tiledOffset = 0u;
    uint linearOffset = 0u;
    if (!elementOffsets(p, params, tiledOffset, linearOffset)) {
        return float4(0.0f);
    }
    uint raw = data[(params.srcBase + tiledOffset) >> 2];
    return UnpackRgba8(raw);
}

inline float4 SampleTiledRgba8Bilinear(device const uint* data, float2 uv, constant TextureDetileParameters& params) {
    float u = uv.x * float(params.width) - 0.5f;
    float v = uv.y * float(params.height) - 0.5f;
    int x0 = clamp(int(floor(u)), 0, int(params.width - 1u));
    int y0 = clamp(int(floor(v)), 0, int(params.height - 1u));
    int x1 = clamp(x0 + 1, 0, int(params.width - 1u));
    int y1 = clamp(y0 + 1, 0, int(params.height - 1u));
    float fx = fract(u);
    float fy = fract(v);
    float4 c00 = SampleTiledRgba8Point(data, uint2(x0, y0), params);
    float4 c10 = SampleTiledRgba8Point(data, uint2(x1, y0), params);
    float4 c01 = SampleTiledRgba8Point(data, uint2(x0, y1), params);
    float4 c11 = SampleTiledRgba8Point(data, uint2(x1, y1), params);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

inline float4 DecodeBc1Block(device const uint* blockPtr, uint2 intraCoord) {
    uint word0 = blockPtr[0];
    uint word1 = blockPtr[1];
    ushort c0 = static_cast<ushort>(word0 & 0xffffu);
    ushort c1 = static_cast<ushort>((word0 >> 16u) & 0xffffu);
    float3 rgb0 = float3(float((c0 >> 11) & 0x1f), float((c0 >> 5) & 0x3f), float(c0 & 0x1f)) * float3(1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f);
    float3 rgb1 = float3(float((c1 >> 11) & 0x1f), float((c1 >> 5) & 0x3f), float(c1 & 0x1f)) * float3(1.0f / 31.0f, 1.0f / 63.0f, 1.0f / 31.0f);
    uint bitIndex = (intraCoord.y * 4u + intraCoord.x) * 2u;
    uint code = (word1 >> bitIndex) & 3u;
    float3 finalRgb = (code == 0u) ? rgb0 :
                      (code == 1u) ? rgb1 :
                      (c0 > c1) ? ((code == 2u) ? mix(rgb0, rgb1, 1.0f / 3.0f) : mix(rgb0, rgb1, 2.0f / 3.0f)) :
                                  ((code == 2u) ? mix(rgb0, rgb1, 0.5f) : float3(0.0f));
    float finalAlpha = (c0 <= c1 && code == 3u) ? 0.0f : 1.0f;
    return float4(finalRgb, finalAlpha);
}

inline float4 SampleTiledBc1Point(device const uint* data, uint2 p, constant TextureDetileParameters& params) {
    if (p.x >= params.width || p.y >= params.height) {
        return float4(0.0f);
    }
    uint2 blockCoord = p / 4u;
    uint2 intraCoord = p % 4u;
    uint tiledOffset = 0u;
    uint linearOffset = 0u;
    if (!elementOffsets(blockCoord, params, tiledOffset, linearOffset)) {
        return float4(0.0f);
    }
    device const uint* blockPtr = data + ((params.srcBase + tiledOffset) >> 2);
    return DecodeBc1Block(blockPtr, intraCoord);
}

inline float4 SampleTiledBc1Bilinear(device const uint* data, float2 uv, constant TextureDetileParameters& params) {
    float u = uv.x * float(params.width) - 0.5f;
    float v = uv.y * float(params.height) - 0.5f;
    int x0 = clamp(int(floor(u)), 0, int(params.width - 1u));
    int y0 = clamp(int(floor(v)), 0, int(params.height - 1u));
    int x1 = clamp(x0 + 1, 0, int(params.width - 1u));
    int y1 = clamp(y0 + 1, 0, int(params.height - 1u));
    float fx = fract(u);
    float fy = fract(v);
    float4 c00 = SampleTiledBc1Point(data, uint2(x0, y0), params);
    float4 c10 = SampleTiledBc1Point(data, uint2(x1, y0), params);
    float4 c01 = SampleTiledBc1Point(data, uint2(x0, y1), params);
    float4 c11 = SampleTiledBc1Point(data, uint2(x1, y1), params);
    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);
}

inline float DecodeBc4Block(device const uint* blockPtr, uint2 intraCoord) {
    uint word0 = blockPtr[0];
    uint word1 = blockPtr[1];
    uint r0 = word0 & 0xffu;
    uint r1 = (word0 >> 8u) & 0xffu;
    ulong indices = (static_cast<ulong>(word1) << 16u) | (static_cast<ulong>(word0 >> 16u));
    uint bitIndex = (intraCoord.y * 4u + intraCoord.x) * 3u;
    uint code = static_cast<uint>((indices >> bitIndex) & 7u);
    if (r0 > r1) {
        if (code == 0u) return float(r0) * (1.0f / 255.0f);
        if (code == 1u) return float(r1) * (1.0f / 255.0f);
        return float(((8u - code) * r0 + (code - 1u) * r1) / 7u) * (1.0f / 255.0f);
    } else {
        if (code == 0u) return float(r0) * (1.0f / 255.0f);
        if (code == 1u) return float(r1) * (1.0f / 255.0f);
        if (code == 6u) return 0.0f;
        if (code == 7u) return 1.0f;
        return float(((6u - code) * r0 + (code - 1u) * r1) / 5u) * (1.0f / 255.0f);
    }
}

inline float4 SampleTiledBc4Point(device const uint* data, uint2 p, constant TextureDetileParameters& params) {
    if (p.x >= params.width || p.y >= params.height) {
        return float4(0.0f);
    }
    uint2 blockCoord = p / 4u;
    uint2 intraCoord = p % 4u;
    uint tiledOffset = 0u;
    uint linearOffset = 0u;
    if (!elementOffsets(blockCoord, params, tiledOffset, linearOffset)) {
        return float4(0.0f);
    }
    device const uint* blockPtr = data + ((params.srcBase + tiledOffset) >> 2);
    float red = DecodeBc4Block(blockPtr, intraCoord);
    return float4(red, 0.0f, 0.0f, 1.0f);
}

kernel void InShaderSampleTiledTexture(device const uint* inputBuffer [[buffer(0)]],
                                     device float4* outputBuffer [[buffer(1)]],
                                     constant TiledSamplingParameters& params [[buffer(2)]],
                                     uint2 position [[thread_position_in_grid]]) {
    if (position.x >= params.tile.width || position.y >= params.tile.height) {
        return;
    }
    uint outputIndex = position.y * params.tile.width + position.x;
    float4 result = float4(0.0f);
    if (params.filterMode == 0u) {
        result = SampleTiledRgba8Point(inputBuffer, position, params.tile);
    } else if (params.filterMode == 1u) {
        float2 uv = (float2(position) + 0.5f) / float2(params.tile.width, params.tile.height);
        result = SampleTiledRgba8Bilinear(inputBuffer, uv, params.tile);
    } else if (params.filterMode == 2u) {
        result = SampleTiledBc1Point(inputBuffer, position, params.tile);
    } else if (params.filterMode == 3u) {
        float2 uv = (float2(position) + 0.5f) / float2(params.tile.width, params.tile.height);
        result = SampleTiledBc1Bilinear(inputBuffer, uv, params.tile);
    } else if (params.filterMode == 4u) {
        result = SampleTiledBc4Point(inputBuffer, position, params.tile);
    }
    outputBuffer[outputIndex] = result;
}

