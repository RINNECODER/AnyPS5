#pragma once

#include "MetalDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

// The textures behind one depth allocation: a 2D array (one layer per DB_DEPTH_VIEW slice) for a
// single-sample surface, a 2D multisample texture otherwise. Every slice surface of a depth array is a
// view of one layer of the same planes, so a layered draw, a single-slice draw and a 2D-array sampler
// all see one copy of the texels.
struct MetalDepthPlanes {
    id<MTLTexture> depth = nil;
    id<MTLTexture> stencil = nil; // == depth for a combined depth/stencil format
    std::uint32_t layers = 1;
    std::uint64_t generation = 0; // bumped whenever the textures are replaced (grown)
};

// A plane of one surface as a sampler would view it: the planes texture, the pixel format to view it
// as and the layer of the surface.
struct MetalDepthPlaneView {
    id<MTLTexture> texture = nil;
    MTLPixelFormat format = MTLPixelFormatInvalid;
    std::uint32_t layer = 0;
};

class MetalDepthSurface {
public:
    // A surface with planes of its own (one layer), cleared to the target's clear values.
    MetalDepthSurface(const MetalDevice& backend, const Graphics::DepthTarget& target);
    // A view of layer `layer` of shared planes; the layer keeps the texels it holds.
    MetalDepthSurface(const Graphics::DepthTarget& target, std::shared_ptr<MetalDepthPlanes> planes, std::uint32_t layer);
    MetalDepthSurface(const MetalDepthSurface&) = delete;
    MetalDepthSurface& operator=(const MetalDepthSurface&) = delete;
    [[nodiscard]] id<MTLTexture> DepthTexture() const;
    [[nodiscard]] id<MTLTexture> StencilTexture() const;
    [[nodiscard]] id<MTLTexture> SampledDepthView() const;
    [[nodiscard]] id<MTLTexture> SampledStencilView() const;
    [[nodiscard]] MetalDepthPlaneView SampledDepthPlane() const;
    [[nodiscard]] MetalDepthPlaneView SampledStencilPlane() const;
    // 2D-array views of `count` layers from this surface's layer on, for a layered render pass (nil
    // stencil when the format has none).
    [[nodiscard]] id<MTLTexture> LayeredDepthTexture(std::uint32_t count) const;
    [[nodiscard]] id<MTLTexture> LayeredStencilTexture(std::uint32_t count) const;
    [[nodiscard]] const Graphics::DepthTarget& Target() const;
    [[nodiscard]] const std::shared_ptr<MetalDepthPlanes>& Planes() const;
    [[nodiscard]] std::uint32_t Layer() const;
    // Moves the surface to layer `layer` of other planes (the caller copies its texels there first).
    void Rebind(std::shared_ptr<MetalDepthPlanes> planes, std::uint32_t layer);
    [[nodiscard]] bool OverlapsMappings(std::span<const NativeGuestMemory::BorrowedRange> changed) const;
    // Whether guest bytes [address, address + bytes) overlap the depth or stencil plane.
    [[nodiscard]] bool Overlaps(std::uint64_t address, std::uint64_t bytes) const;

private:
    Graphics::DepthTarget target;
    std::shared_ptr<MetalDepthPlanes> planes;
    std::uint32_t layer = 0;
    // Single-layer views of the planes, made again when the planes grow.
    void refreshViews() const;
    mutable std::uint64_t viewGeneration = ~0ull;
    mutable id<MTLTexture> depth;
    mutable id<MTLTexture> stencil;
    mutable id<MTLTexture> sampledStencil;
};

// Creates planes of `layers` layers in the target's shape, every layer cleared to its clear values.
[[nodiscard]] std::shared_ptr<MetalDepthPlanes> CreateDepthPlanes(const MetalDevice& backend, const Graphics::DepthTarget& target, std::uint32_t layers);

class MetalDepthSurfaceCache;

