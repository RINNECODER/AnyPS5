#pragma once

#import <Metal/Metal.h>
#include "MetalBackend/MetalShaderBridge.hpp"
#include <optional>
#include <span>

namespace AgcDriver::Metal {

struct MetalBufferBinding {
    id<MTLBuffer> buffer = nil;
    NSUInteger offset = 0;
    std::optional<NSUInteger> length;
};

struct MetalShaderResourceBinding {
    std::uint32_t descriptorSet = 0;
    std::uint32_t binding = 0;
    std::vector<MetalBufferBinding> buffers;
    std::vector<id<MTLTexture>> textures;
    std::vector<id<MTLSamplerState>> samplers;
};

struct MetalImplicitBufferBinding {
    std::uint32_t index;
    MetalBufferBinding buffer;
};

class MetalRectKernelPipeline {
public:
    explicit MetalRectKernelPipeline(id<MTLDevice> device, ShaderRecompiler::MetalBackend::Result shader,
                                    MTLStageInputOutputDescriptor* captureInputs = nil);
    [[nodiscard]] const ShaderRecompiler::MetalBackend::Result& Reflection() const;
    void Encode(id<MTLCommandBuffer> commands, std::span<const MetalShaderResourceBinding> bindings,
                MTLRegion region, std::span<const MetalImplicitBufferBinding> implicitBuffers,
                std::span<const MetalBufferBinding> vertexBuffers = {},
                std::span<const std::byte> pushConstants = {},
                std::span<const id<MTLResource>> indirectResources = {}) const;

private:
    id<MTLDevice> device;
    id<MTLComputePipelineState> pipeline;
    id<MTLFunction> samplerFunction = nil;
    std::array<NSUInteger, 31> bufferAlignments{};
    ShaderRecompiler::MetalBackend::Result shader;
};

class MetalComputePipeline {
public:
    explicit MetalComputePipeline(id<MTLDevice> device, ShaderRecompiler::MetalBackend::Result shader);
    [[nodiscard]] const ShaderRecompiler::MetalBackend::Result& Reflection() const;
    void Encode(id<MTLCommandBuffer> commands, std::span<const MetalShaderResourceBinding> bindings,
                MTLSize grid, std::span<const std::byte> pushConstants = {},
                std::span<const id<MTLResource>> indirectResources = {}) const;

private:
    id<MTLDevice> device;
    id<MTLComputePipelineState> pipeline;
    id<MTLFunction> samplerFunction = nil;
    std::array<NSUInteger, 31> bufferAlignments{};
    ShaderRecompiler::MetalBackend::Result shader;
};

class MetalRenderPipeline {
public:
    explicit MetalRenderPipeline(id<MTLDevice> device, ShaderRecompiler::MetalBackend::Result vertex,
                                 ShaderRecompiler::MetalBackend::Result fragment,
                                 MTLRenderPipelineDescriptor* descriptor);
    [[nodiscard]] const ShaderRecompiler::MetalBackend::Result& VertexReflection() const;
    [[nodiscard]] const ShaderRecompiler::MetalBackend::Result& FragmentReflection() const;
    void Bind(id<MTLRenderCommandEncoder> encoder,
              std::span<const MetalShaderResourceBinding> vertexBindings,
              std::span<const MetalShaderResourceBinding> fragmentBindings,
              std::span<const std::byte> vertexPushConstants = {},
              std::span<const std::byte> fragmentPushConstants = {},
              std::span<const id<MTLResource>> indirectResources = {},
              std::span<const MetalImplicitBufferBinding> implicitBuffers = {},
              std::size_t rectPatchCount = 0) const;

private:
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline;
    id<MTLFunction> vertexSamplerFunction = nil;
    id<MTLFunction> fragmentSamplerFunction = nil;
    std::array<NSUInteger, 31> vertexBufferAlignments{};
    std::array<NSUInteger, 31> fragmentBufferAlignments{};
    ShaderRecompiler::MetalBackend::Result vertex;
    ShaderRecompiler::MetalBackend::Result fragment;
};

class MetalMeshPipeline {
public:
    explicit MetalMeshPipeline(id<MTLDevice> device, ShaderRecompiler::MetalBackend::Result mesh,
                              ShaderRecompiler::MetalBackend::Result fragment,
                              MTLMeshRenderPipelineDescriptor* descriptor);
    [[nodiscard]] const ShaderRecompiler::MetalBackend::Result& MeshReflection() const;
    [[nodiscard]] const ShaderRecompiler::MetalBackend::Result& FragmentReflection() const;
    void ValidateThreadgroups(MTLSize groups) const;
    void Bind(id<MTLRenderCommandEncoder> encoder,
              std::span<const MetalShaderResourceBinding> meshBindings,
              std::span<const MetalShaderResourceBinding> fragmentBindings,
              std::span<const std::byte> meshPushConstants = {},
              std::span<const std::byte> fragmentPushConstants = {},
              std::span<const id<MTLResource>> indirectResources = {}) const;

private:
    id<MTLDevice> device;
    id<MTLRenderPipelineState> pipeline;
    id<MTLFunction> meshSamplerFunction = nil;
    id<MTLFunction> fragmentSamplerFunction = nil;
    std::array<NSUInteger, 31> meshBufferAlignments{};
    std::array<NSUInteger, 31> fragmentBufferAlignments{};
    ShaderRecompiler::MetalBackend::Result mesh;
    ShaderRecompiler::MetalBackend::Result fragment;
};

}
