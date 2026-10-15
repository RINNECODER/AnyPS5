#pragma once

#include "../Recompiler.hpp"
#include <array>
#include <optional>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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

// Converted MSL text, shared by every copy: a cached conversion is copied into a Result for every
// draw that hits it, and the text (often tens of KiB) never changes once converted. Reads go
// through the const std::string it holds.
class SharedSource {
public:
    SharedSource() = default;
    SharedSource(std::string text) : text(std::make_shared<const std::string>(std::move(text))) {}
    SharedSource(const char* text) : SharedSource(std::string(text)) {}

    [[nodiscard]] const std::string& Str() const { return text ? *text : Empty(); }
    // The shared text itself (null when empty): its address identifies this text while it is held.
    [[nodiscard]] const std::shared_ptr<const std::string>& Shared() const { return text; }
    operator const std::string&() const { return Str(); }
    [[nodiscard]] std::size_t size() const { return Str().size(); }
    [[nodiscard]] bool empty() const { return Str().empty(); }
    [[nodiscard]] const char* data() const { return Str().data(); }
    [[nodiscard]] const char* c_str() const { return Str().c_str(); }
    [[nodiscard]] std::size_t find(std::string_view needle, std::size_t at = 0) const { return Str().find(needle, at); }
    friend bool operator==(const SharedSource& left, const SharedSource& right) { return left.Str() == right.Str(); }

private:
    static const std::string& Empty() {
        static const std::string empty;
        return empty;
    }
    std::shared_ptr<const std::string> text;
};

struct Result {
    std::vector<CapturedSamplerRequirement> capturedSamplerRequirements;
    std::vector<MinimumLodImage> minimumLodImages;
    bool requiresTextureLodQueries = false;
    std::vector<MinimumLodPair> minimumLodPairs;
    ShaderStage stage;
    std::uint32_t vertexBufferCount = 0;
    SharedSource source;
    // ShaderSourceHash(source), set by ConvertToMetal so that a draw does not hash the source again to
    // find its compiled function; 0 when not computed. A stale value only makes that lookup miss.
    std::size_t sourceHash = 0;
    // Whether source stores through BDA pointers (StoresThroughBda), set with sourceHash so that a
    // draw does not search the source for it; unset when not computed.
    std::optional<bool> storesThroughBda;
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

[[nodiscard]] inline std::size_t ShaderSourceHash(std::string_view source) {
    return std::hash<std::string_view>{}(source);
}

// Converts guest SPIR-V to MSL. Results are memoized in a bounded in-memory cache keyed exactly on the
// SPIR-V words, every guest field the conversion reads, the stage and every target option; a cached
// result is returned with the caller's own guest metadata.
[[nodiscard]] Result ConvertToMetal(const RecompileResult& guest, ShaderStage stage,
                                    const TargetOptions& options = {});

struct ConversionCacheStatistics {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::size_t entries = 0;
    std::size_t bytes = 0;
};

[[nodiscard]] ConversionCacheStatistics GetConversionCacheStatistics();
void ClearConversionCache();

}
