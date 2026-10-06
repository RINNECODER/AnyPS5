#include "MetalTestSupport.hpp"
#include "MetalTexture.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <string_view>

namespace {
using MetalTests::Require;
using AgcDriver::Graphics::GuestTextureResource;
using AgcDriver::Graphics::TextureDimension;
using AgcDriver::Graphics::TextureTileMode;
using AgcDriver::Metal::MetalTexture;

constexpr const char* source = R"(
#include <metal_stdlib>
using namespace metal;
uchar4 bytes(float4 pixel) { return uchar4(round(pixel * 255.0f)); }
kernel void read2D(texture2d<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { out[i] = bytes(t.read(uint2(i % shape.x, i / shape.x), shape.w)); }
kernel void readArray(texture2d_array<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { uint plane = shape.x * shape.y; out[i] = bytes(t.read(uint2(i % shape.x, (i % plane) / shape.x), i / plane, shape.w)); }
kernel void read3D(texture3d<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { uint plane = shape.x * shape.y; out[i] = bytes(t.read(uint3(i % shape.x, (i % plane) / shape.x, i / plane), shape.w)); }
kernel void sampleView(texture2d<float> t [[texture(0)]], device uchar4* out [[buffer(0)]], uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest); out[i] = bytes(t.sample(s, float2(0.5f), level(0))); }
kernel void sample2D(texture2d<float> t [[texture(0)]], device float4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized, filter::nearest, mip_filter::nearest); out[i] = t.sample(s, (float2(i % shape.x, i / shape.x) + 0.5f) / float2(shape.xy), level(shape.w)); }
kernel void sampleArray(texture2d_array<float> t [[texture(0)]], device float4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized, filter::nearest, mip_filter::nearest); uint plane = shape.x * shape.y; out[i] = t.sample(s, (float2(i % shape.x, (i % plane) / shape.x) + 0.5f) / float2(shape.xy), i / plane, level(shape.w)); }
kernel void writeArray(texture2d_array<float, access::write> t [[texture(0)]], constant uint4& shape [[buffer(0)]], uint3 p [[thread_position_in_grid]]) { t.write(float4((17 + p.x * 3 + p.y * 7 + p.z * 11 + shape.w * 13) & 255, (31 + p.x * 5 + p.z * 17) & 255, (73 + p.y * 9 + shape.w * 23) & 255, 127) / 255.0f, p.xy, p.z, shape.w); }
kernel void writeView(texture2d<float, access::write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(float4(17,31,73,127) / 255.0f, p); }
)";

std::array<std::byte, 4> Pixel(unsigned x, unsigned y, unsigned layer, unsigned level) {
    return {std::byte((x * 7 + y * 3 + layer * 43 + level * 19) & 255),
            std::byte((x * 5 + y * 11 + layer * 13 + level * 37 + 1) & 255),
            std::byte((x + y * 17 + layer * 29 + level * 23 + 2) & 255), std::byte(255)};
}

