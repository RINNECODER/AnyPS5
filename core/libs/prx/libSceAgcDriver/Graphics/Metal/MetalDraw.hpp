#pragma once

#include "MetalDevice.hpp"
#include "BdaAbi.hpp"
#include "GuestImageCache.hpp"
#include "MetalLazyCopyBacks.hpp"
#include "MetalTexture.hpp"
#include "MetalTimeline.hpp"
#include "MetalDrawTranslationCache.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <mutex>
#include <stdexcept>
#include <map>
#include <memory>
#include <tuple>
#include <optional>
#include <functional>

namespace AgcDriver::Metal {

class MetalDepthSurfaceCache;
class MetalDepthSurface;
class MetalColorSurface;
class MetalMultisampleExpander;
class MetalGuestHostAliases;

// Draw throws this for an indirect draw whose records the GPU cannot consume as they are (they need
// rewriting, counting or reading on the CPU) before it has encoded anything; the caller reads them.
struct IndirectNeedsCpuRecords : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class MetalDraw {
public:
    // Records of one GPU indirect draw at most (one Metal draw each).
    static constexpr std::uint32_t MaxGpuIndirectRecords = 256;
    explicit MetalDraw(id<MTLDevice> device, id<MTLLibrary> utilityLibrary);
    // Draws on the driver's own queue. Textures and uploads are shared with dispatches (the image
    // cache, resident surfaces), and a texture encodes its uploads and copy-backs into the caller's
    // command buffer, so draws and dispatches must use the same MetalDevice and command queue.
    explicit MetalDraw(MetalDevice& shared);
    ~MetalDraw();
    MetalDraw(const MetalDraw&) = delete;
    MetalDraw& operator=(const MetalDraw&) = delete;
    void InvalidateBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> changed);
    // The persistent no-copy guest aliases draws and dispatches bind (#291); mapping
    // publications drop the aliases of changed host spans through InvalidateBorrowedRanges.
    [[nodiscard]] MetalGuestHostAliases& HostAliases();
    // Textures that still match the guest surfaces they came from, shared by draws and dispatches
    // (GuestImageCache.hpp). The driver clears it wherever guest memory may have changed under it.
    // Null when ANYPS5_METAL_IMAGE_CACHE=0 turns it off (every binding uploads again).
    [[nodiscard]] GuestImageCache<MetalTexture>* ImageCache();
    // Copy-backs of surfaces draws and dispatches wrote, recorded until someone needs the guest
    // bytes (MetalLazyCopyBacks.hpp). The driver materializes them at its sync points.
    [[nodiscard]] MetalLazyCopyBacks& LazyCopyBacks() { return lazyCopyBacks; }
    void DumpSamplesSynchronously(std::uint64_t guestAddress);
    [[nodiscard]] ShaderRecompiler::MeshTargetLimits MeshLimits() const;
    [[nodiscard]] ShaderRecompiler::TessellationTargetLimits TessellationLimits() const;
    // Depth/stencil and multisampled color surfaces stay resident here and never reach guest
    // memory. Returns the view a guest image descriptor of such a surface binds (nil when the
    // address is not one), so compute dispatches read what the draws rendered; throws when the
    // descriptor is incompatible with the resident surface.
    [[nodiscard]] id<MTLTexture> ResidentTexture(const Graphics::GuestTextureResource& resource, bool storage, bool compare);
    [[nodiscard]] ShaderRecompiler::BdaAbi::Fault DrawSynchronously(
        const Graphics::State& state, const Pm4::DrawParameters& draw,
        std::span<const Graphics::CompiledShader> shaders,
        std::span<const NativeGuestMemory::BorrowedRange> ranges,
        std::optional<std::array<std::uint32_t, 5>> meshArguments = std::nullopt);
    // DrawSynchronously, except that with a `timeline` a draw whose results need no CPU copy into
    // guest memory is committed without waiting: it returns nullopt and `completed` receives the
    // fault (possibly empty) at the timeline's next drain.
    using Completed = std::function<void(ShaderRecompiler::BdaAbi::Fault)>;
    // A CB resolve (CB_COLOR_CONTROL mode 3, Graphics::DecodeColorMetadataPass): averages the samples
    // of `source`'s resident multisampled surface into the single-sample `destination` in guest
    // memory. A source no draw has rendered resolves from the zeroed surface a first draw would create.
    [[nodiscard]] ShaderRecompiler::BdaAbi::Fault ResolveSynchronously(const Graphics::ColorTarget& source,
        const Graphics::ColorTarget& destination, std::span<const NativeGuestMemory::BorrowedRange> ranges);
    // Called before a draw reads guest bytes on the CPU (indices, DCC keys, staged or unaligned
    // surfaces), so the driver can first finish committed work that may write them. It may run
    // completions that draw again, hence the recursive drawMutex.
    void SetCpuReadHook(std::function<void(std::uint64_t, std::uint64_t)> hook);
    // Materializes lazy copy-backs over guest bytes a binding reads through memory
    // (MetalShaderResources::SetMaterializeHook); the driver commits them to its timeline.
    void SetMaterializeHook(std::function<void(std::uint64_t, std::uint64_t)> hook);
    // Statistics of the per-draw translation cache (DrawTranslationCache).
    [[nodiscard]] DrawCacheStatistics TakeDrawCacheStatistics(bool reset = true) { return translationCache.TakeStatistics(reset); }
    void ClearTranslationCache() {
        std::lock_guard<std::recursive_mutex> lock(drawMutex);
        translationCache.Clear();
        lastEncoderState = {};
    }
    [[nodiscard]] std::optional<ShaderRecompiler::BdaAbi::Fault> Draw(
        const Graphics::State& state, const Pm4::DrawParameters& draw,
        std::span<const Graphics::CompiledShader> shaders,
        std::span<const NativeGuestMemory::BorrowedRange> ranges,
        std::optional<std::array<std::uint32_t, 5>> meshArguments, MetalTimeline* timeline, Completed completed);

