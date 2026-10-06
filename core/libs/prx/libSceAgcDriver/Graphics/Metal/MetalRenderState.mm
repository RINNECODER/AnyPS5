#include "MetalRenderState.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {
namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(std::string("Metal render state: ") + message);
}

MTLCompareFunction Compare(VkCompareOp operation) {
    switch (operation) {
    case VK_COMPARE_OP_NEVER: return MTLCompareFunctionNever;
    case VK_COMPARE_OP_LESS: return MTLCompareFunctionLess;
    case VK_COMPARE_OP_EQUAL: return MTLCompareFunctionEqual;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return MTLCompareFunctionLessEqual;
    case VK_COMPARE_OP_GREATER: return MTLCompareFunctionGreater;
    case VK_COMPARE_OP_NOT_EQUAL: return MTLCompareFunctionNotEqual;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
    case VK_COMPARE_OP_ALWAYS: return MTLCompareFunctionAlways;
    default: throw std::invalid_argument("Metal render state: unsupported depth/stencil comparison");
    }
}

MTLStencilOperation Stencil(VkStencilOp operation) {
    switch (operation) {
    case VK_STENCIL_OP_KEEP: return MTLStencilOperationKeep;
    case VK_STENCIL_OP_ZERO: return MTLStencilOperationZero;
    case VK_STENCIL_OP_REPLACE: return MTLStencilOperationReplace;
    case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return MTLStencilOperationIncrementClamp;
    case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return MTLStencilOperationDecrementClamp;
    case VK_STENCIL_OP_INVERT: return MTLStencilOperationInvert;
    case VK_STENCIL_OP_INCREMENT_AND_WRAP: return MTLStencilOperationIncrementWrap;
    case VK_STENCIL_OP_DECREMENT_AND_WRAP: return MTLStencilOperationDecrementWrap;
    default: throw std::invalid_argument("Metal render state: unsupported stencil operation");
    }
}

MTLStencilDescriptor* StencilFace(const VkStencilOpState& state) {
    auto descriptor = [MTLStencilDescriptor new];
    descriptor.stencilCompareFunction = Compare(state.compareOp);
    descriptor.stencilFailureOperation = Stencil(state.failOp);
    descriptor.depthFailureOperation = Stencil(state.depthFailOp);
    descriptor.depthStencilPassOperation = Stencil(state.passOp);
    descriptor.readMask = state.compareMask;
    descriptor.writeMask = state.writeMask;
    return descriptor;
}

MTLBlendFactor BlendFactor(VkBlendFactor factor) {
    switch (factor) {
    case VK_BLEND_FACTOR_ZERO: return MTLBlendFactorZero;
    case VK_BLEND_FACTOR_ONE: return MTLBlendFactorOne;
    case VK_BLEND_FACTOR_SRC_COLOR: return MTLBlendFactorSourceColor;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case VK_BLEND_FACTOR_DST_COLOR: return MTLBlendFactorDestinationColor;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case VK_BLEND_FACTOR_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case VK_BLEND_FACTOR_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case VK_BLEND_FACTOR_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    default: throw std::invalid_argument("Metal render state: unsupported blend factor");
    }
}

MTLBlendOperation BlendOperation(VkBlendOp operation) {
    switch (operation) {
    case VK_BLEND_OP_ADD: return MTLBlendOperationAdd;
    case VK_BLEND_OP_SUBTRACT: return MTLBlendOperationSubtract;
    case VK_BLEND_OP_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case VK_BLEND_OP_MIN: return MTLBlendOperationMin;
    case VK_BLEND_OP_MAX: return MTLBlendOperationMax;
    default: throw std::invalid_argument("Metal render state: unsupported blend operation");
    }
}

MTLColorWriteMask WriteMask(VkColorComponentFlags mask) {
    Require((mask & ~(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)) == 0,
            "unsupported color write mask");
    MTLColorWriteMask result = MTLColorWriteMaskNone;
    if (mask & VK_COLOR_COMPONENT_R_BIT) result |= MTLColorWriteMaskRed;
    if (mask & VK_COLOR_COMPONENT_G_BIT) result |= MTLColorWriteMaskGreen;
    if (mask & VK_COLOR_COMPONENT_B_BIT) result |= MTLColorWriteMaskBlue;
    if (mask & VK_COLOR_COMPONENT_A_BIT) result |= MTLColorWriteMaskAlpha;
    return result;
}

bool SupportsDepthBounds(id<MTLDevice> device) {
    if (@available(macOS 26.0, *)) return [device supportsFamily:MTLGPUFamilyApple10];
    return false;
}