unsigned XorAddress(TextureTileMode mode, unsigned x, unsigned y, unsigned layer) {
    const auto bit = [](unsigned value, unsigned shift) { return (value >> shift) & 1u; };
    unsigned address = 0;
    if (mode == TextureTileMode::kZ64KBX) {
        address = (bit(x,0) << 2) | (bit(y,0) << 3) | (bit(x,1) << 4) | (bit(y,1) << 5) | (bit(x,2) << 6) | (bit(y,2) << 7);
    } else {
        address = (bit(x,0) << 2) | (bit(x,1) << 3) | (bit(y,0) << 4) | (bit(y,1) << 5) | (bit(y,2) << 6) | (bit(x,2) << 7);
    }
    if (mode == TextureTileMode::kS64KBX || mode == TextureTileMode::kD64KBX) {
        address |= (bit(layer,3) ^ bit(y,3) ^ bit(x,6)) << 8;
        address |= (bit(layer,2) ^ bit(y,6) ^ bit(x,3)) << 9;
        address |= (bit(layer,1) ^ bit(y,4) ^ bit(x,5)) << 10;
        address |= (bit(layer,0) ^ bit(y,5) ^ bit(x,4)) << 11;
        address |= (bit(y,5) << 12) | (bit(x,5) << 13) | (bit(y,6) << 14) | (bit(x,6) << 15);
    } else {
        address |= (bit(layer,3) ^ bit(y,3) ^ bit(x,3)) << 8;
        address |= (bit(layer,2) ^ bit(y,4) ^ bit(x,4)) << 9;
        address |= (bit(layer,1) ^ bit(y,5) ^ bit(x,6)) << 10;
        address |= (bit(layer,0) ^ bit(y,6) ^ bit(x,5)) << 11;
        address |= (bit(y,3) << 12) | (bit(x,4) << 13) | (bit(y,6) << 14) | (bit(x,6) << 15);
    }
    return address;
}

std::uint64_t Address(const MetalTexture& resource, unsigned x, unsigned y, unsigned layer, unsigned level) {
    const auto& g = resource.Geometry();
    const auto& mip = g.mips[level];
    if (resource.Descriptor().tileMode == TextureTileMode::kLinear) return g.GuestLayerOffset(layer) + mip.tiledOffset + y * mip.pitchBytes + x * 4;
    Require(!g.thick, "thin texture address fixture received a thick surface");
    if (AgcDriver::Graphics::XorSwizzleMode(resource.Descriptor().tileMode) != 0) {
        const unsigned sx = x + (mip.tail ? mip.tailX : 0);
        const unsigned sy = y + (mip.tail ? mip.tailY : 0);
        const unsigned block = mip.tail ? 0 : (y / 128) * mip.blocksPerRow + x / 128;
        return g.GuestLayerOffset(layer) + mip.tiledOffset + block * 65536u + XorAddress(resource.Descriptor().tileMode,sx,sy,layer);
    }
    const auto block = AgcDriver::Graphics::ThinBlockLayout(resource.Descriptor().tileMode, 4);
    const unsigned sx = x + (mip.tail ? mip.tailX : 0);
    const unsigned sy = y + (mip.tail ? mip.tailY : 0);
    constexpr std::array<int, 16> bits{-1,-1,0,1,12,13,14,2,15,3,16,4,17,5,18,6};
    unsigned within = 0;
    for (unsigned bit = 2; (1u << bit) < block[0]; ++bit) {
        const unsigned coordinate = bits[bit] < 12 ? sx : sy;
        const unsigned index = bits[bit] < 12 ? bits[bit] : bits[bit] - 12;
        within |= ((coordinate >> index) & 1u) << bit;
    }
    const unsigned blockIndex = mip.tail ? 0 : (y / block[2]) * mip.blocksPerRow + x / block[1];
    return g.GuestLayerOffset(layer) + mip.tiledOffset + blockIndex * block[0] + within;
}

GuestTextureResource Descriptor(TextureDimension dimension, TextureTileMode mode = TextureTileMode::kLinear) {
    GuestTextureResource d{};
    d.width = 8;
    d.height = dimension == TextureDimension::k1D ? 1 : 8;
    d.depthOrLastArray = dimension == TextureDimension::k3D ? 3 : dimension == TextureDimension::k2DArray ? 2 : dimension == TextureDimension::kCube ? 5 : 0;
    d.mipCount = d.allocatedMipCount = 4;
    d.lastLevel = d.mipCount - 1;
    d.dimension = dimension;
    d.tileMode = mode;
    d.format = 56;
    d.dstSelX = 4;
    d.dstSelY = 5;
    d.dstSelZ = 6;
    d.dstSelW = 7;
    return d;
}

