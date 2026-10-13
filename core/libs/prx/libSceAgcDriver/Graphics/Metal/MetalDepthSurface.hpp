#pragma once

#include "MetalDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

class MetalDepthSurface {
public:
    MetalDepthSurface(const MetalDevice& backend, const Graphics::DepthTarget& target);
    MetalDepthSurface(const MetalDepthSurface&) = delete;
    MetalDepthSurface& operator=(const MetalDepthSurface&) = delete;
    [[nodiscard]] id<MTLTexture> DepthTexture() const;
    [[nodiscard]] id<MTLTexture> StencilTexture() const;
    [[nodiscard]] id<MTLTexture> SampledDepthView() const;
    [[nodiscard]] id<MTLTexture> SampledStencilView() const;
    [[nodiscard]] const Graphics::DepthTarget& Target() const;
    [[nodiscard]] bool OverlapsMappings(std::span<const NativeGuestMemory::BorrowedRange> changed) const;

private:
    Graphics::DepthTarget target;
    id<MTLTexture> depth;
    id<MTLTexture> stencil;
    id<MTLTexture> sampledStencil;
};

// Cascaded shadow maps: each slice of a depth array is rendered through DB_DEPTH_VIEW SLICE_START as a
// resident surface of its own (State.cpp offsets the address by whole slices), and a later pass samples
// all of them through one 2D-array descriptor at the address of slice 0, `base`. Copies the depth plane
// of slices first..last into a 2D array texture (reusing `reuse` when it has that shape) with a blit
// committed on the backend's queue, so it runs after the passes that rendered the slices and before
// work committed later that samples the array. Throws std::invalid_argument when a slice has no
// resident surface of the base slice's extent, format and sample count.
[[nodiscard]] id<MTLTexture> GatherDepthSlices(const MetalDevice& backend, std::span<const std::shared_ptr<MetalDepthSurface>> surfaces,
    const MetalDepthSurface& base, std::uint32_t first, std::uint32_t last, id<MTLTexture> reuse);

// A color target with several fragments (CB_COLOR_ATTRIB NUM_FRAGMENTS) renders into a resident 2D
// multisample texture, like multisampled depth: the guest's interleaved fragment and FMASK layout is
// not modeled, so the fragments never round-trip through guest memory. Shaders that read the
// surface through a 2D MSAA image descriptor bind this texture. A new surface starts cleared to zero.
class MetalColorSurface {
public:
    MetalColorSurface(const MetalDevice& backend, const Graphics::ColorTarget& target);
    MetalColorSurface(const MetalColorSurface&) = delete;
    MetalColorSurface& operator=(const MetalColorSurface&) = delete;
    [[nodiscard]] id<MTLTexture> Texture() const;
    [[nodiscard]] const Graphics::ColorTarget& Target() const;
    [[nodiscard]] bool Matches(const Graphics::ColorTarget& other) const;
    [[nodiscard]] bool OverlapsMappings(std::span<const NativeGuestMemory::BorrowedRange> changed) const;

private:
    Graphics::ColorTarget target;
    id<MTLTexture> texture;
};

class MetalDepthSurfaceCache {
public:
    explicit MetalDepthSurfaceCache(const MetalDevice& backend);
    [[nodiscard]] std::shared_ptr<MetalDepthSurface> Acquire(const Graphics::DepthTarget& target);
    [[nodiscard]] std::shared_ptr<MetalColorSurface> Acquire(const Graphics::ColorTarget& target);
    void Clear();
    void Invalidate(std::span<const NativeGuestMemory::BorrowedRange> changed);

private:
    const MetalDevice& backend;
    std::mutex mutex;
    std::vector<std::shared_ptr<MetalDepthSurface>> surfaces;
    std::vector<std::shared_ptr<MetalColorSurface>> colorSurfaces;
};

// A single-fragment color target in a multisampled pass renders through a transient multisample
// texture: Expand fills every sample of it with the target's texels before the draw, and the draw
// resolves (averages) the samples back into the target. Untouched pixels resolve to their original
// value exactly; covered edges get the averaged coverage a 1-fragment EQAA target would not store.
class MetalMultisampleExpander {
public:
    explicit MetalMultisampleExpander(const MetalDevice& backend);
    MetalMultisampleExpander(const MetalMultisampleExpander&) = delete;
    MetalMultisampleExpander& operator=(const MetalMultisampleExpander&) = delete;
    // `source` must be a single-level, single-slice 2D float-readable color texture. Each attachment
    // slot owns its transient, so the attachments of one pass never share one.
    [[nodiscard]] id<MTLTexture> Expand(id<MTLCommandBuffer> commands, id<MTLTexture> source, std::uint32_t samples, std::uint32_t slot);

private:
    const MetalDevice& backend;
    std::mutex mutex;
    id<MTLLibrary> library;
    std::vector<std::pair<std::uint64_t, id<MTLRenderPipelineState>>> pipelines;
    std::vector<std::pair<std::uint32_t, id<MTLTexture>>> transients;
};

}
