#pragma once

#include "../Recompiler.hpp"
#include <array>
#include <optional>
#include <string>

namespace ShaderRecompiler::MetalBackend {

enum class NativeExecutionKind { Compute, Vertex, Fragment, Mesh };
enum class RectListMode { None, VertexCapture, Control, Evaluation };
enum class ImplicitBufferRole { StageOutput, StageInput, TessellationFactors, IndirectParameters, Indices };

struct InterfaceMember {
    std::optional<std::uint32_t> location;
    std::optional<std::uint32_t> builtin;
    std::uint32_t components = 0;
    std::uint32_t width = 0;
    std::uint32_t offset = 0;
    std::uint32_t bytes = 0;
};

struct InterfaceLayout {
    std::uint32_t stride = 0;
    std::uint32_t alignment = 0;
    std::vector<InterfaceMember> members;
};

struct RectListBufferSlots {
    std::uint32_t output = 28;
    std::uint32_t input = 22;
    std::uint32_t factors = 26;
    std::uint32_t indirect = 23;
    std::uint32_t indices = 21;
};

struct TargetOptions {
    std::uint32_t mslVersion = 30200;
    std::uint32_t maxBuffers = 31;
    std::uint32_t maxTextures = 128;
    std::uint32_t maxSamplers = 16;
    std::uint32_t pushConstantBuffer = 30;
    std::uint32_t bufferSizesBuffer = 29;
    std::uint32_t pushConstantOffsetBytes = 0;
    std::uint32_t vertexBufferCount = 0;
    bool supportsWorkgroupAtomicFences = false;
    bool supportsInt64 = false;
    bool supportsGpuAddresses = false;
    bool supportsSimdGroups = false;
    bool samplerArgumentBuffer = false;
    bool supportsArgumentBuffersTier2 = false;
    std::uint32_t maxArgumentBufferSamplers = 32;
    bool flipVertexY = true;
    bool fixupClipSpace = false;
    RectListMode rectListMode = RectListMode::None;
    std::uint32_t rectListIndexSize = 0;
    std::optional<InterfaceLayout> rectListInputLayout;
    RectListBufferSlots rectListBuffers;
};

struct ResourceMapping {
    std::uint32_t descriptorSet;
    std::uint32_t binding;
    std::uint32_t count;
    DescriptorKind kind;
    DescriptorRole role;
    std::optional<std::uint32_t> buffer;
    std::optional<std::uint32_t> texture;
    std::optional<std::uint32_t> sampler;
    bool active = false;
    bool requiresByteLengths = false;
    bool unsignedStorageImage = false;
};

struct MeshOutputInfo {
    std::uint32_t maxVertices;
    std::uint32_t maxPrimitives;
};

struct ImplicitBufferMapping {
    ImplicitBufferRole role;
    std::uint32_t index;
    std::uint32_t elementBytes;
};

struct RectListInfo {
    RectListMode mode;
    std::uint32_t indexSize = 0;
    std::uint32_t inputControlPoints = 3;
    std::uint32_t outputControlPoints = 4;
    InterfaceLayout inputLayout;
    InterfaceLayout outputLayout;
    std::vector<ImplicitBufferMapping> buffers;
};

struct MinimumLodPair {
    std::uint32_t imageSet, imageBinding, imageElement;
    std::uint32_t samplerSet, samplerBinding, samplerElement;
    std::uint32_t magArgument, minArgument;
    float relativeViewMin;
};

struct MinimumLodImage {
    std::uint32_t descriptorSet, binding, element;
};

struct CapturedSamplerRequirement {
    std::uint32_t descriptorSet, binding, element;
};

struct Result {
    std::vector<CapturedSamplerRequirement> capturedSamplerRequirements;
    std::vector<MinimumLodImage> minimumLodImages;
    bool requiresTextureLodQueries = false;
    std::vector<MinimumLodPair> minimumLodPairs;
    ShaderStage stage;
    std::uint32_t vertexBufferCount = 0;
    std::string source;
    std::string entryPoint;
    RecompileResult guest;
    std::vector<ResourceMapping> resources;
    std::optional<std::uint32_t> pushConstantBuffer;
    std::uint32_t pushConstantBytes = 0;
    std::vector<std::byte> pushConstantData;
    std::optional<std::uint32_t> bufferSizesBuffer;
    std::optional<std::uint32_t> samplerArgumentBuffer;
    std::uint32_t samplerArgumentCount = 0;
    std::array<std::uint32_t, 3> threadsPerThreadgroup{};
    std::vector<std::uint32_t> capabilities;
    std::vector<std::string> extensions;
    bool requiresWorkgroupAtomicFences = false;
    bool requiresGpuAddresses = false;
    bool requiresInt64 = false;
    bool requiresSimdGroups = false;
    std::optional<MeshOutputInfo> mesh;
    NativeExecutionKind nativeExecutionKind = NativeExecutionKind::Compute;
    std::optional<RectListInfo> rectList;
};

[[nodiscard]] Result ConvertToMetal(const RecompileResult& guest, ShaderStage stage,
                                    const TargetOptions& options = {});

}