// Cascaded shadow maps: each slice of a depth array is rendered through DB_DEPTH_VIEW SLICE_START as a
// resident surface of its own (State.cpp offsets the address by whole slices), or all of them by one
// layered draw, and a later pass samples them through one 2D-array descriptor at the address of slice
// 0, `base`. When the slices are consecutive layers of one set of planes (MetalDepthSurfaceCache keeps
// them so), this is a view of those layers. Otherwise it copies the depth plane of slices first..last
// into a 2D array texture (reusing `reuse` when it has that shape and is not a view) with a blit
// committed on the backend's queue, so it runs after the passes that rendered the slices and before
// work committed later that samples the array. Throws std::invalid_argument when a slice has no
// resident surface of the base slice's extent, format and sample count.
[[nodiscard]] id<MTLTexture> GatherDepthSlices(const MetalDevice& backend, std::span<const std::shared_ptr<MetalDepthSurface>> surfaces,
    const MetalDepthSurface& base, std::uint32_t first, std::uint32_t last, id<MTLTexture> reuse);

// Guest memory reused: a texture descriptor at the address of a resident depth or stencil plane whose
// texels cannot be that plane's (a block-compressed format, or another texel size) is another resource
// placed where the depth target was. Its texels are in guest memory and the surface's are dead, so the
// surfaces at that address are dropped from `surfaces` and the cache (a later draw makes a new one).
void DropReplacedDepthSurfaces(std::vector<std::shared_ptr<MetalDepthSurface>>& surfaces, MetalDepthSurfaceCache& cache,
    const Graphics::GuestTextureResource& resource);

// Another writer over a depth surface's memory: a depth surface lives only in its Metal textures, so a
// color target or another depth target bound over its depth or stencil plane leaves it stale. It is
// retired: dropped from the resident `surfaces` a draw samples and gathers from (all but `keep`), so a
// view of that memory reads it like any texture. The cache keeps it; binding its depth target again
// brings it back with the texels it held.
void RetireDepthSurfaces(std::vector<std::shared_ptr<MetalDepthSurface>>& surfaces, std::uint64_t address, std::uint64_t bytes,
    const MetalDepthSurface* keep = nullptr);
// The same over the depth and stencil planes of a depth target, or over every fragment of a color target.
void RetireDepthSurfaces(std::vector<std::shared_ptr<MetalDepthSurface>>& surfaces, const Graphics::DepthTarget& target,
    const MetalDepthSurface* keep = nullptr);
void RetireDepthSurfaces(std::vector<std::shared_ptr<MetalDepthSurface>>& surfaces, const Graphics::ColorTarget& target,
    const MetalDepthSurface* keep = nullptr);

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
    // A single-sample target with DB_DEPTH_VIEW slices (DepthTarget::slice/lastSlice) gets a layer of
    // the planes of its depth array, which grow to hold slices 0..max(slice, lastSlice); a surface
    // found at the target's address in other planes moves there with its texels.
    // A single-sample target without slices takes the current contents of each of its planes: a depth
    // plane bound with several stencil planes (or a stencil plane with several depth planes) is a
    // surface per pairing, and binding one copies that plane over from the surface that bound it last
    // when both have its format and extent.
    [[nodiscard]] std::shared_ptr<MetalDepthSurface> Acquire(const Graphics::DepthTarget& target);
    [[nodiscard]] std::shared_ptr<MetalColorSurface> Acquire(const Graphics::ColorTarget& target);
    void Clear();
    void Invalidate(std::span<const NativeGuestMemory::BorrowedRange> changed);
    // Drops one surface whose guest memory now holds another resource.
    void Forget(const std::shared_ptr<MetalDepthSurface>& surface);

private:
    void growPlanes(const std::shared_ptr<MetalDepthPlanes>& planes, const Graphics::DepthTarget& shape, std::uint32_t layers);
    void takePlanes(const std::shared_ptr<MetalDepthSurface>& surface);
    const MetalDevice& backend;
    // The surface whose plane at each guest address holds that plane's current texels.
    std::unordered_map<std::uint64_t, std::weak_ptr<MetalDepthSurface>> depthOwners;
    std::unordered_map<std::uint64_t, std::weak_ptr<MetalDepthSurface>> stencilOwners;
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
