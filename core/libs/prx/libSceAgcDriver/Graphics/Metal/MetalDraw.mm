#include "MetalDraw.hpp"
#include "MetalShaderResources.hpp"
#include "MetalRenderState.hpp"
#include "MetalDepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AgcDriver::Metal {
namespace {
using namespace ShaderRecompiler;

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
    return surface;
}

std::span<const std::byte> pushBlock(const MetalBackend::Result& shader,
    const std::array<std::byte, Graphics::PipelinePushConstantBytes>& bytes) {
    if (!shader.pushConstantBuffer) return {};
    if (shader.pushConstantBytes > bytes.size()) throw std::invalid_argument("Metal draw push constant block exceeds pipeline ABI");
    return std::span(bytes).first(shader.pushConstantBytes);
}

}

MetalDraw::MetalDraw(id<MTLDevice> device, id<MTLLibrary> utilityLibrary)
    : backend(device, utilityLibrary), depthCache(std::make_unique<MetalDepthSurfaceCache>(backend)) {
    if (![device supportsFamily:MTLGPUFamilyApple7] || ![device supportsFamily:MTLGPUFamilyMetal3]) {
        throw std::invalid_argument("Metal draw requires an Apple GPU with Metal 3 integer and GPU address support");
    }
}

MetalDraw::~MetalDraw() = default;

