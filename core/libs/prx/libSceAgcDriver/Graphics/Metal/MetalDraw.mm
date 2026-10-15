#include "MetalDraw.hpp"
#include "MetalShaderResources.hpp"
#include "MetalRenderState.hpp"
#include "MetalDepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "RdnaDecoder/include/RdnaDecoder/RdnaDescriptorFormat.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <string>
#include <vector>
#include <limits>
#include <optional>
#include <stdexcept>

namespace AgcDriver::Metal {
namespace {
using namespace ShaderRecompiler;

// Whether a texture descriptor's format reads the depth (or stencil) plane of a resident depth surface:
// the plane's own single-channel format or a depth-bits view of its width.
bool readsDepthPlane(const Graphics::GuestTextureResource& resource, const Graphics::DepthTarget& target, bool stencil) {
    const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
    const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
    const bool depthBits = !stencil && DepthBitsTextureWidth(resource.format << 20u,
        Graphics::XorSwizzleMode(resource.tileMode) << 20u) == (d16 ? 16u : 32u);
    return Graphics::ResolveTextureFormat(resource.format) == expected || depthBits;
}

void configureSamplerArguments(MetalBackend::TargetOptions& options, const RecompileResult& guest, id<MTLDevice> device) {
    options.supportsArgumentBuffersTier2 = device.argumentBuffersSupport == MTLArgumentBuffersTier2;
    options.maxArgumentBufferSamplers = 32;
    options.samplerArgumentBuffer = false;
    std::uint32_t count = 0;
    for (const auto& binding : guest.bindings) {
        if (binding.kind != DescriptorKind::Sampler) continue;
        if (binding.count > options.maxSamplers - count) {
            options.samplerArgumentBuffer = true;
            break;
        }
        count += binding.count;
    }
}

std::atomic<std::uint64_t> samplesPassed{0};

struct SampleCounterParameters {
    std::uint32_t count;
    std::uint32_t store;
};

std::uint64_t sampleTotal(id<MTLBuffer> counter) {
    std::uint64_t total;
    std::memcpy(&total, counter.contents, sizeof(total));
    return total;
}

// A capture or control pass binds the work's fault buffer when it has one (Complete reads it for
// every pass); `required` when the pass must have one.
void requireFaultBinding(const MetalBackend::Result& shader, std::span<const MetalShaderResourceBinding> bindings, const char* missing,
    bool required = true) {
    bool found = false;
    for (const auto& mapping : shader.resources) {
        if (mapping.role != DescriptorRole::FaultBuffer) continue;
        const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& resource) {
            return resource.descriptorSet == mapping.descriptorSet && resource.binding == mapping.binding;
        });
        if (binding == bindings.end() || binding->buffers.size() != 1) throw std::runtime_error(missing);
        found = true;
    }
    if (required && !found) throw std::runtime_error(missing);
}

std::uint32_t rectSlot(const MetalBackend::Result& shader, MetalBackend::ImplicitBufferRole role) {
    if (!shader.rectList) throw std::invalid_argument("Metal rectangle stage lacks an intermediate ABI");
    const auto& buffers = shader.rectList->buffers;
    const auto found = std::find_if(buffers.begin(), buffers.end(), [&](const auto& buffer) { return buffer.role == role; });
    if (found == buffers.end()) throw std::invalid_argument("Metal rectangle intermediate binding is absent");
    return found->index;
}

MTLVertexFormat vertexFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return MTLVertexFormatUCharNormalized;
    case VK_FORMAT_R8G8_UNORM: return MTLVertexFormatUChar2Normalized;
    case VK_FORMAT_R8G8B8_UNORM: return MTLVertexFormatUChar3Normalized;
    case VK_FORMAT_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
    case VK_FORMAT_R8_SNORM: return MTLVertexFormatCharNormalized;
    case VK_FORMAT_R8G8_SNORM: return MTLVertexFormatChar2Normalized;
    case VK_FORMAT_R8G8B8_SNORM: return MTLVertexFormatChar3Normalized;
    case VK_FORMAT_R8G8B8A8_SNORM: return MTLVertexFormatChar4Normalized;
    case VK_FORMAT_R8_UINT: return MTLVertexFormatUChar;
    case VK_FORMAT_R8G8_UINT: return MTLVertexFormatUChar2;
    case VK_FORMAT_R8G8B8_UINT: return MTLVertexFormatUChar3;
    case VK_FORMAT_R8G8B8A8_UINT: return MTLVertexFormatUChar4;
    case VK_FORMAT_R8_SINT: return MTLVertexFormatChar;
    case VK_FORMAT_R8G8_SINT: return MTLVertexFormatChar2;
    case VK_FORMAT_R8G8B8_SINT: return MTLVertexFormatChar3;
    case VK_FORMAT_R8G8B8A8_SINT: return MTLVertexFormatChar4;
    case VK_FORMAT_R16_UNORM: return MTLVertexFormatUShortNormalized;
    case VK_FORMAT_R16G16_UNORM: return MTLVertexFormatUShort2Normalized;
    case VK_FORMAT_R16G16B16_UNORM: return MTLVertexFormatUShort3Normalized;
    case VK_FORMAT_R16G16B16A16_UNORM: return MTLVertexFormatUShort4Normalized;
    case VK_FORMAT_R16_SNORM: return MTLVertexFormatShortNormalized;
    case VK_FORMAT_R16G16_SNORM: return MTLVertexFormatShort2Normalized;
    case VK_FORMAT_R16G16B16_SNORM: return MTLVertexFormatShort3Normalized;
    case VK_FORMAT_R16G16B16A16_SNORM: return MTLVertexFormatShort4Normalized;
    case VK_FORMAT_R16_UINT: return MTLVertexFormatUShort;
    case VK_FORMAT_R16G16_UINT: return MTLVertexFormatUShort2;
    case VK_FORMAT_R16G16B16_UINT: return MTLVertexFormatUShort3;
    case VK_FORMAT_R16G16B16A16_UINT: return MTLVertexFormatUShort4;
    case VK_FORMAT_R16_SINT: return MTLVertexFormatShort;
    case VK_FORMAT_R16G16_SINT: return MTLVertexFormatShort2;
    case VK_FORMAT_R16G16B16_SINT: return MTLVertexFormatShort3;
    case VK_FORMAT_R16G16B16A16_SINT: return MTLVertexFormatShort4;
    case VK_FORMAT_R16_SFLOAT: return MTLVertexFormatHalf;
    case VK_FORMAT_R16G16_SFLOAT: return MTLVertexFormatHalf2;
    case VK_FORMAT_R16G16B16_SFLOAT: return MTLVertexFormatHalf3;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLVertexFormatHalf4;
    case VK_FORMAT_R32_SFLOAT: return MTLVertexFormatFloat;
    case VK_FORMAT_R32G32_SFLOAT: return MTLVertexFormatFloat2;
    case VK_FORMAT_R32G32B32_SFLOAT: return MTLVertexFormatFloat3;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return MTLVertexFormatFloat4;
    case VK_FORMAT_R32_UINT: return MTLVertexFormatUInt;
    case VK_FORMAT_R32G32_UINT: return MTLVertexFormatUInt2;
    case VK_FORMAT_R32G32B32_UINT: return MTLVertexFormatUInt3;
    case VK_FORMAT_R32G32B32A32_UINT: return MTLVertexFormatUInt4;
    case VK_FORMAT_R32_SINT: return MTLVertexFormatInt;
    case VK_FORMAT_R32G32_SINT: return MTLVertexFormatInt2;
    case VK_FORMAT_R32G32B32_SINT: return MTLVertexFormatInt3;
    case VK_FORMAT_R32G32B32A32_SINT: return MTLVertexFormatInt4;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLVertexFormatUInt1010102Normalized;
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return MTLVertexFormatInt1010102Normalized;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return MTLVertexFormatFloatRG11B10;
    default: return MTLVertexFormatInvalid;
    }
}

MTLTextureSwizzle channelSwizzle(std::uint8_t value) {
    switch (value) {
    case 0: return MTLTextureSwizzleZero;
    case 1: return MTLTextureSwizzleOne;
    case 4: return MTLTextureSwizzleRed;
    case 5: return MTLTextureSwizzleGreen;
    case 6: return MTLTextureSwizzleBlue;
    case 7: return MTLTextureSwizzleAlpha;
    default: throw std::invalid_argument("Metal draw resident surface component selection is invalid");
    }
}

Graphics::GuestTextureResource colorSurface(const Graphics::ColorTarget& color) {
    if (color.depth == 0 || color.mipCount == 0 || color.mip >= color.mipCount || color.depthSlice >= color.depth) {
        throw std::invalid_argument("Metal draw color subresource is invalid");
    }
    const auto format = Graphics::FindGuestColorTargetFormat(color.format, color.elementBytes);
    if (!format) throw std::invalid_argument("Metal draw color format has no guest texture mapping");
    const bool chain = color.mipCount > 1;
    Graphics::GuestTextureResource surface{};
    surface.baseAddress = chain ? color.surfaceAddress : color.address;
    surface.width = chain ? color.surfaceExtent.width : color.extent.width;
    surface.height = chain ? color.surfaceExtent.height : color.extent.height;
    surface.depthOrLastArray = color.depth - 1;
    surface.mipCount = color.mipCount;
    surface.lastLevel = color.mipCount - 1;
    surface.tileMode = Graphics::ColorTextureTileMode(color.tileMode);
    surface.dimension = color.depth > 1 ? Graphics::TextureDimension::k3D : Graphics::TextureDimension::k2D;
    surface.format = *format;
    surface.dstSelX = 4;
    surface.dstSelY = 5;
    surface.dstSelZ = 6;
    surface.dstSelW = 7;
    surface.dccAddress = color.dccAddress;
    surface.dccAlphaOnMsb = color.dccAlphaOnMsb;
    surface.dccPipeAligned = color.dccPipeAligned;
    return surface;
}

// Metal has no geometry stage. A pixel shader that reads per-vertex parameters (AMD P0/P10/P20, the
// barycentric emulation Vulkan feeds from BuildBarycentricGeometryShader) is drawn in two passes: the
// vertex shader runs as a capture kernel, as for rectangle lists, and this generated vertex function
// draws the captured triangles. Each corner gets what the geometry shader gives it: its own position
// and parameters, the weights of its corner, and the parameters of all three corners of its triangle.
constexpr const char* BarycentricVertexEntry = "anyps5_barycentric_vertex";

struct FragmentInput {
    std::string type;
    std::uint32_t location;
};