id<MTLComputePipelineState> Pipeline(const MetalTests::Context& context, id<MTLLibrary> library, NSString* name) {
    NSError* error = nil;
    auto pipeline = [context.device newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];
    Require(pipeline != nil, error.localizedDescription.UTF8String ?: "texture probe pipeline creation failed");
    return pipeline;
}

id<MTLBuffer> Probe(const MetalTests::Context& context, id<MTLLibrary> library, id<MTLTexture> texture, TextureDimension dimension, unsigned level) {
    const unsigned width = std::max<unsigned>(texture.width >> level, 1);
    const unsigned height = std::max<unsigned>(texture.height >> level, 1);
    const unsigned layers = dimension == TextureDimension::k3D ? std::max<unsigned>(texture.depth >> level, 1) : texture.arrayLength;
    const unsigned count = width * height * layers;
    auto output = context.Buffer(count * 4, 0xa7);
    NSString* name = dimension == TextureDimension::k3D ? @"read3D" : (dimension == TextureDimension::k1D || dimension == TextureDimension::k2D) ? @"read2D" : @"readArray";
    auto commands = [context.queue commandBuffer];
    auto encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:Pipeline(context, library, name)];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    const std::array<std::uint32_t, 4> shape{width,height,layers,level};
    [encoder setBytes:shape.data() length:sizeof(shape) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(std::min(count,64u),1,1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted, "native texture GPU probe failed");
    return output;
}

