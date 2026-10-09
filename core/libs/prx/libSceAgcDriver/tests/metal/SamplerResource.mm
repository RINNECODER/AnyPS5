#include "MetalTestSupport.hpp"
#include "MetalSampler.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {

using AgcDriver::Graphics::GuestSamplerResource;
using AgcDriver::Metal::MetalSampler;
using Pixel = std::array<float, 4>;

struct SampleParameters {
    float u = 0.5f;
    float v = 0.5f;
    float lod = 0.0f;
    float reference = 0.5f;
    std::uint32_t gradients = 0;
};

GuestSamplerResource Resource(std::uint32_t clamp = 2u, std::uint32_t mag = 0u, std::uint32_t min = 0u, std::uint32_t mip = 1u) {
    const std::array<std::uint32_t, 4> words{
        clamp | (clamp << 3u) | (clamp << 6u), 0xfffu << 12u,
        (mag << 20u) | (min << 22u) | (mip << 26u), 0u
    };
    return AgcDriver::Graphics::DecodeSamplerResource(words);
}

class Probe {
public:
    explicit Probe(const MetalTests::Context& context) : context(context) {
        NSString* source = @R"(
#include <metal_stdlib>
using namespace metal;
struct Params { float u; float v; float lod; float reference; uint gradients; };
kernel void colorProbe(device float4* result [[buffer(0)]], constant Params& p [[buffer(1)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    if (p.gradients != 0u) {
        float scale = exp2(p.lod);
        result[0] = t.sample(s, float2(p.u, p.v), gradient2d(float2(scale / t.get_width(), 0.0), float2(0.0, scale / t.get_height())));
    } else {
        result[0] = t.sample(s, float2(p.u, p.v), level(p.lod));
    }
}
kernel void depthProbe(device float4* result [[buffer(0)]], constant Params& p [[buffer(1)]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    result[0] = float4(t.sample_compare(s, float2(p.u, p.v), p.reference, level(0.0)));
}
)";
        NSError* error = nil;
        auto library = [context.device newLibraryWithSource:source options:nil error:&error];
        if (library == nil) throw std::runtime_error(error.localizedDescription.UTF8String ?: "Sampler probe compilation failed");
        color = [context.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"colorProbe"] error:&error];
        depth = [context.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"depthProbe"] error:&error];
        MetalTests::Require(color != nil && depth != nil, "Sampler probe pipeline creation failed");
    }

    Pixel Sample(const GuestSamplerResource& resource, id<MTLTexture> texture, SampleParameters parameters = {}, bool compare = false) const {
        MetalSampler sampler(context.device, resource);
        return Sample(sampler.Handle(), texture, parameters, compare);
    }

    Pixel Sample(id<MTLSamplerState> sampler, id<MTLTexture> texture, SampleParameters parameters = {}, bool compare = false) const {
        auto result = context.Buffer(sizeof(Pixel), 0xa5);
        auto commands = [context.queue commandBuffer];
        auto encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:compare ? depth : color];
        [encoder setBuffer:result offset:0 atIndex:0];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:1];
        [encoder setTexture:texture atIndex:0];
        [encoder setSamplerState:sampler atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Sampler probe GPU execution failed");
        Pixel value;
        std::memcpy(value.data(), result.contents, sizeof(value));
        return value;
    }

private:
    const MetalTests::Context& context;
    id<MTLComputePipelineState> color;
    id<MTLComputePipelineState> depth;
};

void RequirePixel(const Pixel& value, const Pixel& expected, const char* message) {
    for (std::size_t i = 0; i < value.size(); ++i) MetalTests::Require(std::isfinite(value[i]) && std::fabs(value[i] - expected[i]) < 0.00002f, message);
}

id<MTLTexture> ColorTexture(const MetalTests::Context& context, std::uint32_t width, bool mipmapped, bool uniformLevels) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:width height:width mipmapped:mipmapped];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    auto texture = [context.device newTextureWithDescriptor:descriptor];
    MetalTests::Require(texture != nil, "Sampler fixture texture creation failed");
    for (NSUInteger level = 0; level < texture.mipmapLevelCount; ++level) {
        const auto extent = std::max<NSUInteger>(width >> level, 1u);
        std::vector<Pixel> pixels(extent * extent);
        for (NSUInteger y = 0; y < extent; ++y) {
            for (NSUInteger x = 0; x < extent; ++x) {
                pixels[y * extent + x] = uniformLevels ? Pixel{level == 0 ? 1.0f : 0.0f, level == 1 ? 1.0f : 0.0f, level == 2 ? 1.0f : 0.0f, 1.0f}
                    : Pixel{x < extent / 2u ? 0.0f : 1.0f, y < extent / 2u ? 0.0f : 1.0f, 0.0f, 1.0f};
            }
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, extent, extent) mipmapLevel:level withBytes:pixels.data() bytesPerRow:extent * sizeof(Pixel)];
    }
    return texture;
}