// The [[user(locnN)]] stage inputs of a converted fragment shader, with their MSL types: a vertex
// function feeding it must write the same types at the same locations.
std::vector<FragmentInput> fragmentInputs(const std::string& source) {
    std::vector<FragmentInput> inputs;
    std::size_t at = 0;
    while ((at = source.find("[[user(locn", at)) != std::string::npos) {
        const auto lineStart = source.rfind('\n', at) + 1;
        auto cursor = at + std::strlen("[[user(locn");
        std::uint32_t location = 0;
        const auto digits = cursor;
        while (cursor < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor]))) {
            location = location * 10u + static_cast<std::uint32_t>(source[cursor++] - '0');
        }
        if (cursor == digits || cursor >= source.size() || source[cursor] != ')') {
            throw std::invalid_argument("Metal per-vertex parameter emulation does not support component-packed fragment inputs");
        }
        auto typeStart = source.find_first_not_of(" \t", lineStart);
        const auto typeEnd = source.find_first_of(" \t", typeStart);
        if (typeStart == std::string::npos || typeEnd == std::string::npos || typeEnd > at) {
            throw std::invalid_argument("Metal per-vertex parameter emulation cannot read a fragment input type");
        }
        inputs.push_back({source.substr(typeStart, typeEnd - typeStart), location});
        at = cursor;
    }
    return inputs;
}

// Converts four raw 32-bit words (`words`, a uint4 expression) to an MSL scalar or vector type.
std::string convertWords(const std::string& type, const std::string& words) {
    std::size_t split = type.size();
    while (split > 0 && std::isdigit(static_cast<unsigned char>(type[split - 1]))) --split;
    const auto base = type.substr(0, split);
    const auto count = split == type.size() ? 1u : static_cast<std::uint32_t>(std::stoul(type.substr(split)));
    if (count == 0 || count > 4) throw std::invalid_argument("Metal per-vertex parameter emulation met an invalid fragment input type " + type);
    static constexpr std::array<const char*, 5> swizzles{"", ".x", ".xy", ".xyz", ""};
    std::string value;
    if (base == "float") value = "as_type<float4>(" + words + ")";
    else if (base == "half") value = "half4(as_type<float4>(" + words + "))";
    else if (base == "int") value = "as_type<int4>(" + words + ")";
    else if (base == "uint") value = words;
    else throw std::invalid_argument("Metal per-vertex parameter emulation met an unsupported fragment input type " + type);
    return "(" + value + ")" + swizzles[count];
}

std::string barycentricVertexSource(const MetalBackend::InterfaceLayout& captured, const std::string& fragmentSource,
    const RecompileResult& fragment, bool flipY, bool fixupClipSpace) {
    constexpr std::uint32_t PositionBuiltin = 0; // SPIR-V BuiltIn Position
    if (captured.stride == 0 || captured.stride % 4u != 0) throw std::invalid_argument("Metal per-vertex parameter capture stride is not word aligned");
    const MetalBackend::InterfaceMember* position = nullptr;
    std::vector<const MetalBackend::InterfaceMember*> byLocation;
    for (const auto& member : captured.members) {
        if (member.offset % 4u != 0 || (member.width != 0 && member.width != 32) || member.components > 4) {
            throw std::invalid_argument("Metal per-vertex parameter capture holds a non-32-bit or unaligned output");
        }
        if (member.builtin && *member.builtin == PositionBuiltin) position = &member;
        if (member.location) {
            if (*member.location >= byLocation.size()) byLocation.resize(*member.location + 1u, nullptr);
            byLocation[*member.location] = &member;
        }
    }
    if (position == nullptr || position->components != 4) throw std::invalid_argument("Metal per-vertex parameter capture has no vec4 position");
    const auto layout = LayoutBarycentricEmulation(fragment.fragmentParameters, fragment.barycentricEmulation);
    const auto strideWords = std::to_string(captured.stride / 4u);
    const auto load = [&](const std::string& vertex, std::uint32_t location) -> std::string {
        if (location >= byLocation.size() || byLocation[location] == nullptr) return "uint4(0u)";
        const auto& member = *byLocation[location];
        return "anyps5Words(captured, (" + vertex + ") * " + strideWords + "u + " + std::to_string(member.offset / 4u) + "u, " +
            std::to_string(member.components) + "u)";
    };
    std::string members, stores;
    for (const auto& input : fragmentInputs(fragmentSource)) {
        const auto name = "v" + std::to_string(input.location);
        members += "    " + input.type + " " + name + " [[user(locn" + std::to_string(input.location) + ")]];\n";
        std::string words;
        if (input.location == layout.smoothLocation || input.location == layout.linearLocation) {
            words = "as_type<uint4>(float4(corner == 0u ? 1.0f : 0.0f, corner == 1u ? 1.0f : 0.0f, corner == 2u ? 1.0f : 0.0f, 0.0f))";
        } else {
            const auto perVertex = std::find_if(layout.perVertexLocations.begin(), layout.perVertexLocations.end(), [&](const auto& entry) {
                return input.location >= entry.second && input.location < entry.second + 3u;
            });
            const auto parameter = [&](auto matches) {
                return std::find_if(fragment.fragmentParameters.begin(), fragment.fragmentParameters.end(), matches);
            };
            if (perVertex != layout.perVertexLocations.end()) {
                const auto source = parameter([&](const auto& p) { return p.perVertex && p.location == perVertex->first; });
                if (source == fragment.fragmentParameters.end()) throw std::invalid_argument("Metal per-vertex parameter has no source");
                words = load("first + " + std::to_string(input.location - perVertex->second) + "u", source->sourceLocation);
            } else {
                const auto source = parameter([&](const auto& p) { return !p.perVertex && p.location == input.location; });
                words = load("vertexId", source != fragment.fragmentParameters.end() ? source->sourceLocation : input.location);
            }
        }
        stores += "    out." + name + " = " + convertWords(input.type, words) + ";\n";
    }
    std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct AnyPS5BarycentricOut {\n    float4 position [[position]];\n" + members + "};\n"
        "static inline uint4 anyps5Words(device const uint* words, uint at, uint count) {\n"
        "    uint4 value = uint4(0u);\n"
        "    for (uint i = 0u; i < count; ++i) value[i] = words[at + i];\n"
        "    return value;\n}\n"
        "vertex AnyPS5BarycentricOut " + std::string(BarycentricVertexEntry) +
        "(uint vertexId [[vertex_id]], device const uint* captured [[buffer(0)]]) {\n"
        "    AnyPS5BarycentricOut out;\n"
        "    const uint corner = vertexId % 3u;\n"
        "    const uint first = vertexId - corner;\n"
        "    float4 position = as_type<float4>(anyps5Words(captured, vertexId * " + strideWords + "u + " +
            std::to_string(position->offset / 4u) + "u, 4u));\n";
    if (flipY) source += "    position.y = -position.y;\n";
    if (fixupClipSpace) source += "    position.z = (position.z + position.w) * 0.5f;\n";
    source += "    out.position = position;\n" + stores + "    return out;\n}\n";
    return source;
}

std::span<const std::byte> pushBlock(const MetalBackend::Result& shader,
    const std::array<std::byte, Graphics::PipelinePushConstantBytes>& bytes) {
    if (!shader.pushConstantBuffer) return {};
    if (shader.pushConstantBytes > bytes.size()) throw std::invalid_argument("Metal draw push constant block exceeds pipeline ABI");
    return std::span(bytes).first(shader.pushConstantBytes);
}

}

MetalDraw::MetalDraw(id<MTLDevice> device, id<MTLLibrary> utilityLibrary)
    : MetalDraw(std::make_unique<MetalDevice>(device, utilityLibrary), nullptr) {}

MetalDraw::MetalDraw(MetalDevice& shared) : MetalDraw(nullptr, &shared) {}

MetalDraw::MetalDraw(std::unique_ptr<MetalDevice> owned, MetalDevice* shared)
    : ownedBackend(std::move(owned)), backend(shared != nullptr ? *shared : *ownedBackend), depthCache(std::make_unique<MetalDepthSurfaceCache>(backend)),
      hostAliases(std::make_unique<MetalGuestHostAliases>(backend.Device())) {
    if (![backend.Device() supportsFamily:MTLGPUFamilyApple7] || ![backend.Device() supportsFamily:MTLGPUFamilyMetal3]) {
        throw std::invalid_argument("Metal draw requires an Apple GPU with Metal 3 integer and GPU address support");
    }
}

MetalDraw::~MetalDraw() = default;

void MetalDraw::SetCpuReadHook(std::function<void(std::uint64_t, std::uint64_t)> hook) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    cpuReadHook = std::move(hook);
}

void MetalDraw::SetMaterializeHook(std::function<void(std::uint64_t, std::uint64_t)> hook) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    materializeHook = std::move(hook);
}

void MetalDraw::InvalidateBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> changed) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    imageCache.Clear();
    depthCache->Invalidate(changed);
    for (const auto& range : changed) hostAliases->Invalidate(range.host.data(), range.host.size());
    std::erase_if(depthSurfaces, [&](const auto& surface) { return surface->OverlapsMappings(changed); });
    std::erase_if(colorSurfaces, [&](const auto& surface) { return surface->OverlapsMappings(changed); });
    depthArrays.clear();
}

MetalGuestHostAliases& MetalDraw::HostAliases() { return *hostAliases; }
GuestImageCache<MetalTexture>* MetalDraw::ImageCache() {
    static const bool enabled = [] {
        const char* value = std::getenv("ANYPS5_METAL_IMAGE_CACHE");
        return value == nullptr || std::string_view(value) != "0";
    }();
    return enabled ? &imageCache : nullptr;
}

MeshTargetLimits MetalDraw::MeshLimits() const {
    const auto maximum = backend.Device().maxThreadsPerThreadgroup;
    return {{static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(maximum.height), static_cast<std::uint32_t>(maximum.depth)},
        static_cast<std::uint32_t>(maximum.width), static_cast<std::uint32_t>(backend.Device().maxThreadgroupMemoryLength),
        256, 512, 128, 16384, 1, 1};
}

TessellationTargetLimits MetalDraw::TessellationLimits() const {
    return {32, 128, 128, 120, 4096, 128, 128};
}

id<MTLTexture> MetalDraw::ResidentTexture(const Graphics::GuestTextureResource& resource, bool storage, bool compare) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    return residentTexture(resource, storage, compare);
}

