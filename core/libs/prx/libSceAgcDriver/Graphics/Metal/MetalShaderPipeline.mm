#include "MetalShaderPipeline.hpp"
#include <algorithm>
#include "MetalSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include <cmath>
#include <tuple>
#import <objc/runtime.h>
#include <array>
#include <cstring>
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
using RectMode = ShaderRecompiler::MetalBackend::RectListMode;
using ImplicitRole = ShaderRecompiler::MetalBackend::ImplicitBufferRole;

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
                       type == MTLFunctionTypeMesh ? ShaderRecompiler::ShaderStage::Mesh :
                                                      ShaderRecompiler::ShaderStage::Fragment;
    const bool capture = type == MTLFunctionTypeKernel && shader.rectList &&
        ((shader.rectList->mode == RectMode::VertexCapture && shader.stage == ShaderRecompiler::ShaderStage::Vertex) ||
         (shader.rectList->mode == RectMode::Control && shader.stage == ShaderRecompiler::ShaderStage::TessellationControl));
    const bool evaluation = type == MTLFunctionTypeVertex && shader.rectList &&
        shader.rectList->mode == RectMode::Evaluation && shader.stage == ShaderRecompiler::ShaderStage::TessellationEvaluation;
    if ((shader.rectList && !(capture || evaluation)) ||
        (capture && shader.nativeExecutionKind != ShaderRecompiler::MetalBackend::NativeExecutionKind::Compute) ||
        (evaluation && shader.nativeExecutionKind != ShaderRecompiler::MetalBackend::NativeExecutionKind::Vertex) ||
        (shader.stage != stage && !capture && !evaluation)) {
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
        if (shader.requiresWorkgroupAtomicFences) {
            if (![device supportsFamily:MTLGPUFamilyApple1])
                throw std::invalid_argument("Workgroup acquire/release fences require an Apple Silicon device");
            options.languageVersion = MTLLanguageVersion3_2;
        }
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



std::uint32_t samplerArgumentCapacity(id<MTLDevice> device) {
    if ([device supportsFamily:MTLGPUFamilyApple10] || [device supportsFamily:MTLGPUFamilyApple9]) return 500000;
    if ([device supportsFamily:MTLGPUFamilyApple8] || [device supportsFamily:MTLGPUFamilyApple7]) return 996;
    if ([device supportsFamily:MTLGPUFamilyApple6]) return 128;
    return 16;
}

const ShaderRecompiler::DescriptorBinding& descriptorElement(const ShaderResult& shader, std::uint32_t set,
    std::uint32_t binding, std::uint32_t element, DescriptorKind kind, ShaderRecompiler::DescriptorRole role,
    std::uint32_t words) {
    const auto found = std::find_if(shader.guest.bindings.begin(), shader.guest.bindings.end(), [&](const auto& descriptor) {
        return descriptor.descriptorSet == set && descriptor.binding == binding;
    });
    if (found == shader.guest.bindings.end() || found->kind != kind || found->role != role || element >= found->count ||
        found->guestDescriptor.size() != static_cast<std::size_t>(found->count) * words) {
        throw std::invalid_argument("Metal minimum LOD recipe descriptor identity or captured words are invalid");
    }
    const auto mapping = std::find_if(shader.resources.begin(), shader.resources.end(), [&](const auto& value) {
        return value.descriptorSet == set && value.binding == binding;
    });
    if (mapping == shader.resources.end() || !mapping->active || mapping->kind != kind || mapping->count != found->count ||
        (kind == DescriptorKind::SampledImage && !mapping->texture) || (kind == DescriptorKind::Sampler && !mapping->sampler)) {
        throw std::invalid_argument("Metal minimum LOD recipe does not reference an active native resource mapping");
    }
    return *found;
}

auto samplerConfiguration(const Graphics::GuestSamplerResource& value) {
    return std::tuple(value.magFilter, value.minFilter, value.mipmapMode, value.addressModeU, value.addressModeV,
        value.addressModeW, value.anisotropyEnable, value.maxAnisotropy, value.minLod, value.maxLod, value.lodBias,
        value.borderColor, value.compareEnable, value.compareOp);
}

char samplerStatesAssociation;

id<MTLFunction> samplerBankFunction(id<MTLDevice> device, const ShaderResult& shader, id<MTLFunction> function) {
    if (shader.requiresTextureLodQueries && !device.supportsQueryTextureLOD) {
        throw std::invalid_argument("Metal minimum LOD shadow selection requires native texture LOD queries");
    }
    if (!shader.samplerArgumentBuffer) {
        if (shader.samplerArgumentCount != 0) throw std::invalid_argument("Metal direct sampler reflection contains a bank count");
        return nil;
    }
    if (device.argumentBuffersSupport != MTLArgumentBuffersTier2 || *shader.samplerArgumentBuffer >= 31 ||
        shader.samplerArgumentCount == 0 || shader.samplerArgumentCount > samplerArgumentCapacity(device)) {
        throw std::invalid_argument("Metal sampler bank exceeds its native per-stage argument capacity or lacks Tier 2");
    }
    auto encoder = [function newArgumentEncoderWithBufferIndex:*shader.samplerArgumentBuffer];
    if (encoder == nil || encoder.encodedLength == 0 || encoder.alignment == 0 || encoder.device != device) {
        throw std::runtime_error("Metal sampler bank is missing native argument encoder reflection");
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

std::array<NSUInteger, 31> nativeBufferAlignments(NSArray<id<MTLBinding>>* bindings) {
    if (bindings == nil) {
        throw std::runtime_error("Metal pipeline did not return native buffer binding reflection");
    }
    std::array<NSUInteger, 31> result{};
    for (id<MTLBinding> binding in bindings) {
        if (binding.type != MTLBindingTypeBuffer) continue;
        id<MTLBufferBinding> buffer = static_cast<id<MTLBufferBinding>>(binding);
        if (buffer.index >= result.size() || buffer.bufferAlignment == 0 || result[buffer.index] != 0) {
            throw std::runtime_error("Metal pipeline returned invalid native buffer alignment reflection");
        }
        result[buffer.index] = buffer.bufferAlignment;
    }
    return result;
}

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
            matches = texture.textureType == MTLTextureType2D && texture.height == 1;
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
                         const std::array<NSUInteger, 31>& alignments, id<MTLFunction> samplerFunction,
                         std::span<const MetalShaderResourceBinding> supplied,
                         std::span<const std::byte> pushConstants,
                         std::span<const id<MTLResource>> indirectResources) {
    ValidatePixelSamplerBindings(shader.guest.bindings);
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
    id<MTLArgumentEncoder> samplerEncoder = nil;
    id<MTLBuffer> samplerBuffer = nil;
    auto samplerOwners = [NSMutableArray new];
    if (shader.samplerArgumentBuffer) {
        if (samplerFunction == nil) throw std::runtime_error("Metal sampler bank native function is missing");
        reserveSlots(bufferSlots, *shader.samplerArgumentBuffer, 1, 31);
        samplerEncoder = [samplerFunction newArgumentEncoderWithBufferIndex:*shader.samplerArgumentBuffer];
        if (samplerEncoder == nil || samplerEncoder.encodedLength == 0 || samplerEncoder.alignment == 0 || samplerEncoder.device != device) {
            throw std::runtime_error("Metal sampler bank argument encoder allocation failed");
        }
        samplerBuffer = [device newBufferWithLength:samplerEncoder.encodedLength options:MTLResourceStorageModeShared];
        if (samplerBuffer == nil || alignments[*shader.samplerArgumentBuffer] == 0) {
            throw std::runtime_error("Metal sampler bank allocation or binding alignment reflection is missing");
        }
        [samplerEncoder setArgumentBuffer:samplerBuffer offset:0];
        result.buffers.push_back({*shader.samplerArgumentBuffer, {samplerBuffer, 0, samplerBuffer.length}});
    } else if (samplerFunction != nil || shader.samplerArgumentCount != 0) {
        throw std::invalid_argument("Metal direct sampler reflection contains a native bank function");
    }
    if (shader.vertexBufferCount != 0) {
        if (shader.stage != ShaderRecompiler::ShaderStage::Vertex) {
            throw std::invalid_argument("Converted Metal shader reserves vertex input slots outside the vertex stage");
        }
        reserveSlots(bufferSlots, 0, shader.vertexBufferCount, 31);
    }
    if (shader.pushConstantBuffer) {
        reserveSlots(bufferSlots, *shader.pushConstantBuffer, 1, 31);
    }
    if (shader.bufferSizesBuffer) {
        reserveSlots(bufferSlots, *shader.bufferSizesBuffer, 1, 31);
    }
    if (shader.rectList) for (const auto& mapping : shader.rectList->buffers) {
        reserveSlots(bufferSlots, mapping.index, 1, 31);
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
                const NSUInteger length = resource.length.value_or(remaining);
                resource.length = length;
                if (length > remaining) {
                    throw std::invalid_argument("Metal shader buffer range exceeds its allocation");
                }
                const std::uint32_t index = *mapping.buffer + i;
                if (alignments[index] == 0) {
                    throw std::runtime_error("Metal shader buffer slot is missing native alignment reflection");
                }
                if (resource.offset % alignments[index] != 0) {
                    throw std::invalid_argument("Metal shader buffer offset violates its native alignment");
                }
                result.buffers.push_back({index, resource});
                if (mapping.requiresByteLengths) {
                    if (!shader.bufferSizesBuffer || length > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::invalid_argument("Metal shader buffer byte length cannot be represented");
                    }
                    result.lengths[index] = static_cast<std::uint32_t>(length);
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
                if (!descriptor->imageUnnormalized.empty() && descriptor->imageUnnormalized[i]) {
                    ValidatePixelSampledView(texture,
                        Graphics::DecodeTextureResource(std::span(descriptor->guestDescriptor).subspan(i * 8u, 8)));
                }
                result.textures.push_back({*mapping.texture + i, texture});
            }
        }
        if (mapping.sampler) {
            reserveSlots(samplerSlots, *mapping.sampler, mapping.count, shader.samplerArgumentBuffer ? shader.samplerArgumentCount : 16);
            for (std::uint32_t i = 0; i < mapping.count; ++i) {
                id<MTLSamplerState> sampler = binding.samplers[i];
                if (sampler == nil || sampler.device != device) {
                    throw std::invalid_argument("Metal shader sampler belongs to a different device or is missing");
                }
                if (!descriptor->samplerUnnormalized.empty() && descriptor->samplerUnnormalized[i]) {
                    const bool compare = i < descriptor->samplerDepthCompare.size() && descriptor->samplerDepthCompare[i];
                    if (!MetalSampler::MatchesCapturedDescriptor(sampler,
                        std::span(descriptor->guestDescriptor).subspan(i * 4u, 4), compare, true)) {
                        throw std::invalid_argument("Metal pixel sampler native identity differs from the captured qualified descriptor");
                    }
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
    for (const auto& requirement : shader.capturedSamplerRequirements) {
        descriptorElement(shader, requirement.descriptorSet, requirement.binding, requirement.element,
            DescriptorKind::Sampler, ShaderRecompiler::DescriptorRole::GuestSamplers, 4);
        const auto native = std::find_if(supplied.begin(), supplied.end(), [&](const auto& binding) {
            return binding.descriptorSet == requirement.descriptorSet && binding.binding == requirement.binding;
        });
        if (native == supplied.end() || requirement.element >= native->samplers.size() ||
            !native->samplersMatchCapturedDescriptors) {
            throw std::invalid_argument("Metal captured sampler lowering requires explicit native sampler agreement with captured guest descriptors");
        }
    }
    using Configuration = decltype(samplerConfiguration(Graphics::GuestSamplerResource{}));
    std::set<Configuration> uniqueConfigurations;
    for (const auto& pair : shader.minimumLodPairs) {
        if (!shader.samplerArgumentBuffer) throw std::invalid_argument("Metal minimum LOD recipe requires an argument bank");
        const auto& image = descriptorElement(shader, pair.imageSet, pair.imageBinding, pair.imageElement,
            DescriptorKind::SampledImage, ShaderRecompiler::DescriptorRole::GuestImages, 8);
        const auto& sampler = descriptorElement(shader, pair.samplerSet, pair.samplerBinding, pair.samplerElement,
            DescriptorKind::Sampler, ShaderRecompiler::DescriptorRole::GuestSamplers, 4);
        const auto native = std::find_if(supplied.begin(), supplied.end(), [&](const auto& binding) {
            return binding.descriptorSet == pair.samplerSet && binding.binding == pair.samplerBinding;
        });
        if (native == supplied.end() || !native->samplersMatchCapturedDescriptors) {
            throw std::invalid_argument("Metal minimum LOD shadows require explicit native sampler agreement with captured guest descriptors");
        }
        if (!sampler.samplerDepthCompare.empty() && sampler.samplerDepthCompare.size() != sampler.count) {
            throw std::invalid_argument("Metal minimum LOD sampler comparison metadata has the wrong descriptor count");
        }
        const auto imageResource = Graphics::DecodeTextureResource(std::span(image.guestDescriptor).subspan(pair.imageElement * 8u, 8));
        const float relative = std::max(0.0f, Graphics::EffectiveMinLod(imageResource) - static_cast<float>(imageResource.baseLevel));
        if (!std::isfinite(pair.relativeViewMin) || pair.relativeViewMin < 0.0f || pair.relativeViewMin != relative) {
            throw std::invalid_argument("Metal minimum LOD recipe differs from its captured image view");
        }
        auto original = Graphics::DecodeSamplerResource(std::span(sampler.guestDescriptor).subspan(pair.samplerElement * 4u, 4));
        original.compareEnable = !sampler.samplerDepthCompare.empty() && sampler.samplerDepthCompare[pair.samplerElement];
        uniqueConfigurations.insert(samplerConfiguration(original));
        auto mag = original;
        mag.minLod = std::max(original.minLod, relative);
        mag.maxLod = std::max(original.maxLod, relative);
        mag.minFilter = mag.magFilter = original.magFilter;
        auto min = mag;
        min.minFilter = min.magFilter = original.minFilter;
        reserveSlots(samplerSlots, pair.magArgument, 1, shader.samplerArgumentCount);
        reserveSlots(samplerSlots, pair.minArgument, 1, shader.samplerArgumentCount);
        uniqueConfigurations.insert(samplerConfiguration(mag));
        uniqueConfigurations.insert(samplerConfiguration(min));
        if (uniqueConfigurations.size() > device.maxArgumentBufferSamplerCount) {
            throw std::invalid_argument("Metal minimum LOD sampler configurations exceed the separate per-app native budget");
        }
        MetalSampler magSampler(device, mag), minSampler(device, min);
        result.samplers.push_back({pair.magArgument, magSampler.Handle()});
        result.samplers.push_back({pair.minArgument, minSampler.Handle()});
    }
    if (samplerBuffer != nil) {
        if (samplerSlots.size() != shader.samplerArgumentCount || result.samplers.size() != shader.samplerArgumentCount) {
            throw std::invalid_argument("Metal sampler bank argument IDs are incomplete");
        }
        for (const auto& binding : result.samplers) {
            [samplerEncoder setSamplerState:binding.sampler atIndex:binding.index];
            [samplerOwners addObject:binding.sampler];
        }
        objc_setAssociatedObject(samplerBuffer, &samplerStatesAssociation, [samplerOwners copy], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        result.samplers.clear();
    }
    return result;
}

std::size_t checkedBytes(std::size_t count, std::size_t stride) {
    if (stride == 0 || count > std::numeric_limits<std::size_t>::max() / stride) {
        throw std::invalid_argument("Metal rectangle intermediate byte count is invalid");
    }
    return count * stride;
}

void validateLayout(const ShaderRecompiler::MetalBackend::InterfaceLayout& layout) {
    if (layout.stride == 0 || layout.alignment == 0 || (layout.alignment & (layout.alignment - 1u)) != 0 ||
        layout.stride % layout.alignment != 0 || layout.members.empty()) {
        throw std::invalid_argument("Metal rectangle interface layout is invalid");
    }
    for (const auto& member : layout.members) {
        if (member.bytes == 0 || member.offset > layout.stride || member.bytes > layout.stride - member.offset) {
            throw std::invalid_argument("Metal rectangle interface member exceeds its record");
        }
    }
}

std::vector<BufferArgument> prepareImplicit(id<MTLDevice> device, const ShaderResult& shader,
    const std::array<NSUInteger, 31>& alignments, std::span<const MetalImplicitBufferBinding> supplied,
    std::size_t vertices, std::size_t patches, std::size_t indices = 0) {
    if (!shader.rectList || supplied.size() != shader.rectList->buffers.size()) {
        throw std::invalid_argument("Metal rectangle implicit buffer count differs from reflection");
    }
    std::map<std::uint32_t, MetalBufferBinding> bindings;
    for (const auto& binding : supplied) if (!bindings.emplace(binding.index, binding.buffer).second) {
        throw std::invalid_argument("Metal rectangle implicit buffer was supplied more than once");
    }
    const auto& rect = *shader.rectList;
    std::vector<BufferArgument> result;
    for (const auto& mapping : rect.buffers) {
        const auto found = bindings.find(mapping.index);
        if (found == bindings.end() || mapping.index >= alignments.size() || alignments[mapping.index] == 0) {
            throw std::invalid_argument("Metal rectangle implicit slot lacks a binding or native alignment");
        }
        auto buffer = found->second;
        if (buffer.buffer == nil || buffer.buffer.device != device || buffer.offset >= buffer.buffer.length ||
            buffer.offset % alignments[mapping.index] != 0) {
            throw std::invalid_argument("Metal rectangle implicit buffer device, offset or alignment is invalid");
        }
        const auto available = buffer.buffer.length - buffer.offset;
        const auto length = buffer.length.value_or(available);
        if (length == 0 || length > available) throw std::invalid_argument("Metal rectangle implicit buffer extent is invalid");
        buffer.length = length;
        std::size_t count = 0;
        std::size_t stride = mapping.elementBytes;
        switch (mapping.role) {
        case ImplicitRole::StageOutput:
            validateLayout(rect.outputLayout);
            if (stride != rect.outputLayout.stride || buffer.offset % rect.outputLayout.alignment != 0) {
                throw std::invalid_argument("Metal rectangle output buffer differs from its typed layout");
            }
            count = vertices;
            break;
        case ImplicitRole::StageInput:
            validateLayout(rect.inputLayout);
            if (stride != rect.inputLayout.stride || buffer.offset % rect.inputLayout.alignment != 0) {
                throw std::invalid_argument("Metal rectangle input buffer differs from its typed layout");
            }
            count = checkedBytes(patches, rect.mode == RectMode::Control ? rect.inputControlPoints : rect.outputControlPoints);
            break;
        case ImplicitRole::TessellationFactors:
            if (stride != sizeof(MTLQuadTessellationFactorsHalf)) throw std::invalid_argument("Metal rectangle tessellation factor layout is invalid");
            count = patches;
            break;
        case ImplicitRole::IndirectParameters:
            if (stride != 2 * sizeof(std::uint32_t) || length < stride || buffer.buffer.contents == nullptr ||
                patches > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("Metal rectangle control parameters are invalid");
            }
            {
                std::array<std::uint32_t, 2> parameters;
                std::memcpy(parameters.data(), static_cast<const std::byte*>(buffer.buffer.contents) + buffer.offset, sizeof(parameters));
                if (parameters[0] != rect.inputControlPoints || parameters[1] != patches) {
                    throw std::invalid_argument("Metal rectangle control parameters differ from its dispatch");
                }
            }
            count = 1;
            break;
        case ImplicitRole::Indices:
            if ((rect.indexSize != 2 && rect.indexSize != 4) || stride != rect.indexSize) {
                throw std::invalid_argument("Metal rectangle capture index layout is invalid");
            }
            count = indices;
            break;
        }
        if (length < checkedBytes(count, stride)) throw std::invalid_argument("Metal rectangle implicit buffer is smaller than its invocation range");
        result.push_back({mapping.index, buffer});
        bindings.erase(found);
    }
    if (!bindings.empty()) throw std::invalid_argument("Metal rectangle received an unknown implicit buffer");
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

void bindMesh(id<MTLRenderCommandEncoder> encoder, const ShaderResult& shader,
              const PreparedBindings& bindings) {
    for (const auto& binding : bindings.buffers) {
        [encoder setMeshBuffer:binding.resource.buffer offset:binding.resource.offset atIndex:binding.index];
    }
    for (const auto& binding : bindings.textures) [encoder setMeshTexture:binding.texture atIndex:binding.index];
    for (const auto& binding : bindings.samplers) [encoder setMeshSamplerState:binding.sampler atIndex:binding.index];
    if (shader.pushConstantBuffer) {
        [encoder setMeshBytes:bindings.pushConstants.data() length:bindings.pushConstants.size() atIndex:*shader.pushConstantBuffer];
    }
    if (shader.bufferSizesBuffer) {
        [encoder setMeshBytes:bindings.lengths.data() length:sizeof(bindings.lengths) atIndex:*shader.bufferSizesBuffer];
    }
}

}

MetalRectKernelPipeline::MetalRectKernelPipeline(id<MTLDevice> device, ShaderResult shader,
                                                MTLStageInputOutputDescriptor* captureInputs)
    : device(device), shader(std::move(shader)) {
    if (!this->shader.rectList ||
        (this->shader.rectList->mode != RectMode::VertexCapture && this->shader.rectList->mode != RectMode::Control) ||
        this->shader.nativeExecutionKind != ShaderRecompiler::MetalBackend::NativeExecutionKind::Compute ||
        this->shader.rectList->inputControlPoints != 3 || this->shader.rectList->outputControlPoints != 4) {
        throw std::invalid_argument("Metal rectangle kernel requires typed capture or generated control reflection");
    }
    const bool capture = this->shader.rectList->mode == RectMode::VertexCapture;
    if ((!capture && captureInputs != nil) || (capture && this->shader.vertexBufferCount != 0 && captureInputs == nil)) {
        throw std::invalid_argument("Metal rectangle kernel stage input descriptor differs from its capture contract");
    }
    if (captureInputs != nil) {
        for (std::size_t i = 0; i < this->shader.guest.vertexAttributes.size(); ++i) {
            const auto attribute = captureInputs.attributes[this->shader.guest.vertexAttributes[i].location];
            if (attribute.format == MTLAttributeFormatInvalid || attribute.bufferIndex != i ||
                attribute.bufferIndex >= this->shader.vertexBufferCount) {
                throw std::invalid_argument("Metal rectangle native attributes differ from guest capture metadata");
            }
        }
        if (this->shader.rectList->indexSize != 0 && this->shader.vertexBufferCount != 0) {
            const auto& mappings = this->shader.rectList->buffers;
            const auto indices = std::find_if(mappings.begin(), mappings.end(), [](const auto& mapping) { return mapping.role == ImplicitRole::Indices; });
            if (indices == mappings.end() || captureInputs.indexBufferIndex != indices->index ||
                captureInputs.indexType != (this->shader.rectList->indexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)) {
                throw std::invalid_argument("Metal rectangle native stage input indexing differs from its capture contract");
            }
        }
    }
    auto descriptor = [MTLComputePipelineDescriptor new];
    descriptor.computeFunction = compile(device, this->shader, MTLFunctionTypeKernel);
    samplerFunction = samplerBankFunction(device, this->shader, descriptor.computeFunction);
    descriptor.stageInputDescriptor = captureInputs;
    NSError* error = nil;
    MTLComputePipelineReflection* reflection = nil;
    pipeline = [device newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionBindingInfo reflection:&reflection error:&error];
    if (pipeline == nil) throw std::runtime_error(errorMessage("Converted Metal rectangle kernel creation failed", error));
    if (reflection == nil) throw std::runtime_error("Metal rectangle kernel did not return native binding reflection");
    bufferAlignments = nativeBufferAlignments(reflection.bindings);
    if (pipeline.threadExecutionWidth == 0 || pipeline.threadExecutionWidth > pipeline.maxTotalThreadsPerThreadgroup ||
        pipeline.threadExecutionWidth > device.maxThreadsPerThreadgroup.width) {
        throw std::invalid_argument("Metal rectangle kernel has an invalid native threadgroup width");
    }
    if (this->shader.requiresSimdGroups && this->shader.guest.hostSubgroupSize != pipeline.threadExecutionWidth) {
        throw std::invalid_argument("Metal rectangle kernel subgroup width differs from the guest compilation contract");
    }
}

const ShaderResult& MetalRectKernelPipeline::Reflection() const {
    return shader;
}

void MetalRectKernelPipeline::Encode(id<MTLCommandBuffer> commands, std::span<const MetalShaderResourceBinding> bindings,
    MTLRegion region, std::span<const MetalImplicitBufferBinding> implicitBuffers,
    std::span<const MetalBufferBinding> vertexBuffers, std::span<const std::byte> pushConstants,
    std::span<const id<MTLResource>> indirectResources) const {
    if (commands == nil || commands.device != device || commands.status >= MTLCommandBufferStatusCommitted ||
        region.size.width == 0 || region.size.height == 0 || region.size.depth != 1 || region.origin.z != 0) {
        throw std::invalid_argument("Metal rectangle kernel command buffer or invocation region is invalid");
    }
    const bool capture = shader.rectList->mode == RectMode::VertexCapture;
    if (!capture && (region.origin.x != 0 || region.origin.y != 0 || region.size.height != 1 || region.size.width % 4 != 0)) {
        throw std::invalid_argument("Metal rectangle control requires an exact flattened four-invocation patch grid");
    }
    if (vertexBuffers.size() != shader.vertexBufferCount) throw std::invalid_argument("Metal rectangle capture vertex buffer count differs from reflection");
    auto prepared = prepare(device, shader, bufferAlignments, samplerFunction, bindings, pushConstants, indirectResources);
    const auto vertices = checkedBytes(region.size.width, region.size.height);
    const auto patches = capture ? 0 : region.size.width / 4;
    auto implicit = prepareImplicit(device, shader, bufferAlignments, implicitBuffers, vertices, patches, capture ? region.size.width : 0);
    for (std::size_t i = 0; i < vertexBuffers.size(); ++i) {
        const auto& buffer = vertexBuffers[i];
        if (buffer.buffer == nil || buffer.buffer.device != device || buffer.offset >= buffer.buffer.length ||
            bufferAlignments[i] == 0 || buffer.offset % bufferAlignments[i] != 0 ||
            buffer.length.value_or(buffer.buffer.length - buffer.offset) > buffer.buffer.length - buffer.offset) {
            throw std::invalid_argument("Metal rectangle native vertex buffer device, range or alignment is invalid");
        }
    }
    auto encoder = [commands computeCommandEncoder];
    if (encoder == nil) throw std::runtime_error("Metal rectangle compute encoder allocation failed");
    [encoder setComputePipelineState:pipeline];
    if (capture) [encoder setStageInRegion:region];
    bindCompute(encoder, shader, prepared);
    for (const auto& binding : implicit) [encoder setBuffer:binding.resource.buffer offset:binding.resource.offset atIndex:binding.index];
    for (std::size_t i = 0; i < vertexBuffers.size(); ++i) [encoder setBuffer:vertexBuffers[i].buffer offset:vertexBuffers[i].offset atIndex:i];
    for (id<MTLResource> resource : indirectResources) [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    [encoder dispatchThreads:region.size threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding];
}

MetalComputePipeline::MetalComputePipeline(id<MTLDevice> device, ShaderResult shader)
    : device(device), shader(std::move(shader)) {
    if (this->shader.rectList) throw std::invalid_argument("Ordinary Metal compute pipelines do not accept rectangle kernels");
    id<MTLFunction> function = compile(device, this->shader, MTLFunctionTypeKernel);
    samplerFunction = samplerBankFunction(device, this->shader, function);
    NSError* error = nil;
    MTLComputePipelineReflection* reflection = nil;
    pipeline = [device newComputePipelineStateWithFunction:function options:MTLPipelineOptionBindingInfo
                                               reflection:&reflection error:&error];
    if (pipeline == nil) {
        throw std::runtime_error(errorMessage("Converted Metal compute pipeline creation failed", error));
    }
    if (reflection == nil) throw std::runtime_error("Metal compute pipeline did not return native binding reflection");
    bufferAlignments = nativeBufferAlignments(reflection.bindings);
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
    auto prepared = prepare(device, shader, bufferAlignments, samplerFunction, bindings, pushConstants, indirectResources);
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
    if (this->vertex.rectList) {
        const auto& rect = *this->vertex.rectList;
        bool hasAttributes = false;
        for (NSUInteger i = 0; i < 31; ++i) {
            hasAttributes |= descriptor.vertexDescriptor.attributes[i].format != MTLVertexFormatInvalid;
        }
        if (rect.mode != RectMode::Evaluation || rect.inputControlPoints != 3 || rect.outputControlPoints != 4 ||
            hasAttributes || descriptor.rasterSampleCount != 1 ||
            descriptor.tessellationPartitionMode != MTLTessellationPartitionModePow2 || descriptor.maxTessellationFactor != 64 ||
            descriptor.tessellationFactorFormat != MTLTessellationFactorFormatHalf ||
            descriptor.tessellationFactorStepFunction != MTLTessellationFactorStepFunctionPerPatch ||
            descriptor.tessellationControlPointIndexType != MTLTessellationControlPointIndexTypeNone ||
            descriptor.tessellationFactorScaleEnabled || descriptor.tessellationOutputWindingOrder != MTLWindingClockwise) {
            throw std::invalid_argument("Metal rectangle evaluation descriptor differs from its generated quad patch contract");
        }
        validateLayout(rect.inputLayout);
    }
    if (this->fragment.rectList) throw std::invalid_argument("Metal fragment pipelines do not accept rectangle auxiliary reflection");
    MTLRenderPipelineDescriptor* native = [descriptor copy];
    native.vertexFunction = compile(device, this->vertex, MTLFunctionTypeVertex);
    vertexSamplerFunction = samplerBankFunction(device, this->vertex, native.vertexFunction);
    native.fragmentFunction = compile(device, this->fragment, MTLFunctionTypeFragment);
    fragmentSamplerFunction = samplerBankFunction(device, this->fragment, native.fragmentFunction);
    NSError* error = nil;
    MTLRenderPipelineReflection* reflection = nil;
    pipeline = [device newRenderPipelineStateWithDescriptor:native options:MTLPipelineOptionBindingInfo
                                                reflection:&reflection error:&error];
    if (pipeline == nil) {
        throw std::runtime_error(errorMessage("Converted Metal render pipeline creation failed", error));
    }
    if (reflection == nil) throw std::runtime_error("Metal render pipeline did not return native binding reflection");
    vertexBufferAlignments = nativeBufferAlignments(reflection.vertexBindings);
    fragmentBufferAlignments = nativeBufferAlignments(reflection.fragmentBindings);
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
                               std::span<const id<MTLResource>> indirectResources,
                               std::span<const MetalImplicitBufferBinding> implicitBuffers,
                               std::size_t rectPatchCount) const {
    if (encoder == nil || encoder.device != device) {
        throw std::invalid_argument("Converted Metal render binding requires an encoder on this device");
    }
    auto preparedVertex = prepare(device, vertex, vertexBufferAlignments, vertexSamplerFunction, vertexBindings, vertexPushConstants, indirectResources);
    auto preparedFragment = prepare(device, fragment, fragmentBufferAlignments, fragmentSamplerFunction, fragmentBindings, fragmentPushConstants, indirectResources);
    std::vector<BufferArgument> implicit;
    if (vertex.rectList) {
        if (vertex.rectList->mode != RectMode::Evaluation || rectPatchCount == 0) {
            throw std::invalid_argument("Metal rectangle evaluation requires a positive patch count");
        }
        implicit = prepareImplicit(device, vertex, vertexBufferAlignments, implicitBuffers, 0, rectPatchCount);
    } else if (!implicitBuffers.empty() || rectPatchCount != 0) {
        throw std::invalid_argument("Ordinary Metal render pipelines do not accept rectangle intermediate buffers");
    }
    [encoder setRenderPipelineState:pipeline];
    bindRender(encoder, vertex, preparedVertex, true);
    for (const auto& binding : implicit) [encoder setVertexBuffer:binding.resource.buffer offset:binding.resource.offset atIndex:binding.index];
    bindRender(encoder, fragment, preparedFragment, false);
    for (id<MTLResource> resource : indirectResources) {
        [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex | MTLRenderStageFragment];
    }
}

MetalMeshPipeline::MetalMeshPipeline(id<MTLDevice> device, ShaderResult mesh,
                                    ShaderResult fragment, MTLMeshRenderPipelineDescriptor* descriptor)
    : device(device), mesh(std::move(mesh)), fragment(std::move(fragment)) {
    if (descriptor == nil || device == nil) throw std::invalid_argument("Converted Metal mesh pipeline requires a device and descriptor");
    if (![device supportsFamily:MTLGPUFamilyApple7] || ![device supportsFamily:MTLGPUFamilyMetal3]) {
        throw std::invalid_argument("Converted Metal mesh pipeline requires an Apple GPU with Metal 3 mesh support");
    }
    if (!this->mesh.mesh || this->mesh.mesh->maxVertices == 0 || this->mesh.mesh->maxVertices > 256 ||
        this->mesh.mesh->maxPrimitives == 0 || this->mesh.mesh->maxPrimitives > 512) {
        throw std::invalid_argument("Converted Metal mesh output exceeds native limits");
    }
    const auto& threads = this->mesh.threadsPerThreadgroup;
    const auto maximum = device.maxThreadsPerThreadgroup;
    if (threads[0] == 0 || threads[1] == 0 || threads[2] == 0 ||
        threads[0] > maximum.width || threads[1] > maximum.height || threads[2] > maximum.depth) {
        throw std::invalid_argument("Converted Metal mesh workgroup exceeds the native device limit");
    }
    const std::uint64_t count = std::uint64_t{threads[0]} * threads[1] * threads[2];
    if (count > maximum.width || count > std::numeric_limits<NSUInteger>::max()) {
        throw std::invalid_argument("Converted Metal mesh workgroup exceeds the native device limit");
    }
    MTLMeshRenderPipelineDescriptor* native = [descriptor copy];
    if (native.objectFunction != nil || native.payloadMemoryLength != 0) {
        throw std::invalid_argument("Converted Metal mesh pipeline does not support object shader scheduling");
    }
    native.meshFunction = compile(device, this->mesh, MTLFunctionTypeMesh);
    meshSamplerFunction = samplerBankFunction(device, this->mesh, native.meshFunction);
    native.fragmentFunction = compile(device, this->fragment, MTLFunctionTypeFragment);
    fragmentSamplerFunction = samplerBankFunction(device, this->fragment, native.fragmentFunction);
    native.maxTotalThreadsPerMeshThreadgroup = static_cast<NSUInteger>(count);
    NSError* error = nil;
    MTLRenderPipelineReflection* reflection = nil;
    pipeline = [device newRenderPipelineStateWithMeshDescriptor:native options:MTLPipelineOptionBindingInfo
                                                   reflection:&reflection error:&error];
    if (pipeline == nil) throw std::runtime_error(errorMessage("Converted Metal mesh pipeline creation failed", error));
    if (reflection == nil) throw std::runtime_error("Metal mesh pipeline did not return native binding reflection");
    meshBufferAlignments = nativeBufferAlignments(reflection.meshBindings);
    fragmentBufferAlignments = nativeBufferAlignments(reflection.fragmentBindings);
    if (this->mesh.requiresSimdGroups && this->mesh.guest.hostSubgroupSize != pipeline.meshThreadExecutionWidth) {
        throw std::invalid_argument("Converted Metal mesh subgroup width differs from the guest compilation contract");
    }
    if (count > pipeline.maxTotalThreadsPerMeshThreadgroup || pipeline.maxTotalThreadgroupsPerMeshGrid == 0) {
        throw std::invalid_argument("Converted Metal mesh workgroup exceeds the native pipeline limit");
    }
}

const ShaderResult& MetalMeshPipeline::MeshReflection() const {
    return mesh;
}

const ShaderResult& MetalMeshPipeline::FragmentReflection() const {
    return fragment;
}

void MetalMeshPipeline::ValidateThreadgroups(MTLSize groups) const {
    const auto maximum = pipeline.maxTotalThreadgroupsPerMeshGrid;
    if (groups.width == 0 || groups.height == 0 || groups.depth == 0 || groups.width > maximum ||
        groups.height > maximum / groups.width || groups.depth > maximum / groups.width / groups.height) {
        throw std::invalid_argument("Converted Metal mesh grid exceeds the native pipeline limit");
    }
}

void MetalMeshPipeline::Bind(id<MTLRenderCommandEncoder> encoder,
                            std::span<const MetalShaderResourceBinding> meshBindings,
                            std::span<const MetalShaderResourceBinding> fragmentBindings,
                            std::span<const std::byte> meshPushConstants,
                            std::span<const std::byte> fragmentPushConstants,
                            std::span<const id<MTLResource>> indirectResources) const {
    if (encoder == nil || encoder.device != device) {
        throw std::invalid_argument("Converted Metal mesh binding requires an encoder on this device");
    }
    auto preparedMesh = prepare(device, mesh, meshBufferAlignments, meshSamplerFunction, meshBindings, meshPushConstants, indirectResources);
    auto preparedFragment = prepare(device, fragment, fragmentBufferAlignments, fragmentSamplerFunction, fragmentBindings, fragmentPushConstants, indirectResources);
    [encoder setRenderPipelineState:pipeline];
    bindMesh(encoder, mesh, preparedMesh);
    bindRender(encoder, fragment, preparedFragment, false);
    for (id<MTLResource> resource : indirectResources) {
        [encoder useResource:resource usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageMesh | MTLRenderStageFragment];
    }
}

}
