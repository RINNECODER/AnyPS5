#pragma once

#include "MetalDevice.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <memory>
#include <mutex>
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

class MetalDepthSurfaceCache {
public:
    explicit MetalDepthSurfaceCache(const MetalDevice& backend);
    [[nodiscard]] std::shared_ptr<MetalDepthSurface> Acquire(const Graphics::DepthTarget& target);
    void Clear();
    void Invalidate(std::span<const NativeGuestMemory::BorrowedRange> changed);

private:
    const MetalDevice& backend;
    std::mutex mutex;
    std::vector<std::shared_ptr<MetalDepthSurface>> surfaces;
};

}
