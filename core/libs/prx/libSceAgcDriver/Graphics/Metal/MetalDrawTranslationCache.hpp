#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#import <Metal/Metal.h>
#include "MetalDevice.hpp"
#include "MetalShaderPipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace AgcDriver::Metal {

struct DrawTranslationKey {
    std::uint64_t vertexVariantId = 0;
    std::uint64_t fragmentVariantId = 0;
    std::uint32_t vertexPushOffset = 0;
    std::uint32_t fragmentPushOffset = 0;

    std::uint32_t stagesRegisterValue = 0;
    std::uint32_t stagesPath = 0;
    std::uint32_t vertexWaveSize = 0;
    std::uint32_t fragmentWaveSize = 0;
    std::uint32_t rasterSamples = 1;
    std::uint32_t topology = 0;
    bool primitiveRestart = false;
    bool negativeOneToOne = false;
    bool flipVertexY = false;

    bool depthTest = false;
    bool depthWrite = false;
    std::uint32_t depthCompare = 0;
    bool stencilTest = false;
    VkStencilOpState stencilFront{};
    VkStencilOpState stencilBack{};

    std::uint32_t renderExtentWidth = 0;
    std::uint32_t renderExtentHeight = 0;
    bool customSampleLocations = false;
    std::array<std::uint8_t, 8> sampleLocations{};

    struct ColorTargetInfo {
        std::uint32_t exportIndex = 0;
        std::uint32_t format = 0;
        std::uint32_t samples = 1;
        std::uint32_t packing = 0;
        bool uintExport = false;
        std::uint64_t address = 0;
        std::uint32_t mip = 0;
        std::uint32_t depthSlice = 0;

        bool operator==(const ColorTargetInfo& o) const noexcept {
            return exportIndex == o.exportIndex && format == o.format && samples == o.samples &&
                   packing == o.packing && uintExport == o.uintExport && address == o.address &&
                   mip == o.mip && depthSlice == o.depthSlice;
        }
    };
    std::vector<ColorTargetInfo> colors;

    struct DepthTargetInfo {
        std::uint64_t address = 0;
        std::uint64_t stencilAddress = 0;
        std::uint32_t format = 0;
        std::uint32_t samples = 1;
        std::uint32_t slice = 0;
        std::uint32_t lastSlice = 0;
        bool layered = false;

        bool operator==(const DepthTargetInfo& o) const noexcept {
            return address == o.address && stencilAddress == o.stencilAddress &&
                   format == o.format && samples == o.samples && slice == o.slice &&
                   lastSlice == o.lastSlice && layered == o.layered;
        }
    };
    std::optional<DepthTargetInfo> depth;

    struct BlendInfo {
        VkBool32 blendEnable = VK_FALSE;
        VkBlendFactor srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkBlendFactor dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkBlendOp colorBlendOp = VK_BLEND_OP_ADD;
        VkBlendFactor srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkBlendFactor dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        VkBlendOp alphaBlendOp = VK_BLEND_OP_ADD;
        VkColorComponentFlags colorWriteMask = 0;

        bool operator==(const BlendInfo& o) const noexcept {
            return blendEnable == o.blendEnable && srcColorBlendFactor == o.srcColorBlendFactor &&
                   dstColorBlendFactor == o.dstColorBlendFactor && colorBlendOp == o.colorBlendOp &&
                   srcAlphaBlendFactor == o.srcAlphaBlendFactor && dstAlphaBlendFactor == o.dstAlphaBlendFactor &&
                   alphaBlendOp == o.alphaBlendOp && colorWriteMask == o.colorWriteMask;
        }
    };
    std::vector<BlendInfo> blends;

    struct VertexAttributeInfo {
        std::uint32_t location = 0;
        std::uint32_t fetchIndex = 0;
        std::array<std::uint32_t, 4> fields{};

        bool operator==(const VertexAttributeInfo& o) const noexcept {
            return location == o.location && fetchIndex == o.fetchIndex && fields == o.fields;
        }
    };
    std::vector<VertexAttributeInfo> attributes;

    std::vector<std::uint32_t> guestDescriptorWords;

    bool indexed = false;
    std::uint32_t nativeIndexSize = 4;

