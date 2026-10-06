#include "MetalShaderPipeline.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace AgcDriver::Metal {

namespace {

using ShaderResult = ShaderRecompiler::MetalBackend::Result;
using ResourceMapping = ShaderRecompiler::MetalBackend::ResourceMapping;
using DescriptorKind = ShaderRecompiler::DescriptorKind;

std::string errorMessage(const char* operation, NSError* error) {
    const char* message = error.localizedDescription.UTF8String;
    return std::string(operation) + ": " + (message == nullptr ? "no Metal error description" : message);
}

id<MTLFunction> compile(id<MTLDevice> device, const ShaderResult& shader, MTLFunctionType type) {
    if (device == nil || shader.source.empty() || shader.entryPoint.empty()) {
        throw std::invalid_argument("Metal shader compilation requires a device, converted source, and entry point");
    }
    const auto stage = type == MTLFunctionTypeKernel ? ShaderRecompiler::ShaderStage::Compute :
                       type == MTLFunctionTypeVertex ? ShaderRecompiler::ShaderStage::Vertex :
                                                      ShaderRecompiler::ShaderStage::Fragment;
    if (shader.stage != stage) {
        throw std::invalid_argument("Converted Metal shader reflection has the wrong pipeline stage");
    }
    NSString* source = [[NSString alloc] initWithBytes:shader.source.data() length:shader.source.size()
                                            encoding:NSUTF8StringEncoding];
    NSString* entryPoint = [[NSString alloc] initWithBytes:shader.entryPoint.data() length:shader.entryPoint.size()
                                                encoding:NSUTF8StringEncoding];
    if (source == nil || entryPoint == nil || shader.entryPoint.find('\0') != std::string::npos) {
        throw std::invalid_argument("Metal converted shader source and entry point must be valid UTF-8");
    }
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
    } else {
        throw std::runtime_error("Converted Metal shader pipelines require macOS 15 or later");
    }
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (library == nil) {
        throw std::runtime_error(errorMessage("Converted Metal shader compilation failed", error));
    }
    id<MTLFunction> function = [library newFunctionWithName:entryPoint];
    if (function == nil || function.functionType != type) {
        throw std::invalid_argument("Converted Metal shader entry point has the wrong stage or is missing");
    }
    return function;
}

struct BufferArgument {
    NSUInteger index;
    MetalBufferBinding resource;
};

struct TextureArgument {
    NSUInteger index;
    id<MTLTexture> texture;
};

struct SamplerArgument {
    NSUInteger index;
    id<MTLSamplerState> sampler;
};

struct PreparedBindings {
    std::vector<BufferArgument> buffers;
    std::vector<TextureArgument> textures;
    std::vector<SamplerArgument> samplers;
    std::array<std::uint32_t, 31> lengths{};
    std::span<const std::byte> pushConstants;
};

void reserveSlots(std::set<std::uint32_t>& slots, std::uint32_t first, std::uint32_t count,
                  std::uint32_t limit) {
    if (count == 0 || first >= limit || count > limit - first) {
        throw std::invalid_argument("Converted Metal shader resource slots exceed the native argument limit");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!slots.insert(first + i).second) {
            throw std::invalid_argument("Converted Metal shader resource slots overlap");
        }
    }
}

void validateKind(const ResourceMapping& mapping) {
    bool buffer = false;
    bool texture = false;
    bool sampler = false;
    switch (mapping.kind) {
    case DescriptorKind::UniformBuffer:
    case DescriptorKind::StorageBuffer:
        buffer = true;
        break;
    case DescriptorKind::UniformTexelBuffer:
    case DescriptorKind::StorageTexelBuffer:
    case DescriptorKind::SampledImage:
    case DescriptorKind::StorageImage:
        texture = true;
        break;
    case DescriptorKind::Sampler:
        sampler = true;
        break;
    default:
        throw std::invalid_argument("Converted Metal shader descriptor type is unsupported");
    }
    if (mapping.buffer.has_value() != buffer || mapping.texture.has_value() != texture ||
        mapping.sampler.has_value() != sampler || (mapping.requiresByteLengths && !buffer)) {
        throw std::invalid_argument("Converted Metal shader descriptor type does not match its native slots");
    }
}

