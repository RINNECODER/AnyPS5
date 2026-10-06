#pragma once

#include "MetalDevice.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Shaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <mutex>
#include <memory>

namespace AgcDriver::Metal {

class MetalDepthSurfaceCache;
class MetalDepthSurface;

class MetalDraw {
public:
    explicit MetalDraw(id<MTLDevice> device, id<MTLLibrary> utilityLibrary);
    ~MetalDraw();
    MetalDraw(const MetalDraw&) = delete;
    MetalDraw& operator=(const MetalDraw&) = delete;
    [[nodiscard]] ShaderRecompiler::BdaAbi::Fault DrawSynchronously(
        const Graphics::State& state, const Pm4::DrawParameters& draw,
        std::span<const Graphics::CompiledShader> shaders,
        std::span<const NativeGuestMemory::BorrowedRange> ranges);

private:
    MetalDevice backend;
    std::unique_ptr<MetalDepthSurfaceCache> depthCache;
    std::vector<std::shared_ptr<MetalDepthSurface>> depthSurfaces;
    std::mutex drawMutex;
};

}