void VerifySurface(const MetalTests::Context& context, id<MTLLibrary> probes, GuestTextureResource descriptor, bool mutate = false) {
    AgcDriver::Metal::MetalDevice backend(context.device, context.library);
    MetalTexture texture(backend, descriptor);
    std::vector<std::byte> guest(texture.GuestBytes(), std::byte{0xa7});
    const auto& g = texture.Geometry();
    for (unsigned level = 0; level < descriptor.mipCount; ++level) {
        const auto& mip = g.mips[level];
        for (unsigned layer = 0; layer < g.layers; ++layer) {
            if (!g.HasLayer(level, layer)) continue;
            for (unsigned y = 0; y < mip.height; ++y) for (unsigned x = 0; x < mip.width; ++x) {
                const auto pixel = Pixel(x,y,layer,level);
                std::memcpy(guest.data() + Address(texture,x,y,layer,level), pixel.data(), 4);
            }
        }
    }
    texture.Upload(guest);
    for (unsigned level = 0; level < descriptor.mipCount; ++level) {
        const auto& mip = g.mips[level];
        auto output = Probe(context,probes,texture.Texture(),descriptor.dimension,level);
        const auto* bytes = static_cast<const std::byte*>(output.contents);
        const unsigned layers = descriptor.dimension == TextureDimension::k3D ? std::max(g.layers >> level,1u) : g.layers;
        for (unsigned layer = 0; layer < layers; ++layer) for (unsigned y = 0; y < mip.height; ++y) for (unsigned x = 0; x < mip.width; ++x) {
            const auto expected = Pixel(x,y,layer,level);
            Require(std::memcmp(bytes + ((layer * mip.height + y) * mip.width + x) * 4, expected.data(), 4) == 0, "native texture mip or slice pixels differ from independent fixture");
        }
    }
    if (mutate) {
        auto commands = [context.queue commandBuffer];
        for (unsigned level=0; level<descriptor.mipCount; ++level) {
            const auto& mip = g.mips[level];
            auto encoder = [commands computeCommandEncoder];
            [encoder setComputePipelineState:Pipeline(context,probes,@"writeArray")];
            [encoder setTexture:texture.Texture() atIndex:0];
            const std::array<std::uint32_t,4> shape{mip.width,mip.height,g.layers,level};
            [encoder setBytes:shape.data() length:sizeof(shape) atIndex:0];
            [encoder dispatchThreads:MTLSizeMake(mip.width,mip.height,g.layers) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            [encoder endEncoding];
            for (unsigned layer=0; layer<g.layers; ++layer) for (unsigned y=0; y<mip.height; ++y) for (unsigned x=0; x<mip.width; ++x) {
                const std::array<std::byte,4> expected{std::byte((17 + x*3 + y*7 + layer*11 + level*13) & 255),std::byte((31 + x*5 + layer*17) & 255),std::byte((73 + y*9 + level*23) & 255),std::byte{127}};
                std::memcpy(guest.data()+Address(texture,x,y,layer,level),expected.data(),4);
            }
        }
        [commands commit];
        [commands waitUntilCompleted];
        Require(commands.status == MTLCommandBufferStatusCompleted,"XOR guest texture native mutation command failed");
    }
    auto readback = std::vector<std::byte>(texture.GuestBytes(),std::byte{0xa7});
    texture.Readback(readback);
    Require(readback == guest,"native texture readback changes guest pixels, slice or shared mip tails, or padding");
}

void VerifyViews(const MetalTests::Context& context, id<MTLLibrary> probes) {
    auto descriptor = Descriptor(TextureDimension::k2D);
    descriptor.baseLevel = 1;
    descriptor.lastLevel = 2;
    descriptor.dstSelX = 6;
    descriptor.dstSelY = 4;
    descriptor.dstSelZ = 1;
    descriptor.dstSelW = 0;
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    MetalTexture texture(backend,descriptor);
    std::vector<std::byte> guest(texture.GuestBytes(),std::byte{0xa7});
    texture.Upload(guest);
    auto commands = [context.queue commandBuffer];
    auto write = [commands computeCommandEncoder];
    [write setComputePipelineState:Pipeline(context,probes,@"writeView")];
    [write setTexture:texture.StorageView() atIndex:0];
    [write dispatchThreads:MTLSizeMake(4,4,1) threadsPerThreadgroup:MTLSizeMake(4,4,1)];
    [write endEncoding];
    auto output = context.Buffer(4);
    auto sample = [commands computeCommandEncoder];
    [sample setComputePipelineState:Pipeline(context,probes,@"sampleView")];
    [sample setTexture:texture.SampledView() atIndex:0];
    [sample setBuffer:output offset:0 atIndex:0];
    [sample dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [sample endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted,"native texture storage/sample probe failed");
    const std::array<std::uint8_t,4> sampled{73,17,255,0};
    Require(std::memcmp(output.contents,sampled.data(),4) == 0,"sampled texture view loses mip selection or component swizzle");
    texture.Readback(guest);
    std::vector<std::byte> expected(texture.GuestBytes(),std::byte{0xa7});
    const std::array<std::byte,4> stored{std::byte{17},std::byte{31},std::byte{73},std::byte{127}};
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) std::memcpy(expected.data()+Address(texture,x,y,0,1),stored.data(),4);
    Require(guest == expected,"storage view readback changes another mip or applies sampled swizzle to writes");
}

void VerifyArrayView(const MetalTests::Context& context, id<MTLLibrary> probes) {
    auto descriptor = Descriptor(TextureDimension::k2DArray);
    descriptor.baseArray = 1;
    descriptor.baseLevel = 1;
    descriptor.lastLevel = 2;
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    MetalTexture texture(backend,descriptor);
    std::vector<std::byte> guest(texture.GuestBytes(),std::byte{0xa7});
    for (unsigned layer = 0; layer < 3; ++layer) for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
        const auto pixel = Pixel(x,y,layer,1);
        std::memcpy(guest.data()+Address(texture,x,y,layer,1),pixel.data(),4);
    }
    texture.Upload(guest);
    auto output = Probe(context,probes,texture.SampledView(),TextureDimension::k2DArray,0);
    const auto* bytes = static_cast<const std::byte*>(output.contents);
    for (unsigned layer = 0; layer < 2; ++layer) for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
        const auto pixel = Pixel(x,y,layer+1,1);
        Require(std::memcmp(bytes+((layer*4+y)*4+x)*4,pixel.data(),4)==0,"native array view does not rebase mip and slice indices");
    }
}