void ValidateDevice(const Graphics::State& state, id<MTLDevice> device) {
    Require(!state.depthBoundsTest || !state.depth || SupportsDepthBounds(device),
            "depth bounds testing requires macOS 26 and an Apple10 GPU");
}

void Validate(const Graphics::State& state) {
    const bool vertex = state.stages.path == Graphics::ShaderPath::Vertex && !state.stages.mesh;
    const bool mesh = state.stages.path == Graphics::ShaderPath::Geometry && state.stages.mesh.has_value();
    Require((vertex || mesh) && !state.stages.tessellation && !state.rectList,
            "graphics path requires unimplemented native scheduling");
    Require(!state.primitiveRestart, "primitive restart scheduling is not implemented");
    if (state.depthBoundsTest) {
        Require(std::isfinite(state.minDepthBounds) && std::isfinite(state.maxDepthBounds) &&
                state.minDepthBounds >= 0 && state.maxDepthBounds <= 1 && state.minDepthBounds <= state.maxDepthBounds,
                "depth bounds must be ordered within zero-to-one");
    }
    const auto& viewport = state.viewport;
    Require(std::isfinite(viewport.x) && std::isfinite(viewport.y) && std::isfinite(viewport.width) && std::isfinite(viewport.height) &&
            viewport.width > 0 && viewport.height != 0 && std::isfinite(viewport.y + viewport.height),
            "viewport must be finite with positive width and nonzero height");
    Require(std::isfinite(viewport.minDepth) && std::isfinite(viewport.maxDepth) &&
            viewport.minDepth >= 0 && viewport.minDepth <= 1 && viewport.maxDepth >= 0 && viewport.maxDepth <= 1,
            "viewport depth must lie within zero-to-one");
    Require(state.renderExtent.width != 0 && state.renderExtent.height != 0, "render extent must be nonzero");
    Require(state.scissor.offset.x >= 0 && state.scissor.offset.y >= 0 && state.scissor.extent.width != 0 && state.scissor.extent.height != 0,
            "negative or empty scissor requires draw scheduling");
    Require(static_cast<std::uint64_t>(state.scissor.offset.x) + state.scissor.extent.width <= state.renderExtent.width &&
            static_cast<std::uint64_t>(state.scissor.offset.y) + state.scissor.extent.height <= state.renderExtent.height,
            "scissor exceeds the decoded render extent");
    Require(state.cullMode == VK_CULL_MODE_NONE || state.cullMode == VK_CULL_MODE_FRONT_BIT || state.cullMode == VK_CULL_MODE_BACK_BIT,
            "front-and-back culling requires draw scheduling");
    Require(state.frontFace == VK_FRONT_FACE_CLOCKWISE || state.frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE,
            "unsupported front-face winding");
    Require(std::isfinite(state.depthBiasConstant) && std::isfinite(state.depthBiasSlope) && std::isfinite(state.depthBiasClamp),
            "depth bias must be finite");
    for (const auto value : state.blendConstants) Require(std::isfinite(value), "blend constants must be finite");
    Require(state.depth || (!state.depthTest && !state.depthWrite && !state.stencilTest && !state.depthBias),
            "depth/stencil state requires a decoded depth target");
    if (state.depth) {
        const auto format = state.depth->format;
        Require(format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D32_SFLOAT_S8_UINT,
                "depth target format has no implemented native mapping");
        Require(!state.stencilTest || format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT, "stencil testing requires a native stencil attachment");
    }
    Require(state.colors.size() <= 8 && state.blends.size() <= 8, "too many color attachments");
    std::set<std::uint32_t> exports;
    std::uint32_t blendCount = 0;
    for (const auto& color : state.colors) {
        Require(color.exportIndex < 8 && exports.insert(color.exportIndex).second, "invalid or duplicate color export index");
        blendCount = std::max(blendCount, color.exportIndex + 1);
        const auto format = RenderPixelFormat(color.format);
        Require(format != MTLPixelFormatDepth16Unorm && format != MTLPixelFormatDepth32Float && format != MTLPixelFormatDepth32Float_Stencil8,
                "a depth format cannot be used as a color attachment");
    }
    Require(state.blends.size() == blendCount, "blend attachment count differs from sparse color exports");
    if (vertex) (void)PrimitiveType(state);
}

