#include "MetalTestSupport.hpp"
#include "MetalTexture.hpp"
#include <array>
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
kernel void read1D(texture1d<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { out[i] = bytes(t.read(i)); }
kernel void read2D(texture2d<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { out[i] = bytes(t.read(uint2(i % shape.x, i / shape.x), shape.w)); }
kernel void readArray(texture2d_array<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { uint plane = shape.x * shape.y; out[i] = bytes(t.read(uint2(i % shape.x, (i % plane) / shape.x), i / plane, shape.w)); }
kernel void read3D(texture3d<float, access::read> t [[texture(0)]], device uchar4* out [[buffer(0)]], constant uint4& shape [[buffer(1)]], uint i [[thread_position_in_grid]]) { uint plane = shape.x * shape.y; out[i] = bytes(t.read(uint3(i % shape.x, (i % plane) / shape.x, i / plane), shape.w)); }
kernel void sampleView(texture2d<float> t [[texture(0)]], device uchar4* out [[buffer(0)]], uint i [[thread_position_in_grid]]) { constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest); out[i] = bytes(t.sample(s, float2(0.5f), level(0))); }
kernel void writeView(texture2d<float, access::write> t [[texture(0)]], uint2 p [[thread_position_in_grid]]) { t.write(float4(17,31,73,127) / 255.0f, p); }
)";

std::array<std::byte, 4> Pixel(unsigned x, unsigned y, unsigned layer, unsigned level) {
    return {std::byte((x * 7 + y * 3 + layer * 43 + level * 19) & 255),
            std::byte((x * 5 + y * 11 + layer * 13 + level * 37 + 1) & 255),
            std::byte((x + y * 17 + layer * 29 + level * 23 + 2) & 255), std::byte(255)};
}

std::uint64_t Address(const MetalTexture& resource, unsigned x, unsigned y, unsigned layer, unsigned level) {
    const auto& g = resource.Geometry();
    const auto& mip = g.mips[level];
    if (resource.Descriptor().tileMode == TextureTileMode::kLinear) return g.GuestLayerOffset(layer) + mip.tiledOffset + y * mip.pitchBytes + x * 4;
    Require(!g.thick, "thin texture address fixture received a thick surface");
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
    d.mipCount = d.allocatedMipCount = dimension == TextureDimension::k1D ? 1 : 4;
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
    NSString* name = dimension == TextureDimension::k1D ? @"read1D" : dimension == TextureDimension::k3D ? @"read3D" : dimension == TextureDimension::k2D ? @"read2D" : @"readArray";
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

void VerifySurface(const MetalTests::Context& context, id<MTLLibrary> probes, GuestTextureResource descriptor) {
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
    auto readback = std::vector<std::byte>(texture.GuestBytes(),std::byte{0xa7});
    texture.Readback(readback);
    Require(readback == guest, "native texture readback changes guest pixels or padding");
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
    VerifyViews(context,probes);
    VerifyArrayView(context,probes);
    VerifyDecodedDescriptor(context,probes);
    VerifyThick(context,probes);
    AgcDriver::Metal::MetalDevice backend(context.device,context.library);
    for (unsigned unsupported=0;unsupported<3;++unsupported) {
        auto descriptor=Descriptor(TextureDimension::k2D);
        if (unsupported==0) descriptor.format=169;
        if (unsupported==1) descriptor.dccAddress=256;
        if (unsupported==2) descriptor.tileMode=TextureTileMode::kR64KBX;
        bool rejected=false;
        try { MetalTexture texture(backend,descriptor); } catch (const std::runtime_error& error) {
            const std::string_view message(error.what());
            rejected=message.find(unsupported==0 ? "BC compression" : unsupported==1 ? "DCC metadata" : "XOR tile modes")!=std::string_view::npos;
        }
        Require(rejected,"native texture unsupported feature must fail explicitly");
    }
}