void VerifyDecodedDescriptor(const MetalTests::Context& context, id<MTLLibrary> probes) {
    const std::array<std::uint32_t,8> words{1u,(56u<<20)|(3u<<30),1u|(7u<<14),4u|(5u<<3)|(6u<<6)|(7u<<9)|(3u<<16)|(9u<<28),0u,3u<<4,0u,0u};
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    MetalTexture texture(backend,words);
    std::vector<std::byte> guest(texture.GuestBytes(),std::byte{0xa7});
    const auto pixel=Pixel(1,2,0,0);
    std::memcpy(guest.data()+Address(texture,1,2,0,0),pixel.data(),4);
    texture.Upload(guest);
    auto output=Probe(context,probes,texture.Texture(),TextureDimension::k2D,0);
    Require(std::memcmp(static_cast<const std::byte*>(output.contents)+(2*8+1)*4,pixel.data(),4)==0,"decoded guest descriptor does not produce native texture pixels");
    bool rejected=false;
    try { texture.Upload(std::span<const std::byte>(guest).first(guest.size()-1)); } catch (const std::runtime_error& error) {
        rejected=std::string_view(error.what()).find("byte extent")!=std::string_view::npos;
    }
    Require(rejected,"native texture upload must reject a truncated guest allocation");
    rejected=false;
    try { texture.Readback(std::span<std::byte>(guest).first(guest.size()-1)); } catch (const std::runtime_error& error) {
        rejected=std::string_view(error.what()).find("byte extent")!=std::string_view::npos;
    }
    Require(rejected,"native texture readback must reject a truncated guest allocation");
}

void VerifyThick(const MetalTests::Context& context, id<MTLLibrary> probes) {
    auto descriptor = Descriptor(TextureDimension::k3D,TextureTileMode::kStandard4KB);
    descriptor.depthOrLastArray = 7;
    descriptor.mipCount = descriptor.allocatedMipCount = 2;
    descriptor.lastLevel = 1;
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    MetalTexture texture(backend,descriptor);
    std::vector<std::byte> guest(texture.GuestBytes(),std::byte{0});
    struct Point { unsigned level,x,y,z,address; };
    constexpr Point points[]{{0,0,0,0,0x800},{0,7,7,7,0xffc},{0,4,2,4,0xe20},{0,2,7,0,0x968},{0,7,0,7,0xed4},{1,0,0,0,0x300},{1,3,3,3,0x3fc},{1,2,1,2,0x3c8},{1,1,3,0,0x32c},{1,3,0,3,0x3d4}};
    for (const auto& point : points) {
        const auto pixel=Pixel(point.x,point.y,point.z,point.level);
        std::memcpy(guest.data()+point.address,pixel.data(),4);
    }
    texture.Upload(guest);
    for (unsigned level=0;level<2;++level) {
        auto output=Probe(context,probes,texture.Texture(),TextureDimension::k3D,level);
        const unsigned width=8>>level;
        for (const auto& point: points) {
            if (point.level!=level) continue;
            const auto pixel=Pixel(point.x,point.y,point.z,point.level);
            const auto* actual=static_cast<const std::byte*>(output.contents)+((point.z*width+point.y)*width+point.x)*4;
            Require(std::memcmp(actual,pixel.data(),4)==0,"thick 3D native texture loses independent golden address pixels");
        }
    }
    auto readback=std::vector<std::byte>(texture.GuestBytes(),std::byte{0});
    texture.Readback(readback);
    Require(readback==guest,"thick 3D texture readback changes guest pixel locations");
}

float Srgb(float value) {
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f,2.4f);
}

struct CompressedBlock {
    std::array<std::uint8_t,16> bytes{};
    std::array<float,4> color{0,0,0,1};
};