    bool operator==(const DrawTranslationKey& o) const noexcept {
        return vertexVariantId == o.vertexVariantId && fragmentVariantId == o.fragmentVariantId &&
               vertexPushOffset == o.vertexPushOffset && fragmentPushOffset == o.fragmentPushOffset &&
               stagesRegisterValue == o.stagesRegisterValue && stagesPath == o.stagesPath &&
               vertexWaveSize == o.vertexWaveSize && fragmentWaveSize == o.fragmentWaveSize &&
               rasterSamples == o.rasterSamples && topology == o.topology &&
               primitiveRestart == o.primitiveRestart && negativeOneToOne == o.negativeOneToOne &&
               flipVertexY == o.flipVertexY && depthTest == o.depthTest && depthWrite == o.depthWrite &&
               depthCompare == o.depthCompare && stencilTest == o.stencilTest &&
               std::memcmp(&stencilFront, &o.stencilFront, sizeof(stencilFront)) == 0 &&
               std::memcmp(&stencilBack, &o.stencilBack, sizeof(stencilBack)) == 0 &&
               renderExtentWidth == o.renderExtentWidth && renderExtentHeight == o.renderExtentHeight &&
               customSampleLocations == o.customSampleLocations && sampleLocations == o.sampleLocations &&
               colors == o.colors && depth == o.depth && blends == o.blends &&
               attributes == o.attributes && guestDescriptorWords == o.guestDescriptorWords &&
               indexed == o.indexed && nativeIndexSize == o.nativeIndexSize;
    }
};

struct DrawTranslationKeyHash {
    std::size_t operator()(const DrawTranslationKey& k) const noexcept {
        std::uint64_t h = 0xcbf29ce484222325ull;
        const auto mix = [&](std::uint64_t v) {
            h ^= v;
            h *= 0x100000001b3ull;
        };
        mix(k.vertexVariantId);
        mix(k.fragmentVariantId);
        mix((std::uint64_t{k.vertexPushOffset} << 32u) | k.fragmentPushOffset);
        mix(k.stagesRegisterValue);
        mix((std::uint64_t{k.stagesPath} << 32u) | k.rasterSamples);
        mix((std::uint64_t{k.topology} << 32u) | (k.primitiveRestart ? 1u : 0u));
        mix((std::uint64_t{k.renderExtentWidth} << 32u) | k.renderExtentHeight);
        mix(k.colors.size());
        for (const auto& c : k.colors) {
            mix(c.address);
            mix((std::uint64_t{c.format} << 32u) | c.exportIndex);
        }
        if (k.depth) {
            mix(k.depth->address);
            mix(k.depth->format);
        }
        mix(k.attributes.size());
        for (const auto& attr : k.attributes) {
            mix(attr.fields[0]);
            mix(attr.fields[1]);
        }
        mix(k.guestDescriptorWords.size());
        for (const auto w : k.guestDescriptorWords) {
            mix(w);
        }
        return static_cast<std::size_t>(h);
    }
};

struct DynamicRenderState {
    VkViewport viewport{};
    VkRect2D scissor{};
    VkCullModeFlags cullMode = 0;
    VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthClamp = false;
    bool depthBias = false;
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;
    std::array<float, 4> blendConstants{};
    std::uint32_t stencilFrontRef = 0;
    std::uint32_t stencilBackRef = 0;

    bool operator==(const DynamicRenderState& o) const noexcept {
        return viewport.x == o.viewport.x && viewport.y == o.viewport.y &&
               viewport.width == o.viewport.width && viewport.height == o.viewport.height &&
               viewport.minDepth == o.viewport.minDepth && viewport.maxDepth == o.viewport.maxDepth &&
               scissor.offset.x == o.scissor.offset.x && scissor.offset.y == o.scissor.offset.y &&
               scissor.extent.width == o.scissor.extent.width && scissor.extent.height == o.scissor.extent.height &&
               cullMode == o.cullMode && frontFace == o.frontFace && depthClamp == o.depthClamp &&
               depthBias == o.depthBias && depthBiasConstant == o.depthBiasConstant &&
               depthBiasSlope == o.depthBiasSlope && depthBiasClamp == o.depthBiasClamp &&
               blendConstants == o.blendConstants && stencilFrontRef == o.stencilFrontRef &&
               stencilBackRef == o.stencilBackRef;
    }
};