void validateTexture(id<MTLTexture> texture, const ShaderRecompiler::DescriptorBinding& descriptor,
                     std::uint32_t element) {
    if (descriptor.imageShape) {
        bool matches = false;
        switch (*descriptor.imageShape) {
        case ShaderRecompiler::DescriptorImageShape::Image1D:
            matches = texture.textureType == MTLTextureType1D;
            break;
        case ShaderRecompiler::DescriptorImageShape::Image2D:
            matches = texture.textureType == MTLTextureType2D;
            break;
        case ShaderRecompiler::DescriptorImageShape::Image2DArray:
            matches = texture.textureType == MTLTextureType2DArray;
            break;
        case ShaderRecompiler::DescriptorImageShape::ImageCube:
            matches = texture.textureType == MTLTextureTypeCube || texture.textureType == MTLTextureTypeCubeArray;
            break;
        case ShaderRecompiler::DescriptorImageShape::Image3D:
            matches = texture.textureType == MTLTextureType3D;
            break;
        }
        if (!matches) throw std::invalid_argument("Metal shader texture dimensions do not match guest descriptor metadata");
    }
    if (texture.usage != MTLTextureUsageUnknown) {
        const bool written = element < descriptor.imageWritten.size() && descriptor.imageWritten[element];
        const bool requiresRead = descriptor.kind == DescriptorKind::SampledImage ||
                                  descriptor.kind == DescriptorKind::UniformTexelBuffer || descriptor.readOnly ||
                                  (element < descriptor.imageWritten.size() && !written);
        if (requiresRead && (texture.usage & MTLTextureUsageShaderRead) == 0) {
            throw std::invalid_argument("Metal shader texture lacks shader read usage");
        }
        if (written && (texture.usage & MTLTextureUsageShaderWrite) == 0) {
            throw std::invalid_argument("Metal shader written texture lacks shader write usage");
        }
    }
}