CompressedBlock EncodeBlock(unsigned format, unsigned key) {
    key &= 3u;
    CompressedBlock block;
    constexpr std::array<std::uint16_t,4> endpoints{0x8410,0xf800,0x07e0,0x001f};
    const auto endpoint = endpoints[key];
    const auto rgb = std::array<float,3>{float((endpoint >> 11) & 31u)/31.0f,float((endpoint >> 5) & 63u)/63.0f,float(endpoint & 31u)/31.0f};
    if (format <= 174) {
        const unsigned offset = format <= 170 ? 0 : 8;
        block.bytes[offset] = endpoint & 255u;
        block.bytes[offset+1] = endpoint >> 8;
        std::copy(rgb.begin(),rgb.end(),block.color.begin());
        if (format == 171 || format == 172) {
            const auto alpha = std::uint8_t(3u + 3u * key);
            std::fill_n(block.bytes.begin(),8,std::uint8_t(alpha | (alpha << 4)));
            block.color[3] = float(alpha) / 15.0f;
        }
        if (format == 173 || format == 174) {
            block.bytes[0] = 64u + 48u * key;
            block.color[3] = float(block.bytes[0]) / 255.0f;
        }
    } else if (format <= 178) {
        if (format == 175 || format == 177) {
            block.bytes[0] = 64u + 32u * key;
            block.color[0] = float(block.bytes[0]) / 255.0f;
            if (format == 177) {
                block.bytes[8] = 192u - 32u * key;
                block.color[1] = float(block.bytes[8]) / 255.0f;
            }
        } else {
            const int red = -64 + 32 * int(key);
            block.bytes[0] = std::uint8_t(red);
            block.color[0] = float(red) / 127.0f;
            if (format == 178) {
                const int green = 64 - 32 * int(key);
                block.bytes[8] = std::uint8_t(green);
                block.color[1] = float(green) / 127.0f;
            }
        }
    } else if (format >= 181) {
        constexpr std::array<std::array<unsigned,4>,4> colors{{{127,0,0,127},{64,127,0,127},{32,64,127,127},{96,32,64,127}}};
        unsigned bit = 0;
        auto put = [&](unsigned value, unsigned count) {
            for (unsigned index=0; index<count; ++index,++bit) block.bytes[bit/8] |= ((value >> index) & 1u) << (bit % 8);
        };
        put(64,7);
        for (const auto channel : colors[key]) { put(channel,7); put(channel,7); }
        put(0,1);
        put(0,1);
        put(0,3);
        for (unsigned index=1; index<16; ++index) put(0,4);
        Require(bit == 128,"BC7 independent block encoding has the wrong extent");
        for (unsigned channel=0; channel<4; ++channel) block.color[channel] = float(2u * colors[key][channel]) / 255.0f;
    }
    if (format == 170 || format == 172 || format == 174 || format == 182) {
        for (unsigned channel=0; channel<3; ++channel) block.color[channel] = Srgb(block.color[channel]);
    }
    return block;
}

id<MTLBuffer> SamplePixels(const MetalTests::Context& context, id<MTLLibrary> probes, id<MTLTexture> texture, unsigned level) {
    const unsigned width = std::max<unsigned>(texture.width >> level,1);
    const unsigned height = std::max<unsigned>(texture.height >> level,1);
    const unsigned layers = texture.arrayLength;
    const unsigned count = width * height * layers;
    auto output = context.Buffer((count + 1) * 16,0xa7);
    auto commands = [context.queue commandBuffer];
    auto encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:Pipeline(context,probes,texture.textureType == MTLTextureType2DArray ? @"sampleArray" : @"sample2D")];
    [encoder setTexture:texture atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:0];
    const std::array<std::uint32_t,4> shape{width,height,layers,level};
    [encoder setBytes:shape.data() length:sizeof(shape) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(count,1,1) threadsPerThreadgroup:MTLSizeMake(std::min(count,64u),1,1)];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    Require(commands.status == MTLCommandBufferStatusCompleted,"guest texture sampling command failed");
    const auto* bytes = static_cast<const std::uint8_t*>(output.contents);
    Require(std::all_of(bytes + count * 16,bytes + output.length,[](std::uint8_t value) { return value == 0xa7; }),"guest texture sampling overwrites its output guard");
    return output;
}