// A 2D-array descriptor of resident depth slices (GatherDepthSlices); nil when its address is not a
// resident depth plane or the descriptor does not read one the way residentView would.
id<MTLTexture> MetalDraw::residentDepthArray(const Graphics::GuestTextureResource& resource, bool storage) {
    if (storage || resource.dimension != Graphics::TextureDimension::k2DArray || resource.baseLevel != 0 ||
        resource.lastLevel != 0 || resource.samples > 1 || resource.depthOrLastArray < resource.baseArray) {
        return nil;
    }
    const auto base = std::find_if(depthSurfaces.rbegin(), depthSurfaces.rend(), [&](const auto& surface) { return surface->Target().address == resource.baseAddress; });
    if (base == depthSurfaces.rend()) return nil;
    const auto baseSurface = *base;  // depthSurfaces may grow below
    const auto& target = baseSurface->Target();
    const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
    const bool depthBits = DepthBitsTextureWidth(resource.format << 20u, Graphics::XorSwizzleMode(resource.tileMode) << 20u) == (d16 ? 16u : 32u);
    if (resource.width != target.extent.width || resource.height != target.extent.height || target.samples != 1 ||
        (Graphics::ResolveTextureFormat(resource.format) != (d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT) && !depthBits)) {
        return nil;
    }
    // A slice no draw has rendered gets the surface its first draw would create: a layer of the base's
    // depth array (cleared like the base when the array is created or grows).
    const auto sliceBytes = Graphics::DepthSliceBytes(target.extent, d16 ? 2u : 4u);
    const auto stencilBytes = Graphics::DepthSliceBytes(target.extent, 1u);
    for (std::uint32_t layer = resource.baseArray; layer <= resource.depthOrLastArray; ++layer) {
        auto slice = target;
        slice.address = target.address + std::uint64_t{layer} * sliceBytes;
        if (slice.stencilAddress != 0) slice.stencilAddress += std::uint64_t{layer} * stencilBytes;
        slice.slice = target.slice + layer;
        slice.lastSlice = std::max(target.lastSlice, target.slice + resource.depthOrLastArray);
        // Acquire places the slice in the base's depth array (moving a surface rendered on its own).
        auto surface = depthCache->Acquire(slice);
        std::erase_if(depthSurfaces, [&](const auto& other) { return other != surface && other->Target().address == slice.address; });
        if (std::find(depthSurfaces.begin(), depthSurfaces.end(), surface) == depthSurfaces.end()) depthSurfaces.push_back(surface);
    }
    auto& array = depthArrays[{resource.baseAddress, resource.baseArray, resource.depthOrLastArray}];
    array = GatherDepthSlices(backend, depthSurfaces, *baseSurface, resource.baseArray, resource.depthOrLastArray, array);
    const auto swizzle = MTLTextureSwizzleChannelsMake(channelSwizzle(resource.dstSelX), channelSwizzle(resource.dstSelY),
        channelSwizzle(resource.dstSelZ), channelSwizzle(resource.dstSelW));
    // An in-place array is a view of the depth planes: view the planes themselves rather than the view.
    id<MTLTexture> source = array.parentTexture != nil ? array.parentTexture : array;
    const auto firstSlice = array.parentTexture != nil ? array.parentRelativeSlice : 0;
    id<MTLTexture> view = [source newTextureViewWithPixelFormat:array.pixelFormat textureType:MTLTextureType2DArray
        levels:NSMakeRange(0, 1) slices:NSMakeRange(firstSlice, array.arrayLength) swizzle:swizzle];
    if (view == nil) throw std::runtime_error("Metal draw depth array view creation failed");
    return view;
}

id<MTLTexture> MetalDraw::residentTexture(const Graphics::GuestTextureResource& resource, bool storage, bool compare) {
    // Guest memory reused: a descriptor whose texels cannot be the depth or stencil plane of a resident
    // surface at its address (a block-compressed texture placed where a depth target was) is another
    // resource. Its texels are in guest memory; the surface's are dead, so it is dropped (a later draw
    // into the address makes a new one).
    DropReplacedDepthSurfaces(depthSurfaces, *depthCache, resource);
    if (auto array = residentDepthArray(resource, storage); array != nil) return array;
    if (auto view = residentView(resource, storage, compare); view != nil || resource.samples <= 1) return view;
    // A multisampled image that no draw has rendered yet (a target cleared only through its DCC keys
    // and then read by a resolve or post pass). Guest memory never holds fragments, so the reader
    // gets the resident surface the first draw into the image would create: a new one starts
    // cleared to zero, as a draw-created surface does. Later draws into it render into this surface.
    if (storage || compare || resource.dimension != Graphics::TextureDimension::k2D || resource.baseLevel != 0 ||
        resource.lastLevel != 0 || resource.baseArray != 0 || resource.depthOrLastArray != 0) {
        return nil;
    }
    Graphics::ColorTarget target{};
    target.address = resource.baseAddress;
    target.extent = {resource.width, resource.height};
    target.format = Graphics::ResolveTextureFormat(resource.format);
    target.elementBytes = Graphics::BytesPerElement(resource.format);
    target.bytes = static_cast<std::size_t>(resource.width) * resource.height * target.elementBytes;
    target.dccAddress = resource.dccAddress;
    target.samples = target.coverageSamples = resource.samples;
    static_cast<void>(RenderPixelFormat(target.format)); // A format with no render target stays unbound.
    auto surface = depthCache->Acquire(target);
    if (std::find(colorSurfaces.begin(), colorSurfaces.end(), surface) == colorSurfaces.end()) colorSurfaces.push_back(surface);
    return residentView(resource, storage, compare);
}

id<MTLTexture> MetalDraw::residentView(const Graphics::GuestTextureResource& resource, bool storage, bool compare) const {
    for (auto cursor = depthSurfaces.rbegin(); cursor != depthSurfaces.rend(); ++cursor) {
        const auto& surface = *cursor;
        const auto& target = surface->Target();
        const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
        const bool isDepth = resource.baseAddress == target.address;
        if (!stencil && !isDepth) continue;
        if (storage || resource.width != target.extent.width || resource.height != target.extent.height ||
            resource.dimension != Graphics::TextureDimension::k2D || resource.baseLevel != 0 || resource.lastLevel != 0 ||
            resource.baseArray != 0 || resource.depthOrLastArray != 0 || (stencil && compare) || resource.samples != target.samples) {
            char detail[224];
            std::snprintf(detail, sizeof detail, " (%ux%u dim %d levels %u-%u/%u layers %u-%u samples %u fmt %u tile %u%s%s; surface %ux%u samples %u fmt %d)",
                resource.width, resource.height, static_cast<int>(resource.dimension), resource.baseLevel, resource.lastLevel,
                resource.mipCount, resource.baseArray, resource.depthOrLastArray, resource.samples, resource.format,
                static_cast<unsigned>(resource.tileMode), storage ? " storage" : "", stencil ? " stencil" : "",
                target.extent.width, target.extent.height, target.samples, static_cast<int>(target.format));
            throw std::invalid_argument(std::string("Metal draw resident depth view is incompatible with the guest descriptor") + detail);
        }
        if (!readsDepthPlane(resource, target, stencil)) {
            throw std::invalid_argument("Metal draw resident depth plane format disagrees with the guest descriptor");
        }
        const auto plane = stencil ? surface->SampledStencilPlane() : surface->SampledDepthPlane();
        if (plane.texture == nil) throw std::invalid_argument("Metal draw sampled depth plane is missing");
        const auto swizzle = MTLTextureSwizzleChannelsMake(channelSwizzle(resource.dstSelX), channelSwizzle(resource.dstSelY),
            channelSwizzle(resource.dstSelZ), channelSwizzle(resource.dstSelW));
        // A layer of the surface's planes (a 2D array for single-sample surfaces) viewed as one 2D texture.
        id<MTLTexture> view = [plane.texture newTextureViewWithPixelFormat:plane.format
            textureType:plane.texture.textureType == MTLTextureType2DArray ? MTLTextureType2D : plane.texture.textureType
            levels:NSMakeRange(0, 1) slices:NSMakeRange(plane.layer, 1) swizzle:swizzle];
        if (view == nil) throw std::runtime_error("Metal draw resident depth component view creation failed");
        return view;
    }
    if (resource.samples <= 1) return nil;
    for (auto cursor = colorSurfaces.rbegin(); cursor != colorSurfaces.rend(); ++cursor) {
        const auto& surface = *cursor;
        const auto& target = surface->Target();
        if (resource.baseAddress != target.address) continue;
        if (storage || compare || resource.width != target.extent.width || resource.height != target.extent.height ||
            resource.dimension != Graphics::TextureDimension::k2D || resource.baseArray != 0 || resource.depthOrLastArray != 0 ||
            resource.samples != target.samples) {
            throw std::invalid_argument("Metal draw resident multisampled color view is incompatible with the guest descriptor");
        }
        id<MTLTexture> source = surface->Texture();
        // The descriptor may reinterpret the fragments (an sRGB or swapped view of the same texel size).
        auto format = source.pixelFormat;
        try {
            format = RenderPixelFormat(Graphics::ResolveTextureFormat(resource.format));
        } catch (const std::invalid_argument&) {
        }
        if (format != source.pixelFormat) source = ColorAttachmentView(source, format);
        const auto swizzle = MTLTextureSwizzleChannelsMake(channelSwizzle(resource.dstSelX), channelSwizzle(resource.dstSelY),
            channelSwizzle(resource.dstSelZ), channelSwizzle(resource.dstSelW));
        id<MTLTexture> view = [source newTextureViewWithPixelFormat:source.pixelFormat textureType:source.textureType
            levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:swizzle];
        if (view == nil) throw std::runtime_error("Metal draw resident multisampled color view creation failed");
        return view;
    }
    return nil;
}

void MetalDraw::DumpSamplesSynchronously(std::uint64_t guestAddress) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    constexpr std::size_t targetBytes = 15 * 16 + 8;
    if (guestAddress == 0 || guestAddress % 8 != 0 ||
        guestAddress > std::numeric_limits<std::uint64_t>::max() - targetBytes) {
        throw std::invalid_argument("Metal sample counter target is null, misaligned or overflowing");
    }
    for (std::uint64_t db = 0; db < 16; ++db) {
        GuestMemory::CheckRange(reinterpret_cast<const void*>(guestAddress + db * 16), 8, 8, true);
    }
    if (sampleCounter == nil) {
        auto counter = backend.Buffer(24);
        auto target = backend.Buffer(targetBytes);
        std::memset(counter.contents, 0, counter.length);
        const auto initial = samplesPassed.load(std::memory_order_acquire);
        std::memcpy(counter.contents, &initial, sizeof(initial));
        sampleCounter = counter;
        sampleTarget = target;
    }
    const SampleCounterParameters parameters{0, 1};
    auto commands = backend.CommandBuffer();
    backend.Encode(commands, @"SampleCounter", @[sampleCounter, sampleTarget], &parameters, sizeof(parameters), MTLSizeMake(1, 1, 1));
    backend.Wait(commands);
    samplesPassed.store(sampleTotal(sampleCounter), std::memory_order_release);
    const auto* values = static_cast<const std::byte*>(sampleTarget.contents);
    for (std::uint64_t db = 0; db < 16; ++db) {
        GuestMemory::Write(guestAddress + db * 16, std::span(values + db * 16, 8), 8);
    }
}

