#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#import <Metal/Metal.h>
#include "MetalDevice.hpp"
#include "MetalShaderPipeline.hpp"
#include "MetalTexture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
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
    // The draw's index parameters: the cached path reuses the vertex-buffer extents its index scan
    // produced, so a record stands only for the same index range (the scan's maxIndex depends on the
    // index values, which this key cannot see; the address, count and base vertex pin the range).
    std::uint64_t indexAddress = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t firstVertex = 0;
    std::uint32_t instanceCount = 0;
    std::uint32_t firstInstance = 0;

    void Clear() noexcept {
        vertexVariantId = 0;
        fragmentVariantId = 0;
        vertexPushOffset = 0;
        fragmentPushOffset = 0;
        stagesRegisterValue = 0;
        stagesPath = 0;
        vertexWaveSize = 0;
        fragmentWaveSize = 0;
        rasterSamples = 1;
        topology = 0;
        primitiveRestart = false;
        negativeOneToOne = false;
        flipVertexY = false;
        depthTest = false;
        depthWrite = false;
        depthCompare = 0;
        stencilTest = false;
        stencilFront = {};
        stencilBack = {};
        renderExtentWidth = 0;
        renderExtentHeight = 0;
        customSampleLocations = false;
        sampleLocations = {};
        colors.clear();
        depth = std::nullopt;
        blends.clear();
        attributes.clear();
        guestDescriptorWords.clear();
        indexed = false;
        nativeIndexSize = 4;
        indexAddress = 0;
        indexCount = 0;
        firstVertex = 0;
        instanceCount = 0;
        firstInstance = 0;
    }

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
               indexed == o.indexed && nativeIndexSize == o.nativeIndexSize &&
               indexAddress == o.indexAddress && indexCount == o.indexCount && firstVertex == o.firstVertex &&
               instanceCount == o.instanceCount && firstInstance == o.firstInstance;
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
        mix(k.indexAddress);
        mix((std::uint64_t{k.indexCount} << 32u) | k.firstVertex);
        mix((std::uint64_t{k.instanceCount} << 32u) | k.firstInstance);
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

// One guest image a draw read through the image cache: what its record's bindings stand on. The
// record is valid only while every one of these still proves the bytes it was built from, so a
// record survives GuestImageCache::Expire (the entries re-prove lazily) but not a write to its own
// images.
struct DrawImageDependency {
    const std::byte* host = nullptr;
    std::uint64_t guest = 0;
    std::uint64_t content = 0;
    std::shared_ptr<MetalTexture> texture;
};

// The guest snapshot a draw's bindings carry (MetalShaderResources::GuestSnapshot): the BDA page
// table a shader's guest-address lookup reads and the fault buffer it reports a miss into. A draw
// translation record captures the buffers its bindings were built with, so it stands only while the
// same snapshot comes back.
struct GuestSnapshotIdentity {
    id<MTLBuffer> table = nil;
    id<MTLBuffer> fault = nil;
    [[nodiscard]] bool operator==(const GuestSnapshotIdentity& other) const { return table == other.table && fault == other.fault; }
};

// One resident surface a draw's bindings sampled through the depth cache
// (MetalDraw::residentTexture) instead of a guest-memory texture. A view built now of a surface the
// record was built on has the same root texture (MetalEncoders::RootTexture), so a record whose draw
// sampled a surface since re-created at its address is dropped.
struct DrawResidentDependency {
    Graphics::GuestTextureResource descriptor{};
    bool storage = false;
    bool compare = false;
    id<MTLTexture> root = nil;
};

struct DrawTranslationRecord {
    std::uint64_t recordId = 0;
    std::shared_ptr<MetalRenderPipeline> vertexPipeline;
    id<MTLDepthStencilState> depthStencilState = nil;
    std::vector<MetalBufferBinding> vertexBuffers;
    std::vector<MetalShaderResourceBinding> vertexBindings;
    std::vector<MetalShaderResourceBinding> fragmentBindings;
    std::vector<id<MTLResource>> residency;
    std::vector<id<MTLHeap>> residentHeaps;
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
    std::vector<DrawImageDependency> images;
    // Images the record read that have no image-cache entry (staged or partially borrowed): with no
    // proof to re-run, the record is valid only while `imageCacheGeneration` is unchanged.
    std::uint32_t uncachedImages = 0;
    std::uint64_t imageCacheGeneration = 0;
    std::vector<DrawResidentDependency> residentDependencies;
    // The guest snapshot (the BDA page table and fault buffer) the record's bindings were built
    // with. The table changes whenever the working set's admissions change, and a stale one maps
    // guest addresses through buffers that are no longer current while a fault lands in a fault
    // buffer this work's completion never reads, so a record stands only while it matches.
    GuestSnapshotIdentity guestSnapshot{};
    // Whether this work writes guest memory only through its render-pass attachments. A record whose
    // draw wrote a storage buffer or image has no write plan to replay, so it is not reused.
    bool attachmentsOnly = false;
    // Guest ranges whose lazy copy-backs the draw encoded ahead of its own bindings, encoded again
    // when the record is reused.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> materializedRanges;