BdaAbi::Fault MetalDraw::DrawSynchronously(const Graphics::State& state, const Pm4::DrawParameters& draw,
    std::span<const Graphics::CompiledShader> shaders,
    std::span<const NativeGuestMemory::BorrowedRange> ranges) {
    std::lock_guard lock(drawMutex);
    if (draw.indirect) throw std::invalid_argument("Metal draw indirect submission requires a native argument adapter");
    if (draw.flags != (draw.indexed ? 0u : (draw.flags & 0x20u))) throw std::invalid_argument("Metal draw packet flags are unsupported");
    if (draw.indexCount == 0 || draw.instanceCount == 0) return {};
    if (draw.firstInstance > std::numeric_limits<std::uint32_t>::max() - (draw.instanceCount - 1u)) {
        throw std::invalid_argument("Metal draw instance range overflows guest invocation index");
    }
    const auto primitive = PrimitiveType(state);
    const Graphics::CompiledShader* vertex = nullptr;
    const Graphics::CompiledShader* fragment = nullptr;
    for (const auto& shader : shaders) {
        if (shader.program == nullptr) throw std::invalid_argument("Metal draw compiled shader is missing");
        if (shader.stage == ShaderStage::Vertex && vertex == nullptr) vertex = &shader;
        else if (shader.stage == ShaderStage::Fragment && fragment == nullptr) fragment = &shader;
        else throw std::invalid_argument("Metal draw requires exactly one vertex and one fragment program");
    }
    if (vertex == nullptr || fragment == nullptr) throw std::invalid_argument("Metal draw vertex or fragment program is missing");
    const auto pushConstants = Graphics::AssemblePushConstants(shaders);
    auto depth = state.depth ? depthCache->Acquire(*state.depth) : nullptr;
    if (depth && std::find(depthSurfaces.begin(), depthSurfaces.end(), depth) == depthSurfaces.end()) depthSurfaces.push_back(depth);
    MetalShaderResources resources(backend, ranges, [&](const Graphics::GuestTextureResource& resource, bool storage, bool compare) -> id<MTLTexture> {
        for (auto cursor = depthSurfaces.rbegin(); cursor != depthSurfaces.rend(); ++cursor) {
            const auto& surface = *cursor;
            const auto& target = surface->Target();
            const bool stencil = target.stencilAddress != 0 && resource.baseAddress == target.stencilAddress;
            const bool isDepth = resource.baseAddress == target.address;
            if (!stencil && !isDepth) continue;
            if (storage || resource.width != target.extent.width || resource.height != target.extent.height ||
                resource.dimension != Graphics::TextureDimension::k2D || resource.baseLevel != 0 || resource.lastLevel != 0 ||
                resource.baseArray != 0 || resource.depthOrLastArray != 0 || (stencil && compare)) {
                throw std::invalid_argument("Metal draw resident depth view is incompatible with the guest descriptor");
            }
            const bool d16 = target.format == VK_FORMAT_D16_UNORM || target.format == VK_FORMAT_D16_UNORM_S8_UINT;
            const auto expected = stencil ? VK_FORMAT_R8_UINT : d16 ? VK_FORMAT_R16_UNORM : VK_FORMAT_R32_SFLOAT;
            if (Graphics::ResolveTextureFormat(resource.format) != expected) {
                throw std::invalid_argument("Metal draw resident depth plane format disagrees with the guest descriptor");
            }
            auto channel = [](std::uint8_t value) {
                switch (value) {
                case 0: return MTLTextureSwizzleZero;
                case 1: return MTLTextureSwizzleOne;
                case 4: return MTLTextureSwizzleRed;
                case 5: return MTLTextureSwizzleGreen;
                case 6: return MTLTextureSwizzleBlue;
                case 7: return MTLTextureSwizzleAlpha;
                default: throw std::invalid_argument("Metal draw resident depth component selection is invalid");
                }
            };
            id<MTLTexture> source = stencil ? surface->SampledStencilView() : surface->SampledDepthView();
            if (source == nil) throw std::invalid_argument("Metal draw sampled depth plane is missing");
            const auto swizzle = MTLTextureSwizzleChannelsMake(channel(resource.dstSelX), channel(resource.dstSelY),
                channel(resource.dstSelZ), channel(resource.dstSelW));
            id<MTLTexture> view = [source newTextureViewWithPixelFormat:source.pixelFormat textureType:source.textureType
                levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:swizzle];
            if (view == nil) throw std::runtime_error("Metal draw resident depth component view creation failed");
            return view;
        }
        return nil;
    });
    auto pipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
    ConfigureRenderPipelineDescriptor(pipelineDescriptor, state);
    auto renderPass = [MTLRenderPassDescriptor renderPassDescriptor];
    renderPass.renderTargetWidth = state.renderExtent.width;
    renderPass.renderTargetHeight = state.renderExtent.height;
    for (const auto& color : state.colors) {
        if (color.exportIndex >= 8) throw std::invalid_argument("Metal draw color export slot is invalid");
        auto texture = resources.Texture(colorSurface(color), true);
        id<MTLTexture> handle = texture->Texture();
        if (handle.pixelFormat != RenderPixelFormat(color.format)) throw std::invalid_argument("Metal draw color attachment format disagrees with the guest state");
        auto attachment = renderPass.colorAttachments[color.exportIndex];
        attachment.texture = handle;
        attachment.level = color.mip;
        attachment.depthPlane = color.depthSlice;
        attachment.loadAction = MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionStore;
    }
    if (depth) {
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
    if (draw.indexed) {
        std::uint32_t minIndex = std::numeric_limits<std::uint32_t>::max();
        if ((draw.indexSize != 2 && draw.indexSize != 4) || draw.indexAddress % draw.indexSize != 0) {
            throw std::invalid_argument("Metal draw index format or address is invalid");
        }
        const auto bytes = std::uint64_t{draw.indexCount} * draw.indexSize;
        if (bytes > std::numeric_limits<std::size_t>::max()) throw std::invalid_argument("Metal draw index byte count overflows");
        indexBuffer = resources.Buffer(draw.indexAddress, static_cast<std::size_t>(bytes));
        const auto* data = static_cast<const std::byte*>(indexBuffer.buffer.contents) + indexBuffer.offset;
        for (std::uint32_t i = 0; i < draw.indexCount; ++i) {
            std::uint32_t index = 0;
            if (draw.indexSize == 2) { std::uint16_t value; std::memcpy(&value, data + std::size_t{i} * 2u, 2); index = value; }
            else std::memcpy(&index, data + std::size_t{i} * 4u, 4);
            minIndex = std::min(minIndex, index);
            maxIndex = std::max(maxIndex, index);
        }
        if ((primitive == MTLPrimitiveTypeLineStrip || primitive == MTLPrimitiveTypeTriangleStrip) &&
            maxIndex == std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Metal draw cannot represent the guest UINT32_MAX vertex index without native primitive restart");
        }
        if ((primitive == MTLPrimitiveTypeLineStrip || primitive == MTLPrimitiveTypeTriangleStrip) &&
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
        const std::int64_t baseVertex = std::bit_cast<std::int32_t>(draw.firstVertex);
        const auto firstIndex = std::int64_t{minIndex} + baseVertex;
        const auto lastIndex = std::int64_t{maxIndex} + baseVertex;
        if (firstIndex < 0 || lastIndex > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Metal draw base vertex overflows guest invocation index");
        }
        maxIndex = static_cast<std::uint32_t>(lastIndex);
    } else {
        if (draw.indexAddress != 0 || draw.indexSize != 0 || draw.firstVertex > std::numeric_limits<std::uint32_t>::max() - (draw.indexCount - 1u)) {
            throw std::invalid_argument("Metal draw auto-index parameters are invalid");
        }
        maxIndex = draw.firstVertex + draw.indexCount - 1u;
    }
    const auto& attributes = vertex->program->vertexAttributes;
    const auto layout = Graphics::BuildVertexInputLayout(attributes, 29, 31, 2048,
        [](VkFormat format) { return vertexFormat(format) != MTLVertexFormatInvalid; });
    auto vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    std::vector<MetalBufferBinding> vertexBuffers;
    for (std::size_t i = 0; i < attributes.size(); ++i) {
        const auto& attribute = attributes[i];
        const auto& fields = attribute.resource.fields;
        const auto address = fields[0] | (std::uint64_t{fields[1] & 0xffffu} << 32u);
        auto buffer = resources.Buffer(address, Graphics::VertexBufferReadSize(attribute, maxIndex, draw.instanceCount, draw.firstInstance));
        const auto adjustment = buffer.offset % 4;
        buffer.offset -= adjustment;
        vertexBuffers.push_back(buffer);
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
    pipelineDescriptor.vertexDescriptor = vertexDescriptor;
    MetalBackend::TargetOptions target;
    target.supportsInt64 = true;
    target.supportsGpuAddresses = true;
    target.supportsSimdGroups = true;
    target.vertexBufferCount = static_cast<std::uint32_t>(attributes.size());
    target.pushConstantOffsetBytes = vertex->pushConstantOffset;
    target.flipVertexY = state.viewport.height > 0;
    target.fixupClipSpace = state.negativeOneToOne;
    auto nativeVertex = MetalBackend::ConvertToMetal(*vertex->program, ShaderStage::Vertex, target);
    target.vertexBufferCount = 0;
    target.pushConstantOffsetBytes = fragment->pushConstantOffset;
    auto nativeFragment = MetalBackend::ConvertToMetal(*fragment->program, ShaderStage::Fragment, target);
    MetalRenderPipeline pipeline(backend.Device(), std::move(nativeVertex), std::move(nativeFragment), pipelineDescriptor);
    auto vertexBindings = resources.Bindings(pipeline.VertexReflection());
    auto fragmentBindings = resources.Bindings(pipeline.FragmentReflection());
    auto depthStencilState = CreateDepthStencilState(backend.Device(), state);
    auto commands = backend.CommandBuffer();
    auto encoder = [commands renderCommandEncoderWithDescriptor:renderPass];
    if (encoder == nil) throw std::runtime_error("Metal draw render encoder allocation failed");
    try {
        pipeline.Bind(encoder, vertexBindings, fragmentBindings,
            pushBlock(pipeline.VertexReflection(), pushConstants), pushBlock(pipeline.FragmentReflection(), pushConstants), resources.Residency());
        BindRenderState(encoder, state, depthStencilState);
        for (std::size_t i = 0; i < vertexBuffers.size(); ++i) [encoder setVertexBuffer:vertexBuffers[i].buffer offset:vertexBuffers[i].offset atIndex:i];
        if (draw.indexed) {
            [encoder drawIndexedPrimitives:primitive indexCount:draw.indexCount indexType:nativeIndexSize == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                indexBuffer:indexBuffer.buffer indexBufferOffset:indexBuffer.offset instanceCount:draw.instanceCount baseVertex:static_cast<NSInteger>(std::bit_cast<std::int32_t>(draw.firstVertex)) baseInstance:draw.firstInstance];
        } else {
            [encoder drawPrimitives:primitive vertexStart:draw.firstVertex vertexCount:draw.indexCount instanceCount:draw.instanceCount baseInstance:draw.firstInstance];
        }
    } catch (...) {
        [encoder endEncoding];
        throw;
    }
    [encoder endEncoding];
    backend.Wait(commands);
    return resources.Complete(commands);
}

}
