#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "MetalDepthSurface.hpp"
#include "MetalRenderState.hpp"
#include "MetalShaderPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

// Multisampled depth: DB_Z_INFO NUM_SAMPLES with a matching PA_SC_AA_CONFIG decodes to a 4x depth
// target, the production depth cache allocates a 2D multisample texture, the production pipeline
// configuration rasterizes at 4x, and the resident sampled view of that surface (what a 2D MSAA
// image descriptor of the depth address binds to) reads every sample back. A rect-list draw into the
// same 4x target (a depth clear or fill) builds its quad-patch pipeline at 4x and rasterizes per sample.
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

// A depth-only draw: Z32F depth with the title's DB_Z_INFO layout (4 samples, 64KB_Z_X swizzle,
// ZRANGE_PRECISION), depth test LESS with writes, 4x rasterization and MSAA_ENABLE.
void Configure(QueueState& guest, std::uint64_t depthAddress, std::uint32_t zInfo, std::uint32_t aaConfig) {
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
    cx[0x8e] = 0;
    cx[0x8f] = 0;
    cx[0x1c3] = 4;
    cx[0x1c5] = 0;
    for (const auto offset : {0x10fu, 0x110u}) cx[offset] = std::bit_cast<std::uint32_t>(Edge / 2.0f);
    cx[0x111] = std::bit_cast<std::uint32_t>(-(Edge / 2.0f));
    cx[0x112] = std::bit_cast<std::uint32_t>(Edge / 2.0f);
    cx[0x113] = std::bit_cast<std::uint32_t>(1.0f);
    cx[0x114] = 0;
    cx[0xb5] = std::bit_cast<std::uint32_t>(1.0f);
    cx[0x0d] = (Edge << 16u) | Edge;
}

void Decode(std::uint64_t depthAddress, Graphics::State& msaa) {
    QueueState guest;
    Configure(guest, depthAddress, 0x8000018bu, 2u);
    Require(Graphics::DrawRejection(guest, false).empty(), "the 4x depth draw was pre-rejected: " + Graphics::DrawRejection(guest, false));
    msaa = Graphics::DecodeState(guest);
    Require(msaa.depth && msaa.depth->samples == Samples && msaa.rasterSamples == Samples && msaa.depth->format == VK_FORMAT_D32_SFLOAT &&
            msaa.depth->address == depthAddress && msaa.colors.empty() && msaa.depthWrite, "DB_Z_INFO 0x8000018b did not decode as a writable 4x Z32F target");

    // PARTIALLY_RESIDENT and MAXMIP describe the allocation; single-sample rendering uses its base level.
    QueueState resident;
    Configure(resident, depthAddress, (0x8000018bu & ~0xcu) | 0x1000u | 0x40000u, 0u);
    const auto single = Graphics::DecodeState(resident);
    Require(single.depth && single.depth->samples == 1 && single.rasterSamples == 1, "a partially resident mipmapped depth surface did not decode as one sample");

    QueueState eqaa;
    Configure(eqaa, depthAddress, 0x8000018bu, 0u);
    // EQAA / MSAA-off passes over a 4x depth surface render at the depth surface's sample count.
    const auto eqaaState = Graphics::DecodeState(eqaa);
    Require(eqaaState.depth && eqaaState.depth->samples == Samples && eqaaState.rasterSamples == Samples,
            "a 4x depth surface under 1x rasterization does not render at the depth surface's 4 samples");
    QueueState coverage;
    Configure(coverage, depthAddress, 0x8000018bu, 2u | (1u << 26u));
    Rejects("coverage-to-shader conversion", "coverage conversion", [&] { static_cast<void>(Graphics::DecodeState(coverage)); });
    Require(!Graphics::DrawRejection(coverage, false).empty(), "the draw pre-check accepted coverage conversion");
    std::cout << "PASS: DB_Z_INFO 0x8000018b decodes as 4x Z32F; PRT/MAXMIP surfaces decode; EQAA renders at the depth sample count; coverage conversion is rejected\n";
}