struct DrawTranslationRecord {
    std::uint64_t recordId = 0;
    std::shared_ptr<MetalRenderPipeline> vertexPipeline;
    id<MTLDepthStencilState> depthStencilState = nil;
    std::vector<MetalBufferBinding> vertexBuffers;
    std::vector<MetalShaderResourceBinding> vertexBindings;
    std::vector<MetalShaderResourceBinding> fragmentBindings;
    std::vector<id<MTLResource>> residency;
    MTLRenderPassDescriptor* renderPass = nil;
    MetalEncoders::PassKey passKey{};
    bool bdaStores = false;
    bool mergeable = false;
    bool wholeVertexRanges = false;
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    std::uint32_t nativeIndexSize = 4;

    std::optional<std::uint32_t> vertexPushBuffer;
    std::size_t vertexPushBytes = 0;
    std::size_t vertexPushOffset = 0;
    std::optional<std::uint32_t> fragmentPushBuffer;
    std::size_t fragmentPushBytes = 0;
    std::size_t fragmentPushOffset = 0;

    std::uint64_t memoryVersion = 0;
    std::uint64_t imageCacheClears = 0;

    DrawTranslationKey key;
};

struct DrawCacheStatistics {
    std::uint64_t lookups = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t inserts = 0;
    std::uint64_t clears = 0;
    std::uint64_t redundantStateElisions = 0;
};

class MetalDrawTranslationCache {
public:
    static constexpr std::size_t MaxEntries = 4096;

    MetalDrawTranslationCache() = default;

    [[nodiscard]] static bool Enabled() {
        static const bool enabled = [] {
            const char* v1 = std::getenv("ANYPS5_METAL_DRAW_CACHE");
            if (v1 != nullptr && std::string_view(v1) == "0") return false;
            const char* v2 = std::getenv("APS5_NO_DRAW_CACHE");
            if (v2 != nullptr && std::string_view(v2) != "0") return false;
            return true;
        }();
        return enabled;
    }

    [[nodiscard]] std::shared_ptr<DrawTranslationRecord> Find(const DrawTranslationKey& key,
                                                              std::uint64_t currentMemoryVersion,
                                                              std::uint64_t currentImageCacheClears) {
        if (!Enabled()) return nullptr;
        ++statistics.lookups;
        const auto it = entries.find(key);
        if (it == entries.end()) {
            ++statistics.misses;
            return nullptr;
        }
        const auto& record = it->second;
        if (record->memoryVersion != currentMemoryVersion || record->imageCacheClears != currentImageCacheClears) {
            ++statistics.misses;
            return nullptr;
        }
        // Move to front of LRU order
        if (it->second->recordId != lruOrder.front()) {
            for (auto listIt = lruOrder.begin(); listIt != lruOrder.end(); ++listIt) {
                if (*listIt == record->recordId) {
                    lruOrder.splice(lruOrder.begin(), lruOrder, listIt);
                    break;
                }
            }
        }
        ++statistics.hits;
        return record;
    }

    void Insert(std::shared_ptr<DrawTranslationRecord> record) {
        if (!Enabled() || record == nullptr) return;
        record->recordId = ++nextId;
        while (entries.size() >= MaxEntries && !lruOrder.empty()) {
            const auto oldestId = lruOrder.back();
            lruOrder.pop_back();
            for (auto it = entries.begin(); it != entries.end(); ++it) {
                if (it->second->recordId == oldestId) {
                    entries.erase(it);
                    break;
                }
            }
        }
        lruOrder.push_front(record->recordId);
        entries[record->key] = record;
        ++statistics.inserts;
    }

    void Clear() {
        entries.clear();
        lruOrder.clear();
        ++statistics.clears;
    }

    void RecordStateElision() {
        ++statistics.redundantStateElisions;
    }

    [[nodiscard]] DrawCacheStatistics TakeStatistics(bool reset = true) {
        const auto res = statistics;
        if (reset) statistics = {};
        return res;
    }

    [[nodiscard]] std::size_t Size() const {
        return entries.size();
    }

private:
    std::unordered_map<DrawTranslationKey, std::shared_ptr<DrawTranslationRecord>, DrawTranslationKeyHash> entries;
    std::list<std::uint64_t> lruOrder;
    std::uint64_t nextId = 0;
    DrawCacheStatistics statistics{};
};

}