void CheckAddressing(const MetalTests::Context& context, const Probe& probe) {
    auto texture = ColorTexture(context, 2u, false, false);
    constexpr std::array<std::uint32_t, 5> modes{0u, 1u, 2u, 3u, 4u};
    constexpr std::array<Pixel, 5> negativeU{{{1, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 0}}};
    constexpr std::array<Pixel, 5> positiveV{{{0, 0, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}}};
    for (std::size_t i = 0; i < modes.size(); ++i) {
        RequirePixel(probe.Sample(Resource(modes[i]), texture, {-0.25f, 0.25f}), negativeU[i], "Guest sampler U addressing produced the wrong texel or border");
        RequirePixel(probe.Sample(Resource(modes[i]), texture, {0.25f, 1.25f}), positiveV[i], "Guest sampler V addressing produced the wrong texel or border");
    }
    for (std::uint32_t border = 0; border < 3u; ++border) {
        const std::array<std::uint32_t, 4> words{4u | (4u << 3u) | (4u << 6u), 0u, 0u, border << 30u};
        MetalSampler sampler(context.device, words);
        const Pixel expected = border == 0u ? Pixel{0, 0, 0, 0} : border == 1u ? Pixel{0, 0, 0, 1} : Pixel{1, 1, 1, 1};
        RequirePixel(probe.Sample(sampler.Handle(), texture, {-1.0f, -1.0f}), expected, "Guest sampler border color lost its RGB or alpha");
    }
}

void CheckFilters(const MetalTests::Context& context, const Probe& probe) {
    auto texture = ColorTexture(context, 4u, true, false);
    for (std::uint32_t filter = 0u; filter < 2u; ++filter) {
        const Pixel expected = filter == 0u ? Pixel{1, 1, 0, 1} : Pixel{0.5f, 0.5f, 0, 1};
        RequirePixel(probe.Sample(Resource(2u, filter, 1u - filter), texture, {0.5f, 0.5f, -1.0f, 0.5f, 1u}), expected, "Guest sampler magnification filter selected the wrong pixel blend");
        RequirePixel(probe.Sample(Resource(2u, 1u - filter, filter), texture, {0.5f, 0.5f, 1.0f, 0.5f, 1u}), expected, "Guest sampler minification filter selected the wrong pixel blend");
    }
    auto levels = ColorTexture(context, 4u, true, true);
    RequirePixel(probe.Sample(Resource(), levels, {0.5f, 0.5f, 0.25f, 0.5f, 1u}), {1, 0, 0, 1}, "Guest nearest mip filtering selected the wrong level");
    RequirePixel(probe.Sample(Resource(2u, 0u, 0u, 2u), levels, {0.5f, 0.5f, 0.25f}), {0.75f, 0.25f, 0, 1}, "Guest linear mip filtering lost interpolation");
    auto resource = Resource();
    resource.minLod = 1.0f;
    RequirePixel(probe.Sample(resource, levels, {0.5f, 0.5f, -1.0f, 0.5f, 1u}), {0, 1, 0, 1}, "Guest minimum LOD clamp was ignored");
    resource = Resource();
    resource.maxLod = 0.0f;
    RequirePixel(probe.Sample(resource, levels, {0.5f, 0.5f, 2.0f, 0.5f, 1u}), {1, 0, 0, 1}, "Guest maximum LOD clamp was ignored");
    bool biasSupported = false;
    if (@available(macOS 26.0, *)) biasSupported = [context.device supportsFamily:MTLGPUFamilyApple10];
    if (biasSupported) {
        resource = Resource();
        resource.lodBias = 1.0f;
        RequirePixel(probe.Sample(resource, levels, {0.5f, 0.5f, 0.0f, 0.5f, 1u}), {0, 1, 0, 1}, "Guest positive sampler LOD bias was ignored");
        resource.lodBias = -1.0f;
        RequirePixel(probe.Sample(resource, levels, {0.5f, 0.5f, 1.0f, 0.5f, 1u}), {1, 0, 0, 1}, "Guest negative sampler LOD bias was ignored");
    } else {
        resource = Resource();
        resource.lodBias = 1.0f;
        bool rejected = false;
        try { MetalSampler sampler(context.device, resource); }
        catch (const std::invalid_argument& error) { rejected = std::string(error.what()).find("macOS 26 and Apple10 GPU family") != std::string::npos; }
        MetalTests::Require(rejected, "Guest sampler LOD bias was accepted without native hardware support");
    }
    for (std::uint32_t filter = 2u; filter < 4u; ++filter) {
        resource = Resource(2u, filter, filter);
        for (const float ratio : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f}) {
            resource.maxAnisotropy = ratio;
            RequirePixel(probe.Sample(resource, levels, {0.5f, 0.5f, 1.0f, 0.5f, 1u}), {0, 1, 0, 1}, "Guest anisotropic sampler changed a constant mip level");
        }
    }
}