void ConfigureAttachments(MTLRenderPipelineColorAttachmentDescriptorArray* attachments, const Graphics::State& state) {
    for (NSUInteger i = 0; i < 8; ++i) attachments[i] = [MTLRenderPipelineColorAttachmentDescriptor new];
    for (const auto& color : state.colors) {
        const auto& blend = state.blends[color.exportIndex];
        auto attachment = attachments[color.exportIndex];
        attachment.pixelFormat = RenderPixelFormat(color.format);
        attachment.writeMask = WriteMask(blend.colorWriteMask);
        attachment.blendingEnabled = blend.blendEnable != 0;
        if (attachment.blendingEnabled) {
            attachment.sourceRGBBlendFactor = BlendFactor(blend.srcColorBlendFactor);
            attachment.destinationRGBBlendFactor = BlendFactor(blend.dstColorBlendFactor);
            attachment.rgbBlendOperation = BlendOperation(blend.colorBlendOp);
            attachment.sourceAlphaBlendFactor = BlendFactor(blend.srcAlphaBlendFactor);
            attachment.destinationAlphaBlendFactor = BlendFactor(blend.dstAlphaBlendFactor);
            attachment.alphaBlendOperation = BlendOperation(blend.alphaBlendOp);
        }
    }
}

}

MTLPixelFormat RenderPixelFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return MTLPixelFormatR8Unorm;
    case VK_FORMAT_R8_SNORM: return MTLPixelFormatR8Snorm;
    case VK_FORMAT_R8_UINT: return MTLPixelFormatR8Uint;
    case VK_FORMAT_R16_UNORM: return MTLPixelFormatR16Unorm;
    case VK_FORMAT_R16_SNORM: return MTLPixelFormatR16Snorm;
    case VK_FORMAT_R16_UINT: return MTLPixelFormatR16Uint;
    case VK_FORMAT_R16_SFLOAT: return MTLPixelFormatR16Float;
    case VK_FORMAT_R32_UINT: return MTLPixelFormatR32Uint;
    case VK_FORMAT_R32_SINT: return MTLPixelFormatR32Sint;
    case VK_FORMAT_R32_SFLOAT: return MTLPixelFormatR32Float;
    case VK_FORMAT_R8G8_UNORM: return MTLPixelFormatRG8Unorm;
    case VK_FORMAT_R8G8_SNORM: return MTLPixelFormatRG8Snorm;
    case VK_FORMAT_R16G16_UNORM: return MTLPixelFormatRG16Unorm;
    case VK_FORMAT_R16G16_SNORM: return MTLPixelFormatRG16Snorm;
    case VK_FORMAT_R16G16_UINT: return MTLPixelFormatRG16Uint;
    case VK_FORMAT_R16G16_SFLOAT: return MTLPixelFormatRG16Float;
    case VK_FORMAT_R32G32_UINT: return MTLPixelFormatRG32Uint;
    case VK_FORMAT_R32G32_SFLOAT: return MTLPixelFormatRG32Float;
    case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case VK_FORMAT_R8G8B8A8_SNORM: return MTLPixelFormatRGBA8Snorm;
    case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLPixelFormatRGB10A2Unorm;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return MTLPixelFormatRG11B10Float;
    case VK_FORMAT_R16G16B16A16_UNORM: return MTLPixelFormatRGBA16Unorm;
    case VK_FORMAT_R16G16B16A16_SNORM: return MTLPixelFormatRGBA16Snorm;
    case VK_FORMAT_R16G16B16A16_UINT: return MTLPixelFormatRGBA16Uint;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
    case VK_FORMAT_R32G32B32A32_UINT: return MTLPixelFormatRGBA32Uint;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_D16_UNORM_S8_UINT: return MTLPixelFormatDepth16Unorm;
    case VK_FORMAT_D32_SFLOAT: return MTLPixelFormatDepth32Float;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
    default: throw std::invalid_argument("Metal render state: unsupported attachment format " + std::to_string(format));
    }
}

MTLPrimitiveType PrimitiveType(const Graphics::State& state) {
    switch (state.topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return MTLPrimitiveTypePoint;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return MTLPrimitiveTypeLine;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return MTLPrimitiveTypeTriangle;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
    default: throw std::invalid_argument("Metal render state: topology requires unimplemented native scheduling");
    }
}

void ConfigureRenderPipelineDescriptor(MTLRenderPipelineDescriptor* descriptor, const Graphics::State& state) {
    Require(descriptor != nil, "pipeline descriptor is missing");
    Require(state.stages.path == Graphics::ShaderPath::Vertex && !state.stages.mesh, "vertex pipeline requires a vertex shader path");
    Validate(state);
    ConfigureAttachments(descriptor.colorAttachments, state);
    descriptor.rasterSampleCount = 1;
    const auto primitive = PrimitiveType(state);
    descriptor.inputPrimitiveTopology = primitive == MTLPrimitiveTypePoint ? MTLPrimitiveTopologyClassPoint :
        primitive == MTLPrimitiveTypeLine || primitive == MTLPrimitiveTypeLineStrip ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
    descriptor.depthAttachmentPixelFormat = state.depth ? RenderPixelFormat(state.depth->format) : MTLPixelFormatInvalid;
    descriptor.stencilAttachmentPixelFormat = !state.depth ? MTLPixelFormatInvalid :
        state.depth->format == VK_FORMAT_D16_UNORM_S8_UINT ? MTLPixelFormatStencil8 :
        state.depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
}