BdaAbi::Fault MetalDraw::DrawSynchronously(const Graphics::State& state, const Pm4::DrawParameters& draw,
    std::span<const Graphics::CompiledShader> shaders,
    std::span<const NativeGuestMemory::BorrowedRange> ranges,
    std::optional<std::array<std::uint32_t, 5>> meshArguments) {
    return *Draw(state, draw, shaders, ranges, meshArguments, nullptr, {});
}

BdaAbi::Fault MetalDraw::ResolveSynchronously(const Graphics::ColorTarget& source, const Graphics::ColorTarget& destination,
    std::span<const NativeGuestMemory::BorrowedRange> ranges) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    if (source.samples < 2 || destination.samples != 1 || destination.depth != 1 || source.format != destination.format ||
        source.extent.width != destination.extent.width || source.extent.height != destination.extent.height) {
        throw std::invalid_argument("Metal CB resolve needs a multisampled source and a single-sample 2D destination of its format and extent");
    }
    auto surface = depthCache->Acquire(source);
    if (std::find(colorSurfaces.begin(), colorSurfaces.end(), surface) == colorSurfaces.end()) colorSurfaces.push_back(surface);
    auto resourcesOwner = std::make_shared<MetalShaderResources>(backend, ranges,
        [this](const Graphics::GuestTextureResource& resource, bool storage, bool compare) {
            return residentTexture(resource, storage, compare);
        }, hostAliases.get(), ImageCache());
    auto& resources = *resourcesOwner;
    if (cpuReadHook) resources.SetCpuReadHook(cpuReadHook);
    resources.SetLazyCopyBacks(&lazyCopyBacks);
    if (materializeHook) resources.SetMaterializeHook(materializeHook);
    auto texture = resources.Texture(colorSurface(destination), true, false, false, destination.mip);
    id<MTLTexture> handle = ColorAttachmentView(texture->Texture(), RenderPixelFormat(destination.format));
    switch (handle.pixelFormat) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint:
        throw std::invalid_argument("Metal CB resolve of an integer color target is unsupported");
    default:
        break;
    }
    id<MTLTexture> level = [handle newTextureViewWithPixelFormat:handle.pixelFormat textureType:MTLTextureType2D
        levels:NSMakeRange(destination.mip, 1) slices:NSMakeRange(0, 1)];
    if (level == nil) throw std::runtime_error("Metal CB resolve destination view creation failed");
    // A pass with no draws that loads the samples and stores them resolved.
    auto renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPass.renderTargetWidth = source.extent.width;
    renderPass.renderTargetHeight = source.extent.height;
    renderPass.colorAttachments[0].texture = surface->Texture();
    renderPass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    renderPass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
    renderPass.colorAttachments[0].resolveTexture = level;
    id<MTLCommandBuffer> commands = backend.CommandBuffer();
    auto encoder = MetalEncoders::Render(commands, renderPass);
    if (encoder == nil) throw MetalGpuExecutionError("Metal CB resolve render encoder allocation failed");
    [encoder endEncoding];
    resources.EncodeReadbacks(commands);
    backend.Wait(commands);
    return CompleteCommittedWork([&] { return resources.Complete(commands); });
}