PreparedBindings prepare(id<MTLDevice> device, const ShaderResult& shader,
                         std::span<const MetalShaderResourceBinding> supplied,
                         std::span<const std::byte> pushConstants,
                         std::span<const id<MTLResource>> indirectResources) {
    PreparedBindings result;
    std::map<std::pair<std::uint32_t, std::uint32_t>, const MetalShaderResourceBinding*> bindings;
    for (const auto& binding : supplied) {
        if (!bindings.emplace(std::pair(binding.descriptorSet, binding.binding), &binding).second) {
            throw std::invalid_argument("Metal shader descriptor binding was supplied more than once");
        }
    }
    std::set<std::uint32_t> bufferSlots;
    std::set<std::uint32_t> textureSlots;
    std::set<std::uint32_t> samplerSlots;
    if (shader.pushConstantBuffer) {
        reserveSlots(bufferSlots, *shader.pushConstantBuffer, 1, 31);
    }
    if (shader.bufferSizesBuffer) {
        reserveSlots(bufferSlots, *shader.bufferSizesBuffer, 1, 31);
    }
    for (const auto& mapping : shader.resources) {
        const auto key = std::pair(mapping.descriptorSet, mapping.binding);
        const auto found = bindings.find(key);
        if (!mapping.active) {
            if (found != bindings.end()) bindings.erase(found);
            continue;
        }
        validateKind(mapping);
        const auto descriptor = std::find_if(shader.guest.bindings.begin(), shader.guest.bindings.end(),
            [&mapping](const auto& binding) {
                return binding.descriptorSet == mapping.descriptorSet && binding.binding == mapping.binding;
            });
        if (descriptor == shader.guest.bindings.end() || descriptor->kind != mapping.kind || descriptor->count != mapping.count) {
            throw std::invalid_argument("Metal shader resource reflection disagrees with guest descriptor metadata");
        }
        if (found == bindings.end()) {
            throw std::invalid_argument("Metal shader has an unresolved active descriptor binding");
        }
        const auto& binding = *found->second;
        const std::size_t buffers = mapping.buffer ? mapping.count : 0;
        const std::size_t textures = mapping.texture ? mapping.count : 0;
        const std::size_t samplers = mapping.sampler ? mapping.count : 0;
        if (binding.buffers.size() != buffers || binding.textures.size() != textures ||
            binding.samplers.size() != samplers) {
            throw std::invalid_argument("Metal shader descriptor array count or resource type does not match reflection");
        }
        if (mapping.buffer) {
            reserveSlots(bufferSlots, *mapping.buffer, mapping.count, 31);
            for (std::uint32_t i = 0; i < mapping.count; ++i) {
                auto resource = binding.buffers[i];
                if (resource.buffer == nil || resource.buffer.device != device ||
                    resource.offset >= resource.buffer.length) {
                    throw std::invalid_argument("Metal shader buffer device or offset is invalid");
                }
                const NSUInteger remaining = resource.buffer.length - resource.offset;
                if (resource.length == 0) resource.length = remaining;
                if (resource.length > remaining) {
                    throw std::invalid_argument("Metal shader buffer range exceeds its allocation");
                }
                const std::uint32_t index = *mapping.buffer + i;
                result.buffers.push_back({index, resource});
                if (mapping.requiresByteLengths) {
                    if (!shader.bufferSizesBuffer || resource.length > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::invalid_argument("Metal shader buffer byte length cannot be represented");
                    }
                    result.lengths[index] = static_cast<std::uint32_t>(resource.length);
                }
            }
        }
        if (mapping.texture) {
            reserveSlots(textureSlots, *mapping.texture, mapping.count, 128);
            for (std::uint32_t i = 0; i < mapping.count; ++i) {
                id<MTLTexture> texture = binding.textures[i];
                if (texture == nil || texture.device != device) {
                    throw std::invalid_argument("Metal shader texture belongs to a different device or is missing");
                }
                if ((mapping.kind == DescriptorKind::UniformTexelBuffer ||
                     mapping.kind == DescriptorKind::StorageTexelBuffer) && texture.textureType != MTLTextureTypeTextureBuffer) {
                    throw std::invalid_argument("Metal shader texel buffer requires a native texture buffer");
                }
                validateTexture(texture, *descriptor, i);
                result.textures.push_back({*mapping.texture + i, texture});
            }
        }
        if (mapping.sampler) {
            reserveSlots(samplerSlots, *mapping.sampler, mapping.count, 16);
            for (std::uint32_t i = 0; i < mapping.count; ++i) {
                id<MTLSamplerState> sampler = binding.samplers[i];
                if (sampler == nil || sampler.device != device) {
                    throw std::invalid_argument("Metal shader sampler belongs to a different device or is missing");
                }
                result.samplers.push_back({*mapping.sampler + i, sampler});
            }
        }
        bindings.erase(found);
    }
    if (!bindings.empty()) {
        throw std::invalid_argument("Metal shader received a descriptor binding absent from reflection");
    }
    result.pushConstants = pushConstants.empty() ? std::span<const std::byte>(shader.pushConstantData) : pushConstants;
    if (result.pushConstants.size() > 4096 ||
        (shader.pushConstantBuffer && (shader.pushConstantBytes == 0 || result.pushConstants.size() < shader.pushConstantBytes)) ||
        (!shader.pushConstantBuffer && !pushConstants.empty())) {
        throw std::invalid_argument("Metal shader push constant override must contain the complete reflected block within 4096 bytes");
    }
    if (shader.requiresGpuAddresses && indirectResources.empty()) {
        throw std::invalid_argument("Metal shader GPU address access requires explicit resident resources");
    }
    for (id<MTLResource> resource : indirectResources) {
        if (resource == nil || resource.device != device) {
            throw std::invalid_argument("Metal shader indirect resource belongs to a different device or is missing");
        }
    }
    return result;
}