void RequireColor(const float* actual, const std::array<float,4>& expected, const char* message) {
    for (unsigned channel=0; channel<4; ++channel) Require(std::isfinite(actual[channel]) && std::abs(actual[channel] - expected[channel]) < 0.004f,message);
}

void RequireReadOnlyFormat(const MetalTexture& texture) {
    bool rejected = false;
    try { static_cast<void>(texture.StorageView()); } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("storage or render writes") != std::string_view::npos;
    }
    Require(rejected,"sample-only guest format must reject storage writes before encoding");
}

void VerifyCompressed(const MetalTests::Context& context, id<MTLLibrary> probes, unsigned format, TextureDimension dimension = TextureDimension::k2DArray) {
    auto descriptor = Descriptor(dimension);
    descriptor.format = format;
    descriptor.width = 7;
    descriptor.height = dimension == TextureDimension::k1D ? 1 : 5;
    descriptor.mipCount = descriptor.allocatedMipCount = 3;
    descriptor.lastLevel = 2;
    descriptor.baseLevel = 1;
    descriptor.baseArray = dimension == TextureDimension::k1D ? 0 : 1;
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    MetalTexture texture(backend,descriptor);
    const auto& geometry = texture.Geometry();
    const unsigned elementBytes = format == 169 || format == 170 || format == 175 || format == 176 ? 8 : 16;
    std::vector<std::byte> guest(texture.GuestBytes(),std::byte{0xa7});
    for (unsigned level=0; level<3; ++level) {
        const auto& mip = geometry.mips[level];
        for (unsigned layer=0; layer<geometry.layers; ++layer) for (unsigned y=0; y<mip.height; ++y) for (unsigned x=0; x<mip.width; ++x) {
            const auto block = EncodeBlock(format,x + y + layer + 3u * level);
            const auto address = geometry.GuestLayerOffset(layer) + mip.tiledOffset + y * mip.pitchBytes + x * elementBytes;
            std::memcpy(guest.data() + address,block.bytes.data(),elementBytes);
        }
    }
    texture.Upload(guest);
    for (unsigned view=0; view<2; ++view) {
        auto native = view ? texture.SampledView() : texture.Texture();
        const unsigned baseLevel = view ? 1 : 0;
        const unsigned baseArray = view ? descriptor.baseArray : 0;
        for (unsigned level=0; level<native.mipmapLevelCount; ++level) {
            const unsigned width = std::max<unsigned>(native.width >> level,1);
            const unsigned height = std::max<unsigned>(native.height >> level,1);
            auto output = SamplePixels(context,probes,native,level);
            const auto* pixels = static_cast<const float*>(output.contents);
            for (unsigned layer=0; layer<native.arrayLength; ++layer) for (unsigned y=0; y<height; ++y) for (unsigned x=0; x<width; ++x) {
                const auto expected = EncodeBlock(format,x/4 + y/4 + layer + baseArray + 3u * (level + baseLevel));
                RequireColor(pixels + ((layer * height + y) * width + x) * 4,expected.color,"guest compressed texture mip, slice, format, or edge-block sampled color differs");
            }
        }
    }
    auto readback = std::vector<std::byte>(texture.GuestBytes(),std::byte{0xa7});
    texture.Readback(readback);
    Require(readback == guest,"compressed texture writeback changes encoded blocks or untouched padding");
    RequireReadOnlyFormat(texture);
}