void DescriptorShape() {
    // R32_FLOAT 8x8 2D MSAA view (type 14) of the depth address: LAST_LEVEL = MAX_MIP = log2(4).
    const std::array<std::uint32_t, 8> words{0x3000u, (22u << 20u) | (3u << 30u), (7u << 14u) | 1u, 0xfacu | (2u << 16u) | (0x18u << 20u) | (14u << 28u), 0, 2u << 4u, 0, 0};
    const auto resource = Graphics::DecodeTextureResource(words);
    Require(resource.samples == Samples && resource.dimension == Graphics::TextureDimension::k2D && resource.lastLevel == 0 &&
            resource.baseLevel == 0 && resource.mipCount == 1 && resource.width == Edge && resource.height == Edge,
            "a 2D MSAA image descriptor did not decode as a single-level 4-sample 2D view");
    Require(Graphics::MatchesGuestDimension(ShaderRecompiler::DescriptorImageShape::Image2DMultisample, resource.dimension),
            "a 2D MSAA descriptor does not match the multisample shader shape");
    std::cout << "PASS: 2D MSAA image descriptor decodes as a 4-sample single-level 2D view\n";
}

id<MTLLibrary> Library(id<MTLDevice> device) {
    NSError* error = nil;
    auto library = [device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 halfVertex(uint id [[vertex_id]]) {\n"
        "    const float2 corners[3] = {float2(-1, -1), float2(1, -1), float2(-1, 1)};\n"
        "    return float4(corners[id], 0.25, 1); }\n"
        "kernel void readSamples(depth2d_ms<float> depth [[texture(0)]], device float* out [[buffer(0)]], uint2 id [[thread_position_in_grid]]) {\n"
        "    for (uint s = 0; s < 4; ++s) out[(id.y * 8 + id.x) * 4 + s] = depth.read(id, s); }\n" options:nil error:&error];
    Require(library != nil, std::string("multisample depth library failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
    return library;
}

void Render(id<MTLDevice> device, const Graphics::State& state) {
    auto library = Library(device);
    Metal::MetalDevice backend(device, library);
    Metal::MetalDepthSurfaceCache cache(backend);
    const auto surface = cache.Acquire(*state.depth);
    id<MTLTexture> depth = surface->DepthTexture();
    Require(depth.textureType == MTLTextureType2DMultisample && depth.sampleCount == Samples && depth.pixelFormat == MTLPixelFormatDepth32Float,
            "the depth cache did not allocate a 4x 2D multisample Depth32Float texture");
    Require(cache.Acquire(*state.depth) == surface, "an identical multisampled depth target was reallocated");
    auto single = *state.depth;
    single.samples = 1;
    Require(cache.Acquire(single)->DepthTexture().sampleCount == 1, "a 1x target at the same address reused the 4x surface");

    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, state);
    Require(descriptor.rasterSampleCount == Samples, "the pipeline does not rasterize at the decoded sample count");
    descriptor.vertexFunction = [library newFunctionWithName:@"halfVertex"];
    NSError* error = nil;
    auto pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    Require(pipeline != nil, std::string("4x depth-only pipeline creation failed: ") + (error.localizedDescription.UTF8String ?: "unknown"));
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.renderTargetWidth = state.renderExtent.width;
    pass.renderTargetHeight = state.renderExtent.height;
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    auto commands = backend.CommandBuffer();
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setDepthStencilState:Metal::CreateDepthStencilState(device, state)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];

    // The resident view MetalDraw binds for a 2D MSAA descriptor of the depth address.
    id<MTLTexture> view = [surface->SampledDepthView() newTextureViewWithPixelFormat:depth.pixelFormat textureType:depth.textureType
        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
        swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha)];
    Require(view != nil && view.sampleCount == Samples, "the resident multisampled depth view could not be created");
    auto output = [device newBufferWithLength:Edge * Edge * Samples * sizeof(float) options:MTLResourceStorageModeShared];
    auto read = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"readSamples"] error:&error];
    Require(read != nil && output != nil, "multisample depth readback setup failed");
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
            const auto value = values[pixel * Samples + sample];
            Require(value == 0.25f || value == 1.0f, "a depth sample is neither the cleared 1.0 nor the drawn 0.25");
            inside += value == 0.25f ? 1u : 0u;
        }
        (inside == Samples ? covered : inside == 0 ? empty : partial) += 1;
    }
    Require(covered != 0 && empty != 0 && partial != 0, "the diagonal edge has no partially covered pixels, so the draw did not rasterize at 4x (covered " +
            std::to_string(covered) + ", empty " + std::to_string(empty) + ", partial " + std::to_string(partial) + ")");
    std::cout << "PASS: 4x depth-only draw: " << covered << " covered, " << empty << " empty, " << partial << " edge pixels with mixed samples\n";
}