void ConfigureRenderPipelineDescriptor(MTLMeshRenderPipelineDescriptor* descriptor, const Graphics::State& state) {
    Require(descriptor != nil, "mesh pipeline descriptor is missing");
    Require(state.stages.path == Graphics::ShaderPath::Geometry && state.stages.mesh.has_value(), "mesh pipeline requires a mesh shader path");
    Validate(state);
    ConfigureAttachments(descriptor.colorAttachments, state);
    descriptor.rasterSampleCount = 1;
    descriptor.depthAttachmentPixelFormat = state.depth ? RenderPixelFormat(state.depth->format) : MTLPixelFormatInvalid;
    descriptor.stencilAttachmentPixelFormat = !state.depth ? MTLPixelFormatInvalid :
        state.depth->format == VK_FORMAT_D16_UNORM_S8_UINT ? MTLPixelFormatStencil8 :
        state.depth->format == VK_FORMAT_D32_SFLOAT_S8_UINT ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatInvalid;
}

id<MTLDepthStencilState> CreateDepthStencilState(id<MTLDevice> device, const Graphics::State& state) {
    Require(device != nil, "device is missing");
    Validate(state);
    ValidateDevice(state, device);
    auto descriptor = [MTLDepthStencilDescriptor new];
    descriptor.depthCompareFunction = state.depthTest ? Compare(state.depthCompare) : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = state.depthTest && state.depthWrite;
    if (state.stencilTest) {
        descriptor.frontFaceStencil = StencilFace(state.stencilFront);
        descriptor.backFaceStencil = StencilFace(state.stencilBack);
    }
    auto native = [device newDepthStencilStateWithDescriptor:descriptor];
    if (native == nil) throw std::runtime_error("Metal render state: depth/stencil creation failed");
    return native;
}

void BindRenderState(id<MTLRenderCommandEncoder> encoder, const Graphics::State& state,
                     id<MTLDepthStencilState> depthStencilState) {
    Require(encoder != nil && depthStencilState != nil && encoder.device == depthStencilState.device,
            "encoder and depth/stencil state must share a device");
    Validate(state);
    ValidateDevice(state, encoder.device);
    const auto& viewport = state.viewport;
    const auto y = viewport.height < 0 ? static_cast<double>(viewport.y) + viewport.height : viewport.y;
    [encoder setViewport:MTLViewport{viewport.x, y, viewport.width, std::abs(viewport.height), viewport.minDepth, viewport.maxDepth}];
    if (@available(macOS 26.0, *)) {
        if (SupportsDepthBounds(encoder.device)) {
            [encoder setDepthTestMinBound:state.depthBoundsTest && state.depth ? state.minDepthBounds : 0
                                maxBound:state.depthBoundsTest && state.depth ? state.maxDepthBounds : 1];
        }
    }
    [encoder setScissorRect:MTLScissorRect{static_cast<NSUInteger>(state.scissor.offset.x), static_cast<NSUInteger>(state.scissor.offset.y),
                                        state.scissor.extent.width, state.scissor.extent.height}];
    [encoder setCullMode:state.cullMode == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront :
                        state.cullMode == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack : MTLCullModeNone];
    [encoder setFrontFacingWinding:state.frontFace == VK_FRONT_FACE_CLOCKWISE ? MTLWindingClockwise : MTLWindingCounterClockwise];
    [encoder setDepthClipMode:state.depthClamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    [encoder setDepthBias:state.depthBias ? state.depthBiasConstant : 0
              slopeScale:state.depthBias ? state.depthBiasSlope : 0 clamp:state.depthBias ? state.depthBiasClamp : 0];
    [encoder setBlendColorRed:state.blendConstants[0] green:state.blendConstants[1] blue:state.blendConstants[2] alpha:state.blendConstants[3]];
    [encoder setDepthStencilState:depthStencilState];
    [encoder setStencilFrontReferenceValue:state.stencilTest ? state.stencilFront.reference : 0
                      backReferenceValue:state.stencilTest ? state.stencilBack.reference : 0];
}

}