    struct PreparedDraw {
        Graphics::State state;
        Pm4::DrawParameters draw;
        std::shared_ptr<DrawTranslationRecord> cachedRecord;
        MetalBufferBinding indexBuffer;
        std::array<std::byte, Graphics::PipelinePushConstantBytes> pushConstants{};
        DynamicRenderState dynamicState;
    };

    [[nodiscard]] MetalDrawTranslationCache& TranslationCache() { return translationCache; }
    [[nodiscard]] const MetalDrawTranslationCache& TranslationCache() const { return translationCache; }

    [[nodiscard]] static bool ParallelEncodingEnabled();
    static constexpr std::size_t DefaultMinParallelDraws = 16;
    [[nodiscard]] static std::size_t MinParallelDraws();

    [[nodiscard]] std::optional<PreparedDraw> PrepareCachedDraw(
        const Graphics::State& state,
        const Pm4::DrawParameters& draw,
        std::span<const Graphics::CompiledShader> shaders,
        std::span<const NativeGuestMemory::BorrowedRange> ranges);

    [[nodiscard]] std::optional<ShaderRecompiler::BdaAbi::Fault> DrawBatchParallel(
        std::span<const PreparedDraw> draws,
        std::span<const NativeGuestMemory::BorrowedRange> ranges,
        MetalTimeline* timeline = nullptr,
        Completed completed = nullptr);

private:
    MetalDraw(std::unique_ptr<MetalDevice> owned, MetalDevice* shared);
    [[nodiscard]] id<MTLTexture> residentTexture(const Graphics::GuestTextureResource& resource, bool storage, bool compare);
    [[nodiscard]] id<MTLTexture> residentDepthArray(const Graphics::GuestTextureResource& resource, bool storage);
    [[nodiscard]] id<MTLTexture> residentView(const Graphics::GuestTextureResource& resource, bool storage, bool compare) const;
    void PopulateTranslationKey(DrawTranslationKey& key,
        const Graphics::State& state,
        const Pm4::DrawParameters& draw,
        const Graphics::CompiledShader* vertex,
        const Graphics::CompiledShader* fragment);
    std::unique_ptr<MetalDevice> ownedBackend;
    MetalDevice& backend;
    std::unique_ptr<MetalDepthSurfaceCache> depthCache;
    std::unique_ptr<MetalGuestHostAliases> hostAliases;
    std::vector<std::shared_ptr<MetalDepthSurface>> depthSurfaces;
    std::vector<std::shared_ptr<MetalColorSurface>> colorSurfaces;
    // Array textures that gather resident depth slices (residentDepthArray), by base address and layers.
    std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, id<MTLTexture>> depthArrays;
    std::unique_ptr<MetalMultisampleExpander> multisampleExpander;
    std::recursive_mutex drawMutex;
    std::function<void(std::uint64_t, std::uint64_t)> cpuReadHook;
    std::function<void(std::uint64_t, std::uint64_t)> materializeHook;
    id<MTLBuffer> sampleCounter = nil;
    id<MTLBuffer> sampleTarget = nil;
    // Last, so their textures go before the device they were made on.
    MetalLazyCopyBacks lazyCopyBacks;
    GuestImageCache<MetalTexture> imageCache;
    MetalDrawTranslationCache translationCache;
    std::uint64_t memoryVersion = 0;
    struct EncoderDrawState {
        id<MTLRenderCommandEncoder> encoder = nil;
        std::uint64_t translationId = 0;
        id<MTLRenderPipelineState> pipelineState = nil;
        std::vector<std::byte> pushConstants;
        DynamicRenderState dynamicState;
        id<MTLDepthStencilState> depthStencilState = nil;
        std::vector<id<MTLResource>> residentResources;
        std::vector<id<MTLHeap>> residentHeaps;
        std::vector<MetalBufferBinding> vertexBuffers;
        void Reset() {
            encoder = nil;
            translationId = 0;
            pipelineState = nil;
            pushConstants.clear();
            dynamicState = {};
            depthStencilState = nil;
            residentResources.clear();
            residentHeaps.clear();
            vertexBuffers.clear();
        }
    };
    EncoderDrawState lastEncoderState;
};

}