void bindCompute(id<MTLComputeCommandEncoder> encoder, const ShaderResult& shader, const PreparedBindings& bindings) {
    for (const auto& binding : bindings.buffers) {
        [encoder setBuffer:binding.resource.buffer offset:binding.resource.offset atIndex:binding.index];
    }
    for (const auto& binding : bindings.textures) [encoder setTexture:binding.texture atIndex:binding.index];
    for (const auto& binding : bindings.samplers) [encoder setSamplerState:binding.sampler atIndex:binding.index];
    if (shader.pushConstantBuffer) {
        [encoder setBytes:bindings.pushConstants.data() length:bindings.pushConstants.size() atIndex:*shader.pushConstantBuffer];
    }
    if (shader.bufferSizesBuffer) {
        [encoder setBytes:bindings.lengths.data() length:sizeof(bindings.lengths) atIndex:*shader.bufferSizesBuffer];
    }
}

void bindRender(id<MTLRenderCommandEncoder> encoder, const ShaderResult& shader,
                const PreparedBindings& bindings, bool vertex) {
    for (const auto& binding : bindings.buffers) {
        if (vertex) [encoder setVertexBuffer:binding.resource.buffer offset:binding.resource.offset atIndex:binding.index];
        else [encoder setFragmentBuffer:binding.resource.buffer offset:binding.resource.offset atIndex:binding.index];
    }
    for (const auto& binding : bindings.textures) {
        if (vertex) [encoder setVertexTexture:binding.texture atIndex:binding.index];
        else [encoder setFragmentTexture:binding.texture atIndex:binding.index];
    }
    for (const auto& binding : bindings.samplers) {
        if (vertex) [encoder setVertexSamplerState:binding.sampler atIndex:binding.index];
        else [encoder setFragmentSamplerState:binding.sampler atIndex:binding.index];
    }
    if (shader.pushConstantBuffer) {
        if (vertex) [encoder setVertexBytes:bindings.pushConstants.data() length:bindings.pushConstants.size() atIndex:*shader.pushConstantBuffer];
        else [encoder setFragmentBytes:bindings.pushConstants.data() length:bindings.pushConstants.size() atIndex:*shader.pushConstantBuffer];
    }
    if (shader.bufferSizesBuffer) {
        if (vertex) [encoder setVertexBytes:bindings.lengths.data() length:sizeof(bindings.lengths) atIndex:*shader.bufferSizesBuffer];
        else [encoder setFragmentBytes:bindings.lengths.data() length:sizeof(bindings.lengths) atIndex:*shader.bufferSizesBuffer];
    }
}

}

MetalComputePipeline::MetalComputePipeline(id<MTLDevice> device, ShaderResult shader)
    : device(device), shader(std::move(shader)) {
    id<MTLFunction> function = compile(device, this->shader, MTLFunctionTypeKernel);
    NSError* error = nil;
    pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) {
        throw std::runtime_error(errorMessage("Converted Metal compute pipeline creation failed", error));
    }
    if (this->shader.requiresSimdGroups && this->shader.guest.hostSubgroupSize != 0 &&
        this->shader.guest.hostSubgroupSize != pipeline.threadExecutionWidth) {
        throw std::invalid_argument("Converted Metal compute subgroup width differs from the guest compilation contract");
    }
    const auto& threads = this->shader.threadsPerThreadgroup;
    const MTLSize maximum = device.maxThreadsPerThreadgroup;
    if (threads[0] == 0 || threads[1] == 0 || threads[2] == 0 ||
        threads[0] > maximum.width || threads[1] > maximum.height || threads[2] > maximum.depth ||
        static_cast<std::uint64_t>(threads[0]) * threads[1] * threads[2] > pipeline.maxTotalThreadsPerThreadgroup) {
        throw std::invalid_argument("Converted Metal compute workgroup exceeds the native pipeline limit");
    }
}