void CheckCompare(const MetalTests::Context& context, const Probe& probe) {
    auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:2 height:2 mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    auto texture = [context.device newTextureWithDescriptor:descriptor];
    MetalTests::Require(texture != nil, "Sampler depth texture creation failed");
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = texture;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.5;
    auto commands = [context.queue commandBuffer];
    auto encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    MetalTests::Require(commands.status == MTLCommandBufferStatusCompleted, "Sampler depth fixture clear failed");
    constexpr std::array<std::array<float, 3>, 8> expected{{
        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}
    }};
    constexpr std::array<float, 3> references{0.25f, 0.5f, 0.75f};
    for (std::uint32_t operation = 0u; operation < 8u; ++operation) {
        const std::array<std::uint32_t, 4> words{2u | (2u << 3u) | (2u << 6u) | (operation << 12u), 0u, 0u, 0u};
        MetalSampler sampler(context.device, words, true);
        for (std::size_t i = 0; i < references.size(); ++i) {
            const auto value = expected[operation][i];
            RequirePixel(probe.Sample(sampler.Handle(), texture, {0.5f, 0.5f, 0.0f, references[i]}, true), {value, value, value, value}, "Guest depth comparison selected the wrong reference relation");
        }
    }
}

void CheckRejected(const MetalTests::Context& context) {
    const auto invalid = [&context](const std::function<void(GuestSamplerResource&)>& modify) {
        auto resource = Resource();
        modify(resource);
        bool rejected = false;
        try { MetalSampler sampler(context.device, resource); }
        catch (const std::invalid_argument&) { rejected = true; }
        MetalTests::Require(rejected, "Unsupported guest sampler metadata was silently accepted");
    };
    invalid([](auto& resource) { resource.minFilter = VK_FILTER_CUBIC_EXT; });
    invalid([](auto& resource) { resource.magFilter = VK_FILTER_CUBIC_EXT; });
    invalid([](auto& resource) { resource.mipmapMode = static_cast<VkSamplerMipmapMode>(99); });
    invalid([](auto& resource) { resource.addressModeW = static_cast<VkSamplerAddressMode>(99); });
    invalid([](auto& resource) { resource.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT; });
    invalid([](auto& resource) { resource.compareOp = static_cast<VkCompareOp>(99); });
    invalid([](auto& resource) { resource.minLod = -1.0f; });
    invalid([](auto& resource) { resource.minLod = resource.maxLod + 1.0f; });
    invalid([](auto& resource) { resource.maxLod = std::numeric_limits<float>::quiet_NaN(); });
    invalid([](auto& resource) { resource.lodBias = 16.0f; });
    invalid([](auto& resource) { resource.lodBias = 1.0f / 256.0f; });
    invalid([](auto& resource) { resource.anisotropyEnable = true; resource.maxAnisotropy = 1.5f; });
    invalid([](auto& resource) { resource.anisotropyEnable = true; resource.maxAnisotropy = 17.0f; });
}

void CheckReduction(const MetalTests::Context& context, const Probe& probe) {
    auto texture = ColorTexture(context, 2u, false, false);
    for (std::uint32_t mode = 1u; mode < 3u; ++mode) {
        for (std::uint32_t filter = 0u; filter < 2u; ++filter) {
            const std::array<std::uint32_t, 4> words{2u | (2u << 3u) | (2u << 6u) | (mode << 29u), 0u, (filter << 20u) | (filter << 22u) | (1u << 26u), 0u};
            MetalTests::Require(AgcDriver::Graphics::DecodeSamplerResource(words).reductionMode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE_EXT,
                "Guest min/max reduction sampler fixture did not decode its reduction mode");
            if (filter == 0u) {
                MetalSampler sampler(context.device, words);
                RequirePixel(probe.Sample(sampler.Handle(), texture, {0.75f, 0.25f}), {1, 0, 0, 1}, "Guest nearest min/max reduction sampler selected the wrong texel");
                continue;
            }
            bool rejected = false;
            try { MetalSampler sampler(context.device, words); }
            catch (const std::invalid_argument& error) { rejected = std::string(error.what()).find("min/max reduction") != std::string::npos; }
            MetalTests::Require(rejected, "Guest linear min/max reduction sampler was silently built as a weighted average");
        }
    }
}

}

void RunSamplerResourceTests(const MetalTests::Context& context) {
    Probe probe(context);
    CheckAddressing(context, probe);
    CheckFilters(context, probe);
    CheckCompare(context, probe);
    CheckRejected(context);
    CheckReduction(context, probe);
}
