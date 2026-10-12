#pragma once

#include "MetalDevice.hpp"
#include "BdaAbi.hpp"
#include "GuestImageCache.hpp"
#include "MetalTexture.hpp"
#include "MetalTimeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <mutex>
#include <memory>
#include <optional>
#include <functional>

namespace AgcDriver::Metal {

class MetalDepthSurfaceCache;
class MetalDepthSurface;
class MetalColorSurface;
class MetalMultisampleExpander;
class MetalGuestHostAliases;

class MetalDraw {
public:
    explicit MetalDraw(id<MTLDevice> device, id<MTLLibrary> utilityLibrary, id<MTLCommandQueue> queue = nil);
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
    // Called before a draw reads guest bytes on the CPU (indices, DCC keys, staged or unaligned
    // surfaces), so the driver can first finish committed work that may write them. It may run
    // completions that draw again, hence the recursive drawMutex.
    void SetCpuReadHook(std::function<void(std::uint64_t, std::uint64_t)> hook);
    [[nodiscard]] std::optional<ShaderRecompiler::BdaAbi::Fault> Draw(
        const Graphics::State& state, const Pm4::DrawParameters& draw,
        std::span<const Graphics::CompiledShader> shaders,
        std::span<const NativeGuestMemory::BorrowedRange> ranges,
        std::optional<std::array<std::uint32_t, 5>> meshArguments, MetalTimeline* timeline, Completed completed);

private:
    [[nodiscard]] id<MTLTexture> residentTexture(const Graphics::GuestTextureResource& resource, bool storage, bool compare);
    [[nodiscard]] id<MTLTexture> residentView(const Graphics::GuestTextureResource& resource, bool storage, bool compare) const;
    MetalDevice backend;
    std::unique_ptr<MetalDepthSurfaceCache> depthCache;
    std::unique_ptr<MetalGuestHostAliases> hostAliases;
    std::vector<std::shared_ptr<MetalDepthSurface>> depthSurfaces;
    std::vector<std::shared_ptr<MetalColorSurface>> colorSurfaces;
    std::unique_ptr<MetalMultisampleExpander> multisampleExpander;
    std::recursive_mutex drawMutex;
    std::function<void(std::uint64_t, std::uint64_t)> cpuReadHook;
    id<MTLBuffer> sampleCounter = nil;
    id<MTLBuffer> sampleTarget = nil;
    // Last, so its textures go before the device they were made on.
    GuestImageCache<MetalTexture> imageCache;
};

}