void VerifyPacked(const MetalTests::Context& context, id<MTLLibrary> probes) {
    struct Case { unsigned format; std::array<std::uint16_t,3> pixels; };
    constexpr std::array<Case,3> cases{{{133,{0xf800,0x07e0,0x001f}},{134,{0xfc00,0x83e0,0x801f}},{136,{0xf00f,0x0f0f,0x00ff}}}};
    constexpr std::array<std::array<float,4>,3> colors{{{1,0,0,1},{0,1,0,1},{0,0,1,1}}};
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    for (const auto& test : cases) {
        auto descriptor = Descriptor(TextureDimension::k2D);
        descriptor.width = 3;
        descriptor.height = 1;
        descriptor.format = test.format;
        descriptor.mipCount = descriptor.allocatedMipCount = 1;
        descriptor.lastLevel = 0;
        MetalTexture texture(backend,descriptor);
        std::vector<std::byte> guest(texture.GuestBytes(),std::byte{0xa7});
        std::memcpy(guest.data(),test.pixels.data(),sizeof(test.pixels));
        texture.Upload(guest);
        auto output = SamplePixels(context,probes,texture.SampledView(),0);
        const auto* pixels = static_cast<const float*>(output.contents);
        for (unsigned x=0; x<3; ++x) RequireColor(pixels + x * 4,colors[x],"packed guest texture red, green, blue, or alpha bit positions differ");
        auto readback = std::vector<std::byte>(texture.GuestBytes(),std::byte{0xa7});
        texture.Readback(readback);
        Require(readback == guest,"packed guest texture writeback changes encoded texels or padding");
        RequireReadOnlyFormat(texture);
    }
}

}

void RunTextureResourceTests(const MetalTests::Context& context) {
    NSError* error=nil;
    auto probes=[context.device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
    Require(probes!=nil,error.localizedDescription.UTF8String ?: "native texture probes do not compile");
    for (const auto dimension : {TextureDimension::k1D,TextureDimension::k2D,TextureDimension::k2DArray,TextureDimension::kCube,TextureDimension::k3D}) VerifySurface(context,probes,Descriptor(dimension));
    for (const auto mode : {TextureTileMode::kStandard256B,TextureTileMode::kStandard4KB,TextureTileMode::kStandard64KB}) {
        auto descriptor=Descriptor(TextureDimension::k2DArray,mode);
        descriptor.width=descriptor.height=mode==TextureTileMode::kStandard64KB ? 129 : 33;
        descriptor.mipCount=descriptor.allocatedMipCount=6;
        descriptor.lastLevel=5;
        VerifySurface(context,probes,descriptor);
    }
    for (const auto mode : {TextureTileMode::kZ64KBX,TextureTileMode::kS64KBX,TextureTileMode::kD64KBX,TextureTileMode::kR64KBX}) {
        auto descriptor = Descriptor(TextureDimension::k2DArray,mode);
        descriptor.width = 129;
        descriptor.height = 133;
        descriptor.depthOrLastArray = 8;
        descriptor.mipCount = descriptor.allocatedMipCount = 8;
        descriptor.lastLevel = 7;
        VerifySurface(context,probes,descriptor,true);
    }
    VerifyViews(context,probes);
    VerifyArrayView(context,probes);
    VerifyDecodedDescriptor(context,probes);
    VerifyThick(context,probes);
    for (unsigned format=169; format<=182; ++format) VerifyCompressed(context,probes,format);
    VerifyCompressed(context,probes,169,TextureDimension::k1D);
    VerifyPacked(context,probes);
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    auto unsupported = Descriptor(TextureDimension::k2D);
    unsupported.minLod = 256;
    bool rejected = false;
    try { MetalTexture texture(backend,unsupported); } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("minimum LOD") != std::string_view::npos;
    }
    Require(rejected,"native texture must reject a minimum LOD that the device cannot represent");
    unsupported = Descriptor(TextureDimension::k2D,TextureTileMode::kR64KBX);
    unsupported.baseAddress = 256;
    rejected = false;
    try { MetalTexture texture(backend,unsupported); } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("pipe or bank XOR") != std::string_view::npos;
    }
    Require(rejected,"native XOR texture must reject unimplemented address pipe or bank bits");
}