namespace {

// Whether every vertex buffer of `attributes` can be bound over its descriptor's whole record range
// (Graphics::WholeVertexReadSize): one host run of borrowed guest bytes, at most 256 MiB each, whose
// record count bounds every index. With OUT_OF_BOUNDS_SELECT 0 or 1 a strided per-vertex fetch may
// index past the records (VertexBufferReadSize sizes the binding through the largest index), so such
// a draw still scans.
bool WholeVertexRangesBindable(std::span<const ShaderRecompiler::VertexAttribute> attributes, const Pm4::DrawParameters& draw) {
    constexpr std::size_t MaximumBytes = std::size_t{256} << 20u;
    for (const auto& attribute : attributes) {
        if (Graphics::NullVertexDescriptor(attribute)) continue;
        const auto stride = (attribute.resource.fields[1] >> 16u) & 0x3fffu;
        if (attribute.fetchIndex == 0 && stride != 0 && Graphics::VertexBufferOutOfBoundsSelect(attribute) <= 1u) return false;
        try {
            const auto bytes = Graphics::WholeVertexReadSize(attribute, draw.instanceCount, draw.firstInstance);
            const auto& fields = attribute.resource.fields;
            const auto address = fields[0] | (std::uint64_t{fields[1] & 0xffffu} << 32u);
            if (bytes > MaximumBytes || NativeGuestMemory::ReadableBorrowedBytes(address, bytes) < bytes) return false;
            // One host run, as MetalShaderResources::mirror needs (throws otherwise).
            static_cast<void>(NativeGuestMemory::ContiguousBorrowedRange(address, bytes, false));
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

}

std::optional<BdaAbi::Fault> MetalDraw::Draw(const Graphics::State& state, const Pm4::DrawParameters& draw,
    std::span<const Graphics::CompiledShader> shaders,
    std::span<const NativeGuestMemory::BorrowedRange> ranges,
    std::optional<std::array<std::uint32_t, 5>> meshArguments, MetalTimeline* timeline, Completed completed) {
    std::lock_guard<std::recursive_mutex> lock(drawMutex);
    // An indirect draw here is one whose records the GPU reads (MetalDriver's GpuReadsIndirectRecords):
    // the CP's counts and first index or vertex come from the guest argument buffer when the draw runs,
    // and so do the base vertex and first instance where the rule is InPlace (Rule::Constant uses the
    // driver's constant). indexCount is then the declared INDEX_BUFFER_SIZE and instanceCount unknown.
    const auto* gpuIndirect = draw.indirect ? &*draw.indirect : nullptr;
    if (gpuIndirect != nullptr && (gpuIndirect->countIndirect || gpuIndirect->arguments % 4 != 0 || gpuIndirect->stride % 4 != 0 ||
        gpuIndirect->count > MaxGpuIndirectRecords || meshArguments)) {
        throw IndirectNeedsCpuRecords("Metal GPU indirect draw needs its records counted on the CPU");
    }
    if (draw.flags != (draw.indexed ? 0u : (draw.flags & 0x20u))) throw std::invalid_argument("Metal draw packet flags are unsupported");
    if (gpuIndirect != nullptr && (gpuIndirect->count == 0 || (draw.indexed && draw.indexCount == 0))) return {};
    if (gpuIndirect == nullptr && (draw.indexCount == 0 || draw.instanceCount == 0)) return {};
    if (gpuIndirect == nullptr && draw.firstInstance > std::numeric_limits<std::uint32_t>::max() - (draw.instanceCount - 1u)) {
        throw std::invalid_argument("Metal draw instance range overflows guest invocation index");
    }
    const bool meshPath = state.stages.mesh.has_value();
    const bool rectPath = state.rectList;
    if (rectPath && (meshPath || draw.indexCount % 3 != 0)) {
        throw std::invalid_argument("Metal rectangle draw requires complete three-vertex primitives");
    }
    if (meshArguments && (!meshPath || !draw.indexed)) {
        throw std::invalid_argument("Metal mesh arguments require an indexed mesh draw");
    }
    const bool fanPath = !meshPath && !rectPath && state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    const auto primitive = meshPath || rectPath ? MTLPrimitiveTypeTriangle : PrimitiveType(state);
    const bool nativeStrip = !meshPath && !rectPath && (primitive == MTLPrimitiveTypeLineStrip || primitive == MTLPrimitiveTypeTriangleStrip);
    const bool nativeRestart = (nativeStrip || fanPath) && state.primitiveRestart;
    // Fans are expanded and strips without restart are promoted on the CPU from the indices; the mesh
    // and rectangle paths size their passes from the counts.
    if (gpuIndirect != nullptr && (meshPath || rectPath || fanPath || (nativeStrip && !nativeRestart) || draw.IndexReadCount() < draw.indexCount)) {
        throw IndirectNeedsCpuRecords("Metal GPU indirect draw path needs the records on the CPU");
    }
    std::uint32_t meshGroups = 0;
    if (meshPath) {
        const auto& mesh = *state.stages.mesh;
        // VGT_SHADER_STAGES_EN.GS_FAST_LAUNCH: the shared decode takes one point per subgroup, unindexed.
        if (((state.stages.registerValue >> 19u) & 3u) != 0 && draw.indexed) throw std::invalid_argument("Metal indexed fast-launch draws are unsupported");
        const auto inputSize = mesh.inputPrimitive == 1 ? 1u : mesh.inputPrimitive == 2 ? 2u : 3u;
        const auto step = mesh.inputPrimitive == 5 || mesh.inputPrimitive == 6 ? 1u : inputSize;
        if (draw.indexCount < inputSize || mesh.primitivesPerGroup == 0) {
            throw std::invalid_argument("Metal mesh draw contains no complete primitive");
        }
        meshGroups = ((draw.indexCount - inputSize) / step) / mesh.primitivesPerGroup + 1u;
        if (meshArguments && ((*meshArguments)[0] != meshGroups || (*meshArguments)[1] != draw.instanceCount ||
            (*meshArguments)[2] != 1 || (*meshArguments)[3] != draw.indexCount ||
            (*meshArguments)[4] > std::numeric_limits<std::uint32_t>::max() - draw.indexCount)) {
            throw std::invalid_argument("Metal mesh arguments disagree with the expanded draw");
        }
    }
    const Graphics::CompiledShader* vertex = nullptr;
    const Graphics::CompiledShader* fragment = nullptr;
    const Graphics::CompiledShader* control = nullptr;
    const Graphics::CompiledShader* evaluation = nullptr;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) throw std::invalid_argument("Metal draw compiled shader is missing");
        if (shader.stage == (meshPath ? ShaderStage::Mesh : ShaderStage::Vertex) && vertex == nullptr) vertex = &shader;
        else if (shader.stage == ShaderStage::Fragment && fragment == nullptr) fragment = &shader;
        else if (rectPath && shader.stage == ShaderStage::TessellationControl && control == nullptr) control = &shader;
        else if (rectPath && shader.stage == ShaderStage::TessellationEvaluation && evaluation == nullptr) evaluation = &shader;
        else throw std::invalid_argument("Metal draw requires one rasterization and one fragment program");
    }
    if (vertex == nullptr || fragment == nullptr) throw std::invalid_argument("Metal draw vertex or fragment program is missing");
    const bool baryPath = fragment->program->barycentricEmulation.active;
    if (baryPath && (meshPath || rectPath || state.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || draw.indexCount % 3 != 0)) {
        throw std::invalid_argument("Metal draw pixel shader needs per-vertex barycentric inputs, which Metal emulates only for complete triangle lists");
    }
    if (rectPath && (control == nullptr || evaluation == nullptr)) throw std::invalid_argument("Metal rectangle draw requires the original generated control and evaluation programs");
    if (gpuIndirect != nullptr) {
        // The vertex buffers are bound over their descriptors' whole record ranges (the indices and
        // counts are not known on the CPU), so those must be borrowed guest bytes.
        if (baryPath) throw IndirectNeedsCpuRecords("Metal GPU indirect draw cannot capture barycentric vertices");
        constexpr std::size_t MaximumBytes = std::size_t{256} << 20u;
        for (const auto& attribute : vertex->program->vertexAttributes) {
            if (Graphics::NullVertexDescriptor(attribute)) continue;
            bool bindable = false;
            try {
                const auto bytes = Graphics::VertexBufferExtent(attribute);
                const auto& fields = attribute.resource.fields;
                const auto address = fields[0] | (std::uint64_t{fields[1] & 0xffffu} << 32u);
                bindable = bytes <= MaximumBytes && NativeGuestMemory::ReadableBorrowedBytes(address, bytes) >= bytes;
            } catch (const std::exception&) {
            }
            if (!bindable) throw IndirectNeedsCpuRecords("Metal GPU indirect draw vertex buffer range is not bindable whole");
        }
        if (draw.indexed && ((draw.indexSize != 2 && draw.indexSize != 4) || draw.indexAddress % draw.indexSize != 0 ||
            NativeGuestMemory::ReadableBorrowedBytes(draw.indexAddress, std::uint64_t{draw.indexCount} * draw.indexSize) < std::uint64_t{draw.indexCount} * draw.indexSize)) {
            throw IndirectNeedsCpuRecords("Metal GPU indirect draw index buffer is not bindable whole");
        }
        if (NativeGuestMemory::ReadableBorrowedBytes(gpuIndirect->arguments, gpuIndirect->RangeBytes()) < gpuIndirect->RangeBytes()) {
            throw IndirectNeedsCpuRecords("Metal GPU indirect draw records are not borrowed guest bytes");
        }
    }
    auto pushConstants = Graphics::AssemblePushConstants(shaders);
    id<MTLBuffer> meshArgumentBuffer = nil;
    if (meshPath) {
        for (const auto& shader : shaders) {
            if (!shader.program->pushConstants.empty() &&
                shader.pushConstantOffset + shader.program->pushConstants.size() > MeshDrawPushOffsetBytes) {
                throw std::invalid_argument("Metal mesh shader data overlaps the draw push constants");
            }
        }
        std::uint64_t argumentAddress = 0;
        if (meshArguments) {
            static_assert(sizeof(*meshArguments) == MeshArgumentBytes);
            meshArgumentBuffer = backend.Buffer(MeshArgumentBytes);
            std::memcpy(meshArgumentBuffer.contents, meshArguments->data(), MeshArgumentBytes);
            argumentAddress = meshArgumentBuffer.gpuAddress;
            if (argumentAddress == 0 || argumentAddress % 4 != 0) {
                throw std::runtime_error("Metal mesh argument buffer has no aligned GPU address");
            }
        }
        const std::array<std::uint32_t, MeshDrawPushBytes / 4> words{
            draw.indexCount, draw.firstVertex, draw.firstInstance, draw.indexed ? draw.indexSize : 0u,
            static_cast<std::uint32_t>(argumentAddress), static_cast<std::uint32_t>(argumentAddress >> 32u)};
        // Upstream places the mesh draw words at the end of the first push slot.
        static_assert(MeshDrawPushOffsetBytes + MeshDrawPushBytes == Graphics::PipelinePushSlotBytes);
        std::memcpy(pushConstants.data() + MeshDrawPushOffsetBytes, words.data(), sizeof(words));
    }
    auto depth = state.depth ? depthCache->Acquire(*state.depth) : nullptr;
    if (depth) {
        // The surface bound last at an address is the one a view of it reads: it goes last, and the
        // depth surfaces its planes overlap (another stencil pairing of its depth plane, another extent
        // at its address) are retired until they are bound again.
        std::erase(depthSurfaces, depth);
        RetireDepthSurfaces(depthSurfaces, depth->Target());
        depthSurfaces.push_back(depth);
    }
    // A color target over a depth surface's memory writes where its planes were (all its fragments).
    for (const auto& color : state.colors) RetireDepthSurfaces(depthSurfaces, color, depth.get());
    // A depth-only draw that exports a render target index renders into slices slice..lastSlice of its
    // depth array at once; each slice also gets its surface, so later single-slice and array readers find it.
    const std::uint32_t depthLayers = depth && state.layered && state.colors.empty() && state.depth->samples == 1 ?
        state.depth->lastSlice - state.depth->slice + 1u : 0u;
    if (depthLayers > 1) {
        for (std::uint32_t layer = 1; layer < depthLayers; ++layer) {
            const auto slice = DepthArraySlice(*state.depth, layer);
            auto surface = depthCache->Acquire(slice);
            RetireDepthSurfaces(depthSurfaces, slice, surface.get());
            if (std::find(depthSurfaces.begin(), depthSurfaces.end(), surface) == depthSurfaces.end()) depthSurfaces.push_back(surface);
        }
    }
    auto resourcesOwner = std::make_shared<MetalShaderResources>(backend, ranges,
        [this](const Graphics::GuestTextureResource& resource, bool storage, bool compare) {
            return residentTexture(resource, storage, compare);
        }, hostAliases.get(), ImageCache());
    auto& resources = *resourcesOwner;
    if (cpuReadHook) resources.SetCpuReadHook(cpuReadHook);
    resources.SetLazyCopyBacks(&lazyCopyBacks);
    if (materializeHook) resources.SetMaterializeHook(materializeHook);
    if (timeline != nullptr) resources.SetPendingStores(MetalShaderResources::PendingStoresOf(*timeline));
    auto pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    if (!meshPath) ConfigureRenderPipelineDescriptor(pipelineDescriptor, state);
    auto renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPass.renderTargetWidth = state.renderExtent.width;
    renderPass.renderTargetHeight = state.renderExtent.height;
    ConfigureSamplePositions(renderPass, backend.Device(), state);
    struct Expansion {
        id<MTLTexture> source;
        std::uint32_t slot;
    };
    std::vector<Expansion> expansions;
    for (const auto& color : state.colors) {
        if (color.exportIndex >= 8) throw std::invalid_argument("Metal draw color export slot is invalid");
        auto attachment = renderPass.colorAttachments[color.exportIndex];
        attachment.loadAction = MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionStore;
        if (color.samples > 1) {
            if (color.samples != state.rasterSamples) throw std::invalid_argument("Metal draw multisampled color target disagrees with the pass sample count");
            auto surface = depthCache->Acquire(color);
            if (std::find(colorSurfaces.begin(), colorSurfaces.end(), surface) == colorSurfaces.end()) colorSurfaces.push_back(surface);
            attachment.texture = surface->Texture();
            continue;
        }
        auto texture = resources.Texture(colorSurface(color), true, false, false, color.mip);
        // The surface is typed by its memory layout; the attachment renders through the guest state's format.
        id<MTLTexture> handle = ColorAttachmentView(texture->Texture(), RenderPixelFormat(color.format));
        if (state.rasterSamples == 1) {
            attachment.texture = handle;
            attachment.level = color.mip;
            attachment.depthPlane = color.depthSlice;
            continue;
        }
        // A single-fragment target in a multisampled pass: expand it into a transient multisample
        // texture before the draw and resolve the samples back into the target after it.
        if (color.depth != 1 || color.uintExport || color.packing != ShaderRecompiler::ColorExportPacking::None) {
            throw std::invalid_argument("Metal draw cannot resolve a 3D, integer or packed single-fragment color target from a multisampled pass");
        }
        switch (handle.pixelFormat) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
        case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint:
            throw std::invalid_argument("Metal draw cannot resolve an integer single-fragment color target from a multisampled pass");
        default:
            break;
        }
        id<MTLTexture> level = [handle newTextureViewWithPixelFormat:handle.pixelFormat textureType:MTLTextureType2D
            levels:NSMakeRange(color.mip, 1) slices:NSMakeRange(0, 1)];
        if (level == nil) throw std::runtime_error("Metal draw color level view creation failed");
        expansions.push_back({level, color.exportIndex});
        attachment.resolveTexture = level;
        attachment.storeAction = MTLStoreActionMultisampleResolve;
    }
    if (depth && depthLayers > 1) {
        id<MTLTexture> layeredDepth = depth->LayeredDepthTexture(depthLayers);
        renderPass.renderTargetArrayLength = depthLayers;
        renderPass.depthAttachment.texture = layeredDepth;
        renderPass.depthAttachment.loadAction = MTLLoadActionLoad;
        renderPass.depthAttachment.storeAction = MTLStoreActionStore;
        if (depth->StencilTexture() != nil) {
            id<MTLTexture> layeredStencil = depth->LayeredStencilTexture(depthLayers);
            renderPass.stencilAttachment.texture = layeredStencil != nil ? layeredStencil : layeredDepth;
            renderPass.stencilAttachment.loadAction = MTLLoadActionLoad;
            renderPass.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    } else if (depth) {
        renderPass.depthAttachment.texture = depth->DepthTexture();
        renderPass.depthAttachment.loadAction = MTLLoadActionLoad;
        renderPass.depthAttachment.storeAction = MTLStoreActionStore;
        if (depth->StencilTexture() != nil) {
            renderPass.stencilAttachment.texture = depth->StencilTexture();
            renderPass.stencilAttachment.loadAction = MTLLoadActionLoad;
            renderPass.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    }
    MetalBufferBinding indexBuffer;
    std::uint32_t nativeIndexSize = draw.indexSize;
    std::uint32_t maxIndex = 0;
    bool noVertexIndices = false;
    // Vertex buffers bound over their descriptors' whole record ranges, instead of up to the
    // largest index (WholeVertexReadSize): the indices were not read on the CPU.
    bool wholeVertexRanges = false;
    std::vector<std::uint32_t> fanIndices;
    std::uint32_t fanCenter = 0, fanPrevious = 0, fanVertexCount = 0;
    const auto appendFanVertex = [&](std::uint32_t index) {
        if (fanVertexCount == 0) fanCenter = index;
        else if (fanVertexCount >= 2) {
            if (backend.Device().maxBufferLength / sizeof(std::uint32_t) < 3 ||
                fanIndices.size() > backend.Device().maxBufferLength / sizeof(std::uint32_t) - 3) {
                throw std::invalid_argument("Metal triangle fan expansion exceeds the device index buffer limit");
            }
            fanIndices.insert(fanIndices.end(), {fanPrevious, index, fanCenter});
        }
        fanPrevious = index;
        if (fanVertexCount < 2) ++fanVertexCount;
    };
    if (draw.indexed) {
        if ((draw.indexSize != 2 && draw.indexSize != 4) || draw.indexAddress % draw.indexSize != 0) {
            throw std::invalid_argument("Metal draw index format or address is invalid");
        }
        const auto bytes = std::uint64_t{draw.indexCount} * draw.indexSize;
        if (bytes > std::numeric_limits<std::size_t>::max()) throw std::invalid_argument("Metal draw index byte count overflows");
        const auto readable = draw.IndexReadCount();
        if (readable > draw.indexCount) throw std::invalid_argument("Metal draw readable index count exceeds the draw count");
        // Indices that committed GPU work may still be writing (GPU culling output) are not read on
        // the CPU, which would wait for that work: the scan only bounds the vertex buffer ranges
        // here, so they are bound whole instead. Fans (expanded on the CPU), strips without restart
        // (an index 0xffff or 0xffffffff must be told apart from Metal's restart), mesh fans and
        // bounded fetches (copied on the CPU) still read them.
        const bool cpuNeedsIndices = fanPath || (nativeStrip && !nativeRestart) ||
            (meshPath && state.stages.mesh->inputPrimitive == 5 && state.primitiveRestart) || readable < draw.indexCount;
        bool skipScan = gpuIndirect == nullptr && !cpuNeedsIndices && timeline != nullptr &&
            timeline->MayHaveWritten(draw.indexAddress, draw.indexAddress + bytes) &&
            WholeVertexRangesBindable(vertex->program->vertexAttributes, draw);
        if (readable < draw.indexCount) {
            // A direct DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2 whose count passes its max_size (upstream PR
            // 4824): the first max_size indices come from guest memory and the remaining fetches read
            // zero, from a buffer of the draw's own.
            if (meshPath) throw std::invalid_argument("Metal bounded index fetches on a mesh path are unsupported");
            const auto readBytes = std::uint64_t{readable} * draw.indexSize;
            id<MTLBuffer> padded = backend.Buffer(static_cast<std::size_t>(bytes));
            auto* destination = static_cast<std::byte*>(padded.contents);
            if (readBytes != 0) {
                if (cpuReadHook) cpuReadHook(draw.indexAddress, readBytes);
                const auto source = resources.Buffer(draw.indexAddress, static_cast<std::size_t>(readBytes));
                std::memcpy(destination, static_cast<const std::byte*>(source.buffer.contents) + source.offset, static_cast<std::size_t>(readBytes));
            }
            std::memset(destination + readBytes, 0, static_cast<std::size_t>(bytes - readBytes));
            indexBuffer = {padded, 0, padded.length};
        } else if (gpuIndirect != nullptr) {
            // The GPU reads the indices the records select; nothing here reads them.
            indexBuffer = resources.Buffer(draw.indexAddress, static_cast<std::size_t>(bytes));
        } else {
            if (cpuReadHook && !skipScan) cpuReadHook(draw.indexAddress, bytes);
            indexBuffer = resources.Buffer(draw.indexAddress, static_cast<std::size_t>(bytes));
            // Only a no-copy alias of the guest bytes sees the producer's stores; a copied snapshot
            // holds the bytes from before it, so such a draw waits and scans as before.
            if (skipScan && static_cast<const std::byte*>(indexBuffer.buffer.contents) + indexBuffer.offset !=
                    NativeGuestMemory::ContiguousBorrowedRange(draw.indexAddress, static_cast<std::size_t>(bytes), false).data()) {
                skipScan = false;
                if (cpuReadHook) cpuReadHook(draw.indexAddress, bytes);
            }
        }
        wholeVertexRanges = skipScan;
    }
    if (draw.indexed && !wholeVertexRanges) {
        std::uint32_t minIndex = std::numeric_limits<std::uint32_t>::max();
        const auto* data = static_cast<const std::byte*>(indexBuffer.buffer.contents) + indexBuffer.offset;
        const auto restartIndex = draw.indexSize == 2 ? 0xffffu : 0xffffffffu;
        bool hasVertexIndex = gpuIndirect != nullptr;
        for (std::uint32_t i = 0; gpuIndirect == nullptr && i < draw.indexCount; ++i) {
            std::uint32_t index = 0;
            if (draw.indexSize == 2) { std::uint16_t value; std::memcpy(&value, data + std::size_t{i} * 2u, 2); index = value; }
            else std::memcpy(&index, data + std::size_t{i} * 4u, 4);
            if (nativeRestart && index == restartIndex) {
                if (fanPath) fanVertexCount = 0;
                continue;
            }
            if (fanPath) appendFanVertex(index);
            hasVertexIndex = true;
            minIndex = std::min(minIndex, index);
            maxIndex = std::max(maxIndex, index);
        }
        noVertexIndices = !hasVertexIndex;
        if (nativeStrip && !nativeRestart &&
            maxIndex == std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Metal draw cannot represent the guest UINT32_MAX vertex index without native primitive restart");
        }
        if (nativeStrip && !nativeRestart &&
            draw.indexSize == 2 && maxIndex == std::numeric_limits<std::uint16_t>::max()) {
            id<MTLBuffer> promoted = backend.Buffer(std::size_t{draw.indexCount} * sizeof(std::uint32_t));
            auto* destination = static_cast<std::uint32_t*>(promoted.contents);
            for (std::uint32_t i = 0; i < draw.indexCount; ++i) {
                std::uint16_t value;
                std::memcpy(&value, data + std::size_t{i} * sizeof(value), sizeof(value));
                destination[i] = value;
            }
            indexBuffer = {promoted, 0, promoted.length};
            nativeIndexSize = 4;
        }
        if (meshPath && state.stages.mesh->inputPrimitive == 5 && state.primitiveRestart &&
            maxIndex == (draw.indexSize == 2 ? 0xffffu : 0xffffffffu)) {
            throw std::invalid_argument("Metal triangle fan geometry draw primitive restart is unsupported");
        }
        if (!noVertexIndices && gpuIndirect == nullptr) {
            const std::int64_t baseVertex = meshPath ? std::int64_t{draw.firstVertex} : std::bit_cast<std::int32_t>(draw.firstVertex);
            const auto firstIndex = std::int64_t{minIndex} + baseVertex;
            const auto lastIndex = std::int64_t{maxIndex} + baseVertex;
            if (firstIndex < 0 || lastIndex > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("Metal draw base vertex overflows guest invocation index");
            }
            maxIndex = static_cast<std::uint32_t>(lastIndex);
        }
    } else if (!draw.indexed && gpuIndirect != nullptr) {
        if (draw.indexAddress != 0 || draw.indexSize != 0) throw std::invalid_argument("Metal draw auto-index parameters are invalid");
    } else if (!draw.indexed) {
        if (draw.indexAddress != 0 || draw.indexSize != 0 || draw.firstVertex > std::numeric_limits<std::uint32_t>::max() - (draw.indexCount - 1u)) {
            throw std::invalid_argument("Metal draw auto-index parameters are invalid");
        }
        maxIndex = draw.firstVertex + draw.indexCount - 1u;
        if (fanPath) for (std::uint32_t i = 0; i < draw.indexCount; ++i) appendFanVertex(draw.firstVertex + i);
    }
    NSUInteger nativeIndexCount = draw.indexCount;
    if (fanPath) {
        nativeIndexCount = fanIndices.size();
        if (!fanIndices.empty()) {
            auto expanded = backend.Buffer(fanIndices.size() * sizeof(std::uint32_t));
            std::memcpy(expanded.contents, fanIndices.data(), expanded.length);
            indexBuffer = {expanded, 0, expanded.length};
            nativeIndexSize = 4;
        }
    }
    const auto& attributes = vertex->program->vertexAttributes;
    if (meshPath && !attributes.empty()) throw std::invalid_argument("Metal mesh draw requires shader-based vertex fetch");
    const auto buildLayout = [](std::span<const ShaderRecompiler::VertexAttribute> input) {
        return Graphics::BuildVertexInputLayout(input, 29, 31, 2048,
            [](VkFormat format) { return vertexFormat(format) != MTLVertexFormatInvalid; });
    };
    // Kept per vertex shader variant instead of rebuilt every draw (upstream #1316).
    static Graphics::VertexInputCache vertexInputs(buildLayout);
    auto retainedLayout = vertexInputs.TryGet(vertex->program->variantId, attributes);
    std::optional<Graphics::VertexInputLayout> builtLayout;
    if (retainedLayout == nullptr) builtLayout = buildLayout(attributes);
    const auto& layout = retainedLayout != nullptr ? *retainedLayout : *builtLayout;
    auto vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    std::vector<MetalBufferBinding> vertexBuffers;
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        const auto& attribute = attributes[i];
        const auto& fields = attribute.resource.fields;
        const auto address = fields[0] | (std::uint64_t{fields[1] & 0xffffu} << 32u);
        std::size_t adjustment = 0;
        if (!noVertexIndices && Graphics::NullVertexDescriptor(attribute)) {
            // An all-zero V# fetches zeros (upstream's Vulkan draw binds a zero buffer the same way).
            auto zero = backend.Buffer(Graphics::DecodeVertexFormat(attribute).bytes);
            std::memset(zero.contents, 0, zero.length);
            vertexBuffers.push_back({zero, 0, zero.length});
        } else if (!noVertexIndices) {
            auto buffer = resources.Buffer(address, gpuIndirect != nullptr ? Graphics::VertexBufferExtent(attribute) : wholeVertexRanges ?
                Graphics::WholeVertexReadSize(attribute, draw.instanceCount, draw.firstInstance) :
                Graphics::VertexBufferReadSize(attribute, maxIndex, draw.instanceCount, draw.firstInstance));
            adjustment = buffer.offset % 4;
            buffer.offset -= adjustment;
            vertexBuffers.push_back(buffer);
        }
        auto nativeAttribute = vertexDescriptor.attributes[attribute.location];
        nativeAttribute.format = vertexFormat(layout.attributes[i].format);
        nativeAttribute.bufferIndex = i;
        nativeAttribute.offset = adjustment;
        auto nativeLayout = vertexDescriptor.layouts[i];
        nativeLayout.stride = layout.bindings[i].stride ? layout.bindings[i].stride : Graphics::DecodeVertexFormat(attribute).bytes;
        nativeLayout.stepFunction = layout.bindings[i].stride == 0 ? MTLVertexStepFunctionConstant :
            (attribute.fetchIndex == 0 ? MTLVertexStepFunctionPerVertex : MTLVertexStepFunctionPerInstance);
        nativeLayout.stepRate = layout.bindings[i].stride == 0 ? 0 : 1;
    }
    pipelineDescriptor.vertexDescriptor = rectPath || baryPath ? nil : vertexDescriptor;
    MetalBackend::TargetOptions target;
    if (@available(macOS 15.0, *))
        target.supportsWorkgroupAtomicFences = [backend.Device() supportsFamily:MTLGPUFamilyApple1];
    target.supportsInt64 = true;
    target.supportsGpuAddresses = true;
    target.supportsSimdGroups = true;
    target.vertexBufferCount = static_cast<std::uint32_t>(attributes.size());
    target.pushConstantOffsetBytes = vertex->pushConstantOffset;
    target.flipVertexY = state.viewport.height > 0;
    target.fixupClipSpace = state.negativeOneToOne;
    const bool capturePath = rectPath || baryPath;
    target.rectListMode = capturePath ? MetalBackend::RectListMode::VertexCapture : MetalBackend::RectListMode::None;
    target.rectListIndexSize = capturePath && draw.indexed ? nativeIndexSize : 0;
    configureSamplerArguments(target, *vertex->program, backend.Device());
    auto nativeVertex = MetalBackend::ConvertToMetal(*vertex->program, meshPath ? ShaderStage::Mesh : ShaderStage::Vertex, target);
    bool bdaStores = StoresThroughBda(nativeVertex);
    std::unique_ptr<MetalRectKernelPipeline> capturePipeline;
    std::unique_ptr<MetalRectKernelPipeline> controlPipeline;
    id<MTLBuffer> rectCapture = nil;
    id<MTLBuffer> rectOutput = nil;
    id<MTLBuffer> rectFactors = nil;
    id<MTLBuffer> rectParameters = nil;
    std::uint32_t rectPatches = 0;
    id<MTLBuffer> baryCapture = nil;
    std::uint32_t baryTriangles = 0;
    std::vector<MetalImplicitBufferBinding> evaluationBuffers;
    if (capturePath) {
        const auto patches = std::uint64_t{draw.indexCount / 3} * draw.instanceCount;
        if (patches > std::numeric_limits<std::uint32_t>::max() / 4u) throw std::invalid_argument("Metal rectangle patch grid exceeds shader integer range");
        (rectPath ? rectPatches : baryTriangles) = static_cast<std::uint32_t>(patches);
        auto inputs = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        inputs.indexType = nativeIndexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        inputs.indexBufferIndex = target.rectListBuffers.indices;
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            auto input = inputs.attributes[attributes[i].location];
            auto source = vertexDescriptor.attributes[attributes[i].location];
            input.format = static_cast<MTLAttributeFormat>(source.format);
            input.offset = source.offset;
            input.bufferIndex = i;
            auto inputLayout = inputs.layouts[i];
            auto sourceLayout = vertexDescriptor.layouts[i];
            inputLayout.stride = sourceLayout.stride;
            inputLayout.stepRate = sourceLayout.stepRate;
            inputLayout.stepFunction = sourceLayout.stepFunction == MTLVertexStepFunctionConstant ? MTLStepFunctionConstant :
                attributes[i].fetchIndex == 0 ? (draw.indexed ? MTLStepFunctionThreadPositionInGridXIndexed : MTLStepFunctionThreadPositionInGridX) : MTLStepFunctionThreadPositionInGridY;
        }
        capturePipeline = std::make_unique<MetalRectKernelPipeline>(backend.Device(), std::move(nativeVertex), inputs);
        const auto& captured = capturePipeline->Reflection();
        if (baryPath) {
            baryCapture = backend.Buffer(std::uint64_t{baryTriangles} * 3u * captured.rectList->outputLayout.stride);
        }
    }
    if (rectPath) {
        const auto& captured = capturePipeline->Reflection();
        target.vertexBufferCount = 0;
        target.pushConstantOffsetBytes = control->pushConstantOffset;
        target.rectListMode = MetalBackend::RectListMode::Control;
        target.rectListIndexSize = 0;
        target.rectListInputLayout = captured.rectList->outputLayout;
        configureSamplerArguments(target, *control->program, backend.Device());
        auto nativeControl = MetalBackend::ConvertToMetal(*control->program, ShaderStage::TessellationControl, target);
        controlPipeline = std::make_unique<MetalRectKernelPipeline>(backend.Device(), std::move(nativeControl));
        const auto& controlled = controlPipeline->Reflection();
        rectCapture = backend.Buffer(std::uint64_t{rectPatches} * 3u * captured.rectList->outputLayout.stride);
        rectOutput = backend.Buffer(std::uint64_t{rectPatches} * 4u * controlled.rectList->outputLayout.stride);
        rectFactors = backend.Buffer(std::uint64_t{rectPatches} * sizeof(MTLQuadTessellationFactorsHalf));
        rectParameters = backend.Buffer(2 * sizeof(std::uint32_t));
        const std::array<std::uint32_t, 2> parameters{3, rectPatches};
        std::memcpy(rectParameters.contents, parameters.data(), sizeof(parameters));
        target.pushConstantOffsetBytes = evaluation->pushConstantOffset;
        target.rectListMode = MetalBackend::RectListMode::Evaluation;
        target.rectListInputLayout = controlled.rectList->outputLayout;
        configureSamplerArguments(target, *evaluation->program, backend.Device());
        nativeVertex = MetalBackend::ConvertToMetal(*evaluation->program, ShaderStage::TessellationEvaluation, target);
        evaluationBuffers.push_back({rectSlot(nativeVertex, MetalBackend::ImplicitBufferRole::StageInput), {rectOutput, 0, rectOutput.length}});
    }
    target.vertexBufferCount = 0;
    target.pushConstantOffsetBytes = fragment->pushConstantOffset;
    target.rectListMode = MetalBackend::RectListMode::None;
    target.rectListIndexSize = 0;
    target.rectListInputLayout.reset();
    configureSamplerArguments(target, *fragment->program, backend.Device());
    auto nativeFragment = MetalBackend::ConvertToMetal(*fragment->program, ShaderStage::Fragment, target);
    bdaStores = bdaStores || StoresThroughBda(nativeFragment);
    if (baryPath) {
        MetalBackend::Result corners;
        corners.stage = ShaderStage::Vertex;
        corners.nativeExecutionKind = MetalBackend::NativeExecutionKind::Vertex;
        corners.entryPoint = BarycentricVertexEntry;
        corners.source = barycentricVertexSource(capturePipeline->Reflection().rectList->outputLayout, nativeFragment.source,
            *fragment->program, state.viewport.height > 0, state.negativeOneToOne);
        nativeVertex = std::move(corners);
    }
    std::unique_ptr<MetalRenderPipeline> vertexPipeline;
    std::unique_ptr<MetalMeshPipeline> meshPipeline;
    if (meshPath) {
        auto descriptor = [[MTLMeshRenderPipelineDescriptor alloc] init];
        ConfigureRenderPipelineDescriptor(descriptor, state);
        meshPipeline = std::make_unique<MetalMeshPipeline>(backend.Device(), std::move(nativeVertex), std::move(nativeFragment), descriptor);
        meshPipeline->ValidateThreadgroups(MTLSizeMake(meshGroups, draw.instanceCount, 1));
    } else {
        vertexPipeline = std::make_unique<MetalRenderPipeline>(backend.Device(), std::move(nativeVertex), std::move(nativeFragment), pipelineDescriptor);
    }
    const auto& mainReflection = meshPath ? meshPipeline->MeshReflection() : vertexPipeline->VertexReflection();
    const auto& fragmentReflection = meshPath ? meshPipeline->FragmentReflection() : vertexPipeline->FragmentReflection();
    auto vertexBindings = resources.Bindings(mainReflection);
    auto fragmentBindings = resources.Bindings(fragmentReflection);
    // Every binding is resolved before the command buffer is taken: resolving may upload, read
    // guest memory or wait for the GPU, and with batching (MetalDevice::SetBatching) a wait commits
    // the shared command buffer, which nothing may encode into afterwards.
    std::vector<MetalShaderResourceBinding> captureBindings;
    std::vector<MetalShaderResourceBinding> controlBindings;
    if (baryPath || rectPath) captureBindings = resources.Bindings(capturePipeline->Reflection());
    if (rectPath) controlBindings = resources.Bindings(controlPipeline->Reflection());
    MetalBufferBinding indirectRecords;
    NSUInteger indirectStride = gpuIndirect != nullptr ? gpuIndirect->stride : 0;
    if (gpuIndirect != nullptr) indirectRecords = resources.Buffer(gpuIndirect->arguments, static_cast<std::size_t>(gpuIndirect->RangeBytes()));
    // Nothing to draw: decided before any pass is encoded, so no capture pass runs for it.
    if (noVertexIndices || (fanPath && fanIndices.empty())) return {};
    // The vertex capture and rectangle control passes go into the draw's own command buffer: the
    // queue runs them before the draw, and Complete reads the one fault buffer all passes share
    // (a fault in any pass reports the work as faulted, and it runs again or fails like the draw).
    id<MTLCommandBuffer> commands = backend.CommandBuffer();
    MetalBatchedWork batched(backend, commands);
    if (gpuIndirect != nullptr) {
        using Rule = Pm4::DrawParameters::IndirectDraw::Rule;
        if (gpuIndirect->vertexRule == Rule::Constant || gpuIndirect->instanceRule == Rule::Constant) {
            // A dimension whose CP register write no shader reads (Rule::Constant) draws with the
            // driver's constant, not the record's dword: the GPU copies each record's other dwords
            // (written by earlier GPU work) next to the constants, and the draws read the copies.
            const auto recordBytes = gpuIndirect->recordBytes;
            id<MTLBuffer> rewritten = backend.TransientBuffer(std::size_t{gpuIndirect->count} * recordBytes, commands);
            auto* words = static_cast<std::uint32_t*>(rewritten.contents);
            std::memset(words, 0, rewritten.length);
            const auto vertexWord = gpuIndirect->VertexDwordOffset() / 4u;
            const auto instanceWord = gpuIndirect->InstanceDwordOffset() / 4u;
            for (std::uint32_t record = 0; record < gpuIndirect->count; ++record) {
                if (gpuIndirect->vertexRule == Rule::Constant) words[record * (recordBytes / 4u) + vertexWord] = gpuIndirect->vertexConstant;
                if (gpuIndirect->instanceRule == Rule::Constant) words[record * (recordBytes / 4u) + instanceWord] = gpuIndirect->instanceConstant;
            }
            auto blit = MetalEncoders::Blit(commands);
            if (blit == nil) throw MetalGpuExecutionError("Metal indirect record blit encoder allocation failed");
            for (std::uint32_t record = 0; record < gpuIndirect->count; ++record) {
                const auto source = indirectRecords.offset + NSUInteger{record} * gpuIndirect->stride;
                const auto destination = NSUInteger{record} * recordBytes;
                // Count, instances and first index or vertex always come from the record.
                [blit copyFromBuffer:indirectRecords.buffer sourceOffset:source toBuffer:rewritten destinationOffset:destination size:12];
                if (gpuIndirect->vertexRule == Rule::InPlace) {
                    [blit copyFromBuffer:indirectRecords.buffer sourceOffset:source + vertexWord * 4u toBuffer:rewritten
                        destinationOffset:destination + vertexWord * 4u size:4];
                }
                if (gpuIndirect->instanceRule == Rule::InPlace) {
                    [blit copyFromBuffer:indirectRecords.buffer sourceOffset:source + instanceWord * 4u toBuffer:rewritten
                        destinationOffset:destination + instanceWord * 4u size:4];
                }
            }
            [blit endEncoding];
            indirectRecords = {rewritten, 0, rewritten.length};
            indirectStride = recordBytes;
        }
    }
    if (baryPath) {
        const auto& captured = capturePipeline->Reflection();
        std::vector<MetalImplicitBufferBinding> captureBuffers{{rectSlot(captured, MetalBackend::ImplicitBufferRole::StageOutput), {baryCapture, 0, baryCapture.length}}};
        if (draw.indexed) captureBuffers.push_back({rectSlot(captured, MetalBackend::ImplicitBufferRole::Indices), indexBuffer});
        capturePipeline->Encode(commands, captureBindings, MTLRegionMake3D(draw.firstVertex, draw.firstInstance, 0, draw.indexCount, draw.instanceCount, 1),
            captureBuffers, vertexBuffers, pushBlock(captured, pushConstants), resources.Residency());
        requireFaultBinding(captured, captureBindings, "Metal per-vertex capture fault binding is missing", false);
    }
    if (rectPath) {
        const auto& captured = capturePipeline->Reflection();
        const auto& controlled = controlPipeline->Reflection();
        std::vector<MetalImplicitBufferBinding> captureBuffers{{rectSlot(captured, MetalBackend::ImplicitBufferRole::StageOutput), {rectCapture, 0, rectCapture.length}}};
        if (draw.indexed) captureBuffers.push_back({rectSlot(captured, MetalBackend::ImplicitBufferRole::Indices), indexBuffer});
        const std::array<MetalImplicitBufferBinding, 4> controlBuffers{{
            {rectSlot(controlled, MetalBackend::ImplicitBufferRole::StageInput), {rectCapture, 0, rectCapture.length}},
            {rectSlot(controlled, MetalBackend::ImplicitBufferRole::StageOutput), {rectOutput, 0, rectOutput.length}},
            {rectSlot(controlled, MetalBackend::ImplicitBufferRole::TessellationFactors), {rectFactors, 0, rectFactors.length}},
            {rectSlot(controlled, MetalBackend::ImplicitBufferRole::IndirectParameters), {rectParameters, 0, rectParameters.length}}}};
        capturePipeline->Encode(commands, captureBindings, MTLRegionMake3D(draw.firstVertex, draw.firstInstance, 0, draw.indexCount, draw.instanceCount, 1),
            captureBuffers, vertexBuffers, pushBlock(captured, pushConstants), resources.Residency());
        controlPipeline->Encode(commands, controlBindings, MTLRegionMake3D(0, 0, 0, std::uint64_t{rectPatches} * 4u, 1, 1),
            controlBuffers, {}, pushBlock(controlled, pushConstants), resources.Residency());
        requireFaultBinding(controlled, controlBindings, "Metal rectangle control requires its original fault buffer");
    }
    auto residency = resources.Residency();
    std::vector<id<MTLResource>> meshResidency;
    if (meshArgumentBuffer != nil) {
        meshResidency.assign(residency.begin(), residency.end());
        meshResidency.push_back(meshArgumentBuffer);
        residency = meshResidency;
    }
    auto depthStencilState = CreateDepthStencilState(backend.Device(), state);
    if (sampleCounter != nil) {
        std::memset(static_cast<std::byte*>(sampleCounter.contents) + 16, 0, 8);
        renderPass.visibilityResultBuffer = sampleCounter;
    }
    if (!expansions.empty()) {
        if (!multisampleExpander) multisampleExpander = std::make_unique<MetalMultisampleExpander>(backend);
        for (const auto& expansion : expansions) {
            renderPass.colorAttachments[expansion.slot].texture = multisampleExpander->Expand(commands, expansion.source, state.rasterSamples, expansion.slot);
        }
    }
    // Render-pass merging (MetalEncoders): the draw continues the pass the previous draw left open
    // when it has the same attachments, and leaves its own open for the next. Not when the draw
    // writes memory through its shaders (a later draw of the pass could read it unordered), counts
    // samples, expands MSAA targets, uses custom sample locations (merging those changed pixels from
    // run to run) or samples one of its own attachments.
    const auto passKey = MetalEncoders::KeyOf(renderPass);
    bool mergeable = sampleCounter == nil && expansions.empty() && !bdaStores && !(state.customSampleLocations && state.rasterSamples > 1) &&
        !resources.WritesBesides(passKey);
    // The bindings hold every texture the shaders read or write (the residency also holds the
    // draw's own color targets).
    for (const auto* list : {&vertexBindings, &fragmentBindings}) {
        for (const auto& binding : *list) {
            for (id<MTLTexture> texture : binding.textures) mergeable = mergeable && !MetalEncoders::IsAttachment(passKey, texture);
        }
    }
    auto encoder = MetalEncoders::RenderMerging(backend, commands, renderPass, passKey, mergeable);
    if (encoder == nil) throw MetalGpuExecutionError("Metal draw render encoder allocation failed");
    try {
        if (meshPath) {
            meshPipeline->Bind(encoder, vertexBindings, fragmentBindings,
                pushBlock(mainReflection, pushConstants), pushBlock(fragmentReflection, pushConstants), residency);
        } else {
            vertexPipeline->Bind(encoder, vertexBindings, fragmentBindings,
                pushBlock(mainReflection, pushConstants), pushBlock(fragmentReflection, pushConstants), residency, evaluationBuffers, rectPatches);
        }
        BindRenderState(encoder, state, depthStencilState);
        if (sampleCounter != nil) [encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:16];
        if (baryPath) [encoder setVertexBuffer:baryCapture offset:0 atIndex:0];
        else if (!rectPath) for (std::size_t i = 0; i < vertexBuffers.size(); ++i) [encoder setVertexBuffer:vertexBuffers[i].buffer offset:vertexBuffers[i].offset atIndex:i];
        if (meshPath) {
            const auto threads = mainReflection.threadsPerThreadgroup;
            [encoder drawMeshThreadgroups:MTLSizeMake(meshGroups, draw.instanceCount, 1)
                threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                threadsPerMeshThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
        } else if (baryPath) {
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:NSUInteger{baryTriangles} * 3u];
        } else if (rectPath) {
            [encoder setTessellationFactorBuffer:rectFactors offset:0 instanceStride:0];
            [encoder drawPatches:4 patchStart:0 patchCount:rectPatches patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        } else if (gpuIndirect != nullptr) {
            // One Metal draw per record: MTLDrawIndexedPrimitivesIndirectArguments and
            // MTLDrawPrimitivesIndirectArguments have the CP's record layouts (indexed: count, instances,
            // first index, base vertex, first instance; auto: count, instances, first vertex, first instance).
            if (indirectRecords.offset % 4 != 0) throw std::invalid_argument("Metal draw indirect records are misaligned");
            for (std::uint32_t record = 0; record < gpuIndirect->count; ++record) {
                const auto offset = indirectRecords.offset + NSUInteger{record} * indirectStride;
                if (draw.indexed) {
                    [encoder drawIndexedPrimitives:primitive indexType:nativeIndexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                        indexBuffer:indexBuffer.buffer indexBufferOffset:indexBuffer.offset indirectBuffer:indirectRecords.buffer indirectBufferOffset:offset];
                } else {
                    [encoder drawPrimitives:primitive indirectBuffer:indirectRecords.buffer indirectBufferOffset:offset];
                }
            }
        } else if (draw.indexed || fanPath) {
            [encoder drawIndexedPrimitives:primitive indexCount:nativeIndexCount indexType:nativeIndexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                indexBuffer:indexBuffer.buffer indexBufferOffset:indexBuffer.offset instanceCount:draw.instanceCount baseVertex:draw.indexed ? static_cast<NSInteger>(std::bit_cast<std::int32_t>(draw.firstVertex)) : 0 baseInstance:draw.firstInstance];
        } else {
            [encoder drawPrimitives:primitive vertexStart:draw.firstVertex vertexCount:draw.indexCount instanceCount:draw.instanceCount baseInstance:draw.firstInstance];
        }
    } catch (...) {
        [encoder endEncoding];
        throw;
    }
    MetalEncoders::EndOrPark(backend, commands, encoder, passKey, mergeable);
    if (sampleCounter != nil) {
        const SampleCounterParameters parameters{1, 0};
        backend.Encode(commands, @"SampleCounter", @[sampleCounter, sampleTarget], &parameters, sizeof(parameters), MTLSizeMake(1, 1, 1));
    }
    // Committed to the timeline without waiting: its copy-backs may wait for a reader.
    resources.EncodeReadbacks(commands, timeline != nullptr && sampleCounter == nil);
    if (timeline != nullptr && sampleCounter == nil && !resources.NeedsSynchronousCompletion()) {
        resources.CommittedWithoutWaiting();
        batched.Done();
        timeline->Commit(commands, [resourcesOwner, commands, completed = std::move(completed)] {
            const auto fault = CompleteCommittedWork([&] { return resourcesOwner->Complete(commands); });
            if (completed) completed(fault);
        }, bdaStores, resources.WrittenGuestRanges(), resources.KnownStores());
        return std::nullopt;
    }
    batched.Done();
    backend.Wait(commands);
    if (sampleCounter != nil) samplesPassed.store(sampleTotal(sampleCounter), std::memory_order_release);
    return CompleteCommittedWork([&] { return resources.Complete(commands); });
}

}