id<MTLTexture> ResidentDepthView(const Metal::MetalDepthSurface& surface) {
    id<MTLTexture> depth = surface.DepthTexture();
    return [surface.SampledDepthView() newTextureViewWithPixelFormat:depth.pixelFormat textureType:depth.textureType
        levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
        swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha)];
}

// Counts pixels whose 4 samples are all drawn (0.25), none drawn (1.0) or mixed.
std::array<std::uint32_t, 3> ReadCoverage(id<MTLDevice> device, id<MTLLibrary> library, id<MTLCommandBuffer> commands,
    const Metal::MetalDevice& backend, const Metal::MetalDepthSurface& surface) {
    id<MTLTexture> view = ResidentDepthView(surface);
    NSError* error = nil;
    auto output = [device newBufferWithLength:Edge * Edge * Samples * sizeof(float) options:MTLResourceStorageModeShared];
    auto read = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"readSamples"] error:&error];
    Require(view != nil && read != nil && output != nil, "multisample depth readback setup failed");
    auto compute = [commands computeCommandEncoder];
    [compute setComputePipelineState:read];
    [compute setTexture:view atIndex:0];
    [compute setBuffer:output offset:0 atIndex:0];
    [compute dispatchThreads:MTLSizeMake(Edge, Edge, 1) threadsPerThreadgroup:MTLSizeMake(Edge, Edge, 1)];
    [compute endEncoding];
    backend.Wait(commands);
    const auto* values = static_cast<const float*>(output.contents);
    std::array<std::uint32_t, 3> counts{};
    for (std::uint32_t pixel = 0; pixel < Edge * Edge; ++pixel) {
        std::uint32_t inside = 0;
        for (std::uint32_t sample = 0; sample < Samples; ++sample) {
            const auto value = values[pixel * Samples + sample];
            Require(value == 0.25f || value == 1.0f, "a depth sample is neither the cleared 1.0 nor the drawn 0.25");
            inside += value == 0.25f ? 1u : 0u;
        }
        ++counts[inside == Samples ? 0 : inside == 0 ? 1 : 2];
    }
    return counts;
}