const ShaderResult& MetalComputePipeline::Reflection() const {
    return shader;
}

void MetalComputePipeline::Encode(id<MTLCommandBuffer> commands,
                                  std::span<const MetalShaderResourceBinding> bindings,
                                  MTLSize grid, std::span<const std::byte> pushConstants,
                                  std::span<const id<MTLResource>> indirectResources) const {
    if (commands == nil || commands.device != device || commands.status >= MTLCommandBufferStatusCommitted) {
        throw std::invalid_argument("Converted Metal compute encoding requires an uncommitted command buffer on this device");
    }
    auto prepared = prepare(device, shader, bindings, pushConstants, indirectResources);
    if (grid.width == 0 || grid.height == 0 || grid.depth == 0) return;
    const auto& threads = shader.threadsPerThreadgroup;
    if (grid.width % threads[0] != 0 || grid.height % threads[1] != 0 || grid.depth % threads[2] != 0) {
        throw std::invalid_argument("Converted Metal compute grid must contain complete reflected workgroups");
    }
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    if (encoder == nil) throw std::runtime_error("Converted Metal compute encoder creation failed");
    [encoder setComputePipelineState:pipeline];
    bindCompute(encoder, shader, prepared);
    for (id<MTLResource> resource : indirectResources) {
        [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(grid.width / threads[0], grid.height / threads[1], grid.depth / threads[2])
             threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    [encoder endEncoding];
}

MetalRenderPipeline::MetalRenderPipeline(id<MTLDevice> device, ShaderResult vertex,
                                        ShaderResult fragment, MTLRenderPipelineDescriptor* descriptor)
    : device(device), vertex(std::move(vertex)), fragment(std::move(fragment)) {
    if (descriptor == nil) throw std::invalid_argument("Converted Metal render pipeline requires a descriptor");
    MTLRenderPipelineDescriptor* native = [descriptor copy];
    native.vertexFunction = compile(device, this->vertex, MTLFunctionTypeVertex);
    native.fragmentFunction = compile(device, this->fragment, MTLFunctionTypeFragment);
    NSError* error = nil;
    pipeline = [device newRenderPipelineStateWithDescriptor:native error:&error];
    if (pipeline == nil) {
        throw std::runtime_error(errorMessage("Converted Metal render pipeline creation failed", error));
    }
}

const ShaderResult& MetalRenderPipeline::VertexReflection() const {
    return vertex;
}

const ShaderResult& MetalRenderPipeline::FragmentReflection() const {
    return fragment;
}

void MetalRenderPipeline::Bind(id<MTLRenderCommandEncoder> encoder,
                               std::span<const MetalShaderResourceBinding> vertexBindings,
                               std::span<const MetalShaderResourceBinding> fragmentBindings,
                               std::span<const std::byte> vertexPushConstants,
                               std::span<const std::byte> fragmentPushConstants,
                               std::span<const id<MTLResource>> indirectResources) const {
    if (encoder == nil || encoder.device != device) {
        throw std::invalid_argument("Converted Metal render binding requires an encoder on this device");
    }
    auto preparedVertex = prepare(device, vertex, vertexBindings, vertexPushConstants, indirectResources);
    auto preparedFragment = prepare(device, fragment, fragmentBindings, fragmentPushConstants, indirectResources);
    [encoder setRenderPipelineState:pipeline];
    bindRender(encoder, vertex, preparedVertex, true);
    bindRender(encoder, fragment, preparedFragment, false);
    for (id<MTLResource> resource : indirectResources) {
        [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex | MTLRenderStageFragment];
    }
}

}