    DrawTranslationKey key;
};

struct DrawCacheStatistics {
    std::uint64_t lookups = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t inserts = 0;
    std::uint64_t clears = 0;
    // Records the cache served and the draw then dropped: one of their own images no longer proved
    // the bytes they were built from, the draw wrote something a reused record cannot replay, or it
    // no longer binds the record's attachment objects.
    std::uint64_t rejectedImages = 0;
    std::uint64_t rejectedWrites = 0;
    std::uint64_t rejectedAttachments = 0;
    std::uint64_t redundantStateElisions = 0;
};

class MetalDrawTranslationCache {
public:
    static constexpr std::size_t MaxEntries = 4096;

    MetalDrawTranslationCache() = default;

    // Off unless ANYPS5_METAL_DRAW_CACHE is set to a nonzero value. Reusing a record reuses
    // everything its bindings stand on: the guest-image entries it re-proves, the attachment objects
    // and resident surfaces it was built from, the snapshot buffers (the BDA page table, the fault
    // buffer, the mirror aliases) it captured, and the vertex-buffer extents its index scan produced.
    // The frame replay gate repeats one frame, so it cannot exercise a reuse *across* frames and
    // cannot protect any of that; the cache is opt-in until a multi-frame gate exists.
    [[nodiscard]] static bool Enabled() {
        static const bool enabled = [] {
            const char* v1 = std::getenv("ANYPS5_METAL_DRAW_CACHE");
            if (v1 == nullptr || *v1 == '\0' || std::string_view(v1) == "0") return false;
            const char* v2 = std::getenv("APS5_NO_DRAW_CACHE");
            if (v2 != nullptr && std::string_view(v2) != "0") return false;
            return true;
        }();
        return enabled;
    }

    [[nodiscard]] std::shared_ptr<DrawTranslationRecord> Find(const DrawTranslationKey& key,
                                                              std::uint64_t currentMemoryVersion) {
        if (!Enabled()) return nullptr;
        std::lock_guard lock(mutex);
        ++statistics.lookups;
        const auto it = entries.find(key);
        if (it == entries.end()) {
            ++statistics.misses;
            return nullptr;
        }
        const auto& record = it->second;
        if (record->memoryVersion != currentMemoryVersion) {
            ++statistics.misses;
            live.store(false, std::memory_order_relaxed);
            return nullptr;
        }
        if (!lruOrder.empty() && record->recordId != lruOrder.front()) {
            for (auto listIt = lruOrder.begin(); listIt != lruOrder.end(); ++listIt) {
                if (*listIt == record->recordId) {
                    lruOrder.splice(lruOrder.begin(), lruOrder, listIt);
                    break;
                }
            }
        }
        ++statistics.hits;
        live.store(true, std::memory_order_relaxed);
        return record;
    }

    // Whether the cache served a record that its images still prove: a batch scan recompiles every
    // draw it looks at, so the scan only runs while this holds (the batch needs a record for its
    // first draw, and none exists before the serial path made one). A record whose own images
    // changed is removed, which clears this until the cache serves another.
    [[nodiscard]] bool Live() const { return live.load(std::memory_order_relaxed); }

    // Drops the record for `key` (its images no longer prove the bytes it was built from).
    void Remove(const DrawTranslationKey& key) {
        std::lock_guard lock(mutex);
        const auto it = entries.find(key);
        if (it == entries.end()) return;
        const auto id = it->second->recordId;
        entries.erase(it);
        lruOrder.remove(id);
        live.store(false, std::memory_order_relaxed);
    }

    void Insert(std::shared_ptr<DrawTranslationRecord> record) {
        if (!Enabled() || record == nullptr) return;
        std::lock_guard lock(mutex);
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
        std::lock_guard lock(mutex);
        entries.clear();
        lruOrder.clear();
        live.store(false, std::memory_order_relaxed);
        ++statistics.clears;
    }

    void RecordStateElision() {
        std::lock_guard lock(mutex);
        ++statistics.redundantStateElisions;
    }

    // Counts a record the cache served that the draw then dropped (DrawCacheStatistics).
    void RecordRejection(std::uint64_t DrawCacheStatistics::* reason) {
        std::lock_guard lock(mutex);
        ++(statistics.*reason);
    }

    [[nodiscard]] DrawCacheStatistics TakeStatistics(bool reset = true) {
        std::lock_guard lock(mutex);
        const auto res = statistics;
        if (reset) statistics = {};
        return res;
    }

    [[nodiscard]] std::size_t Size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }

    [[nodiscard]] bool Contains(const DrawTranslationKey& key) const {
        if (!Enabled()) return false;
        std::lock_guard lock(mutex);
        return entries.find(key) != entries.end();
    }

private:
    mutable std::mutex mutex;
    std::atomic<bool> live{false};
    std::unordered_map<DrawTranslationKey, std::shared_ptr<DrawTranslationRecord>, DrawTranslationKeyHash> entries;
    std::list<std::uint64_t> lruOrder;
    std::uint64_t nextId = 0;
    DrawCacheStatistics statistics{};
};

}