// A rect-list depth draw into the 4x target: the production pipeline descriptor (quad patches, 4x
// rasterization) and the production rectangle-evaluation pipeline contract accept it, and the quad,
// whose right edge crosses pixel column 5 between sample positions, covers some samples of that column.
void RectRender(id<MTLDevice> device, std::uint64_t depthAddress) {
    QueueState guest;
    Configure(guest, depthAddress, 0x8000018bu, 2u);
    guest.userConfig[0x242] = 0x11; // DI_PT_RECTLIST
    const auto state = Graphics::DecodeState(guest);
    Require(state.rectList && state.rasterSamples == Samples && state.depth && state.depth->samples == Samples,
            "a rect-list draw into the 4x depth target did not decode as a 4x rectangle draw");
    auto library = Library(device);
    Metal::MetalDevice backend(device, library);
    Metal::MetalDepthSurfaceCache cache(backend);
    const auto surface = cache.Acquire(*state.depth);
    auto descriptor = [MTLRenderPipelineDescriptor new];
    Metal::ConfigureRenderPipelineDescriptor(descriptor, state);
    Require(descriptor.rasterSampleCount == Samples, "the rectangle pipeline does not rasterize at the decoded sample count");

    namespace Bridge = ShaderRecompiler::MetalBackend;
    const std::string source = "#include <metal_stdlib>\nusing namespace metal;\n"
        "[[patch(quad, 4)]] vertex float4 rectEvaluation(float2 uv [[position_in_patch]]) {\n"
        "    return float4(mix(-1.0f, 0.375f, uv.x), mix(-1.0f, 1.0f, uv.y), 0.25f, 1.0f); }\n"
        "fragment void rectDepth() {}\n";
    Bridge::Result vertex{};
    vertex.stage = ShaderRecompiler::ShaderStage::TessellationEvaluation;
    vertex.nativeExecutionKind = Bridge::NativeExecutionKind::Vertex;
    vertex.source = source;
    vertex.entryPoint = "rectEvaluation";
    Bridge::RectListInfo rect{};
    rect.mode = Bridge::RectListMode::Evaluation;
    rect.inputLayout.stride = rect.inputLayout.alignment = 16;
    rect.inputLayout.members.push_back({std::nullopt, std::nullopt, 4, 32, 0, 16});
    vertex.rectList = rect;
    Bridge::Result fragment{};
    fragment.stage = ShaderRecompiler::ShaderStage::Fragment;
    fragment.nativeExecutionKind = Bridge::NativeExecutionKind::Fragment;
    fragment.source = source;
    fragment.entryPoint = "rectDepth";
    const Metal::MetalRenderPipeline pipeline(device, vertex, fragment, descriptor);

    struct Factors { std::uint16_t edge[4]; std::uint16_t inside[2]; };
    const Factors one{{0x3c00, 0x3c00, 0x3c00, 0x3c00}, {0x3c00, 0x3c00}};
    auto factors = [device newBufferWithBytes:&one length:sizeof one options:MTLResourceStorageModeShared];
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.renderTargetWidth = state.renderExtent.width;
    pass.renderTargetHeight = state.renderExtent.height;
    pass.depthAttachment.texture = surface->DepthTexture();
    pass.depthAttachment.loadAction = MTLLoadActionLoad;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    auto commands = backend.CommandBuffer();
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    pipeline.Bind(encoder, {}, {}, {}, {}, {}, {}, 1);
    Metal::BindRenderState(encoder, state, Metal::CreateDepthStencilState(device, state));
    [encoder setTessellationFactorBuffer:factors offset:0 instanceStride:0];
    [encoder drawPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
    [encoder endEncoding];
    const auto [covered, empty, partial] = ReadCoverage(device, library, commands, backend, *surface);
    Require(covered == 5 * Edge && empty == 2 * Edge && partial == Edge, "the 4x rectangle did not cover columns 0-4, part of column 5 and none of 6-7 (covered " +
            std::to_string(covered) + ", empty " + std::to_string(empty) + ", partial " + std::to_string(partial) + ")");
    std::cout << "PASS: 4x rect-list depth draw: " << covered << " covered, " << empty << " empty, " << partial << " pixels with mixed samples\n";
}

}

int main() {
    @autoreleasepool {
        try {
            auto device = MTLCreateSystemDefaultDevice();
            Require(device != nil, "Multisample depth tests require a Metal device");
            Require([device supportsTextureSampleCount:Samples], "Metal device lacks 4x multisampling");
            static std::array<std::byte, 65536 + Edge * Edge * Samples * 4> depthStorage{};
            const auto rawAddress = reinterpret_cast<std::uintptr_t>(depthStorage.data());
            const auto depthAddress = (rawAddress + 65535ull) & ~65535ull;
            const auto depthSpan = std::span<std::byte>(reinterpret_cast<std::byte*>(depthAddress), Edge * Edge * Samples * 4);
            const std::array<NativeGuestMemory::BorrowedRange, 1> ranges{{{depthAddress, depthSpan, true}}};
            const NativeGuestMemory::BorrowedRangesScope borrowed(ranges);
            Graphics::State state;
            Decode(depthAddress, state);
            DescriptorShape();
            Render(device, state);
            RectRender(device, depthAddress);
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
}
