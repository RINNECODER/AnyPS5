#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "MetalDepthSurface.hpp"
#include "MetalRenderState.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

// Color attachments around multisampling, on synthetic guest state:
//   format_view - a COMP_SWAP_ALT 8_8_8_8 target is RGBA8 memory written as BGRA: the attachment
//                 renders through a BGRA view of the surface, so red lands in byte 2.
//   eqaa        - a 4x depth surface under 1x PA_SC_AA_CONFIG renders at 4 samples, and its 1x color
//                 target is expanded into a 4x transient and resolved back (untouched pixels exact).
//   msaa_color  - CB_COLOR_ATTRIB NUM_SAMPLES/NUM_FRAGMENTS decode to a 4-fragment target, rendered into
//                 a resident 2D multisample texture whose samples read back through texture2d_ms.
namespace {

using namespace AgcDriver;

constexpr std::uint32_t Edge = 8, Samples = 4;

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename Action>
void Rejects(const char* name, const char* reason, Action&& action) {
    try {
        action();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(reason) != std::string::npos, std::string(name) + " failed for another reason: " + error.what());
        return;
    }
    throw std::runtime_error(std::string(name) + " was accepted");
}

struct Guest {
    alignas(256) std::array<std::byte, 2048> color{};
    alignas(256) std::array<std::byte, Edge * Edge * Samples * 4> depth{};
};

// An 8x8 draw into CB slot 3 (export 3) with a Z32F depth surface tested LESS with writes.
QueueState Configure(const Guest& memory, std::uint32_t colorInfo, std::uint32_t colorAttrib, std::uint32_t zInfo, std::uint32_t aaConfig) {
    QueueState guest;
    const auto colorAddress = reinterpret_cast<std::uintptr_t>(memory.color.data());
    const auto depthAddress = reinterpret_cast<std::uintptr_t>(memory.depth.data());
    auto& cx = guest.context;
    guest.userConfig[0x242] = 4;
    cx[0x2d5] = 0x2000;
    cx[0x204] = 0x80000;
    cx[0x200] = 0x16;
    cx[0x010] = zInfo;
    cx[0x011] = 0;
    cx[0x007] = ((Edge - 1) << 16u) | (Edge - 1);
    cx[0x00b] = std::bit_cast<std::uint32_t>(1.0f);
    for (const auto offset : {0x012u, 0x014u}) cx[offset] = static_cast<std::uint32_t>(depthAddress >> 8u);
    for (const auto offset : {0x01au, 0x01cu}) cx[offset] = static_cast<std::uint32_t>(depthAddress >> 40u);
    cx[0x2f8] = aaConfig;
    cx[0x292] = aaConfig != 0 ? 3u : 2u;
    cx[0x8e] = 0xf000;
    cx[0x8f] = 0xffff;
    cx[0x1c3] = 4;
    cx[0x1c5] = 0x9000;
    constexpr auto stride = 3u * 0xfu;
    cx[0x318 + stride] = static_cast<std::uint32_t>(colorAddress >> 8u);
    cx[0x390 + 3] = static_cast<std::uint32_t>(colorAddress >> 40u);
    cx[0x31b + stride] = 0;
    cx[0x31c + stride] = colorInfo;
    cx[0x31d + stride] = colorAttrib;
    cx[0x3b0 + 3] = ((Edge - 1) << 14u) | (Edge - 1);
    cx[0x3b8 + 3] = 0x09000000;
    for (const auto offset : {0x10fu, 0x110u}) cx[offset] = std::bit_cast<std::uint32_t>(Edge / 2.0f);
    cx[0x111] = std::bit_cast<std::uint32_t>(-(Edge / 2.0f));
    cx[0x112] = std::bit_cast<std::uint32_t>(Edge / 2.0f);
    cx[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    cx[0x114] = 0;
    cx[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    cx[0x0d] = (Edge << 16u) | Edge;
    return guest;
}

constexpr std::uint32_t Rgba8Info = 0x8028;            // 8_8_8_8 UNORM, clamped, COMP_SWAP_STD.
constexpr std::uint32_t Bgra8Info = 0x8028 | (1u << 11u); // COMP_SWAP_ALT.
constexpr std::uint32_t Depth4x = 0x8000018bu, Depth1x = 0x80000183u;
constexpr std::uint32_t Attrib4x = (2u << 12u) | (2u << 15u); // NUM_SAMPLES and NUM_FRAGMENTS = 4.

id<MTLLibrary> Library(id<MTLDevice> device) {
    NSError* error = nil;
    auto library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 halfVertex(uint id [[vertex_id]]) {\n"
        "    const float2 corners[3] = {float2(-1, -1), float2(1, -1), float2(-1, 1)};\n"
        "    return float4(corners[id], 0.25, 1); }\n"
        "fragment float4 solidFragment() { return float4(1, 0.5, 0.25, 1); }\n"
        "kernel void readSamples(texture2d_ms<float> color [[texture(0)]], device float4* out [[buffer(0)]], uint2 id [[thread_position_in_grid]]) {\n"
        "    for (uint s = 0; s < 4; ++s) out[(id.y * 8 + id.x) * 4 + s] = color.read(id, s); }\n" options:nil error:&error];
    Require(library != nil, std::string("multisample color library failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
    return library;
}

id<MTLTexture> GuestSurface(id<MTLDevice> device, MTLPixelFormat format) {
    // The usage MetalTexture gives a renderable guest surface.
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:Edge height:Edge mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    auto texture = [device newTextureWithDescriptor:descriptor];
    Require(texture != nil, "guest surface allocation failed");
    return texture;
}

id<MTLRenderPipelineState> SolidPipeline(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat format, std::uint32_t samples) {
    auto descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"halfVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"solidFragment"];
    descriptor.colorAttachments[0].pixelFormat = format;
    descriptor.rasterSampleCount = samples;
    NSError* error = nil;
    auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    Require(pipeline != nil, std::string("solid pipeline failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
    return pipeline;
}

void FormatView(id<MTLDevice> device, id<MTLLibrary> library, const Guest& memory) {
    const auto state = Graphics::DecodeState(Configure(memory, Bgra8Info, 0, Depth1x, 0));
    Require(state.colors.size() == 1 && state.colors[0].format == VK_FORMAT_B8G8R8A8_UNORM, "COMP_SWAP_ALT 8_8_8_8 did not decode as a BGRA attachment");
    // The guest surface is typed by its memory layout, not by the swap.
    const auto guest = Graphics::FindGuestColorTargetFormat(state.colors[0].format, state.colors[0].elementBytes);
    Require(guest && Graphics::ResolveTextureFormat(*guest) == VK_FORMAT_R8G8B8A8_UNORM, "a swapped 8_8_8_8 target is not an RGBA8 memory surface");

    Metal::MetalDevice backend(device, library);
    auto surface = GuestSurface(device, MTLPixelFormatRGBA8Unorm);
    auto attachment = Metal::ColorAttachmentView(surface, Metal::RenderPixelFormat(state.colors[0].format));
    Require(attachment.pixelFormat == MTLPixelFormatBGRA8Unorm, "the swapped attachment is not a BGRA view");
    Require(Metal::ColorAttachmentView(surface, MTLPixelFormatRGBA8Unorm) == surface, "a matching attachment format made a view");
    Rejects("an RGBA16F attachment over RGBA8 memory", "disagrees with the guest state", [&] {
        static_cast<void>(Metal::ColorAttachmentView(surface, MTLPixelFormatRGBA16Float));
    });
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = attachment;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.2, 1.0);
    auto commands = backend.CommandBuffer();
    [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    backend.Wait(commands);
    std::array<std::uint8_t, Edge * Edge * 4> bytes{};
    [surface getBytes:bytes.data() bytesPerRow:Edge * 4 fromRegion:MTLRegionMake2D(0, 0, Edge, Edge) mipmapLevel:0];
    for (std::uint32_t pixel = 0; pixel < Edge * Edge; ++pixel) {
        Require(bytes[pixel * 4] == 0x33 && bytes[pixel * 4 + 1] == 0 && bytes[pixel * 4 + 2] == 0xff && bytes[pixel * 4 + 3] == 0xff,
                "a BGRA attachment did not store blue, green, red, alpha in memory order");
    }
    std::cout << "PASS: COMP_SWAP_ALT 8_8_8_8 renders through a BGRA view of its RGBA8 memory surface\n";
}

void Eqaa(id<MTLDevice> device, id<MTLLibrary> library, const Guest& memory) {
    const auto eqaa = Graphics::DecodeState(Configure(memory, Rgba8Info, 0, Depth4x, 0));
    Require(eqaa.depth && eqaa.depth->samples == Samples && eqaa.rasterSamples == Samples && eqaa.colors.size() == 1 && eqaa.colors[0].samples == 1,
            "a 4x depth surface under 1x rasterization does not render its 1x color target at 4 samples");
    const auto coverage = Graphics::DecodeState(Configure(memory, Rgba8Info, 0, Depth1x, 2));
    Require(coverage.depth && coverage.depth->samples == 1 && coverage.rasterSamples == 1,
            "a 1x depth surface under 4x rasterization does not render at the depth surface's sample count");
    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, eqaa);
    Require(descriptor.rasterSampleCount == Samples, "the EQAA pipeline does not rasterize at the depth sample count");

    // Expand a 1x target into the 4x pass, draw half of it, and resolve back.
    Metal::MetalDevice backend(device, library);
    Metal::MetalMultisampleExpander expander(backend);
    auto target = GuestSurface(device, MTLPixelFormatRGBA8Unorm);
    std::array<std::uint8_t, Edge * Edge * 4> original{};
    for (std::uint32_t i = 0; i < original.size(); ++i) original[i] = static_cast<std::uint8_t>(i * 7u + 3u);
    [target replaceRegion:MTLRegionMake2D(0, 0, Edge, Edge) mipmapLevel:0 withBytes:original.data() bytesPerRow:Edge * 4];
    auto commands = backend.CommandBuffer();
    auto transient = expander.Expand(commands, target, Samples, 3);
    Require(transient.sampleCount == Samples && transient.textureType == MTLTextureType2DMultisample, "the expansion target is not 4x multisample");
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = transient;
    pass.colorAttachments[0].resolveTexture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:SolidPipeline(device, library, MTLPixelFormatRGBA8Unorm, Samples)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    backend.Wait(commands);
    Require(expander.Expand(backend.CommandBuffer(), target, Samples, 3) == transient, "the slot's transient was reallocated");
    std::array<std::uint8_t, Edge * Edge * 4> resolved{};
    [target getBytes:resolved.data() bytesPerRow:Edge * 4 fromRegion:MTLRegionMake2D(0, 0, Edge, Edge) mipmapLevel:0];
    const std::array<std::uint8_t, 4> solid{0xff, 0x80, 0x40, 0xff};
    std::uint32_t untouched = 0, covered = 0, edge = 0;
    for (std::uint32_t pixel = 0; pixel < Edge * Edge; ++pixel) {
        const auto* value = resolved.data() + pixel * 4;
        if (std::memcmp(value, original.data() + pixel * 4, 4) == 0) ++untouched;
        else if (std::memcmp(value, solid.data(), 4) == 0) ++covered;
        else ++edge;
    }
    Require(untouched != 0 && covered != 0 && edge != 0, "the expanded 4x pass did not keep untouched texels and resolve mixed edges (untouched " +
            std::to_string(untouched) + ", covered " + std::to_string(covered) + ", edge " + std::to_string(edge) + ")");
    std::cout << "PASS: EQAA renders at the depth sample count; expand/resolve kept " << untouched << " texels, drew " << covered
              << " and resolved " << edge << " edge texels\n";
}

void MultisampleColor(id<MTLDevice> device, id<MTLLibrary> library, const Guest& memory) {
    const auto state = Graphics::DecodeState(Configure(memory, Rgba8Info | (1u << 14u), Attrib4x, Depth4x, 2));
    Require(state.colors.size() == 1 && state.colors[0].samples == Samples && state.colors[0].coverageSamples == Samples && state.rasterSamples == Samples,
            "a 4-sample 4-fragment FMASK-compressed color target did not decode as a 4x attachment");
    Rejects("a 2-fragment color target in a 4x depth pass", "cannot share", [&] {
        static_cast<void>(Graphics::DecodeState(Configure(memory, Rgba8Info, (2u << 12u) | (1u << 15u), Depth4x, 2)));
    });
    Rejects("fragments beyond the sample count", "exceed the color sample count", [&] {
        static_cast<void>(Graphics::DecodeState(Configure(memory, Rgba8Info, (1u << 12u) | (2u << 15u), Depth4x, 2)));
    });

    Metal::MetalDevice backend(device, library);
    Metal::MetalDepthSurfaceCache cache(backend);
    const auto surface = cache.Acquire(state.colors[0]);
    id<MTLTexture> texture = surface->Texture();
    Require(texture.textureType == MTLTextureType2DMultisample && texture.sampleCount == Samples && texture.pixelFormat == MTLPixelFormatRGBA8Unorm,
            "the resident color surface is not a 4x RGBA8 multisample texture");
    Require(cache.Acquire(state.colors[0]) == surface, "an identical multisampled color target was reallocated");
    auto commands = backend.CommandBuffer();
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:SolidPipeline(device, library, texture.pixelFormat, Samples)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    // The swizzled single-level view MetalDraw binds for a 2D MSAA descriptor of the color address.
    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:texture.pixelFormat textureType:texture.textureType levels:NSMakeRange(0, 1)
        slices:NSMakeRange(0, 1) swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha)];
    Require(view != nil, "the resident multisampled color view could not be created");
    NSError* error = nil;
    auto read = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"readSamples"] error:&error];
    auto output = [device newBufferWithLength:Edge * Edge * Samples * 16 options:MTLResourceStorageModeShared];
    Require(read != nil && output != nil, "multisample color readback setup failed");
    auto compute = [commands computeCommandEncoder];
    [compute setComputePipelineState:read];
    [compute setTexture:view atIndex:0];
    [compute setBuffer:output offset:0 atIndex:0];
    [compute dispatchThreads:MTLSizeMake(Edge, Edge, 1) threadsPerThreadgroup:MTLSizeMake(Edge, Edge, 1)];
    [compute endEncoding];
    backend.Wait(commands);
    const auto* values = static_cast<const float*>(output.contents);
    std::uint32_t covered = 0, empty = 0, partial = 0;
    for (std::uint32_t pixel = 0; pixel < Edge * Edge; ++pixel) {
        std::uint32_t inside = 0;
        for (std::uint32_t sample = 0; sample < Samples; ++sample) {
            const auto* value = values + (pixel * Samples + sample) * 4;
            const bool drawn = value[0] == 1.0f && value[3] == 1.0f;
            Require(drawn || (value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0), "a color sample is neither the cleared zero nor the drawn color");
            inside += drawn ? 1u : 0u;
        }
        (inside == Samples ? covered : inside == 0 ? empty : partial) += 1;
    }
    Require(covered != 0 && empty != 0 && partial != 0, "the resident color surface did not keep per-sample coverage (covered " +
            std::to_string(covered) + ", empty " + std::to_string(empty) + ", partial " + std::to_string(partial) + ")");
    std::cout << "PASS: 4x color target: " << covered << " covered, " << empty << " empty, " << partial << " pixels with mixed samples\n";
}

}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            const std::string mode = argc > 1 ? argv[1] : "all";
            Require(mode == "all" || mode == "format_view" || mode == "eqaa" || mode == "msaa_color", "Unknown multisample color mode");
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "Multisample color tests require a Metal device");
            Require([device supportsTextureSampleCount:Samples], "Metal device lacks 4x multisampling");
            static Guest memory;
            const std::array<NativeGuestMemory::BorrowedRange, 2> ranges{{
                {reinterpret_cast<std::uintptr_t>(memory.color.data()), memory.color, true},
                {reinterpret_cast<std::uintptr_t>(memory.depth.data()), memory.depth, true},
            }};
            const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
            auto library = Library(device);
            if (mode == "all" || mode == "format_view") FormatView(device, library, memory);
            if (mode == "all" || mode == "eqaa") Eqaa(device, library, memory);
            if (mode == "all" || mode == "msaa_color") MultisampleColor(device, library, memory);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
