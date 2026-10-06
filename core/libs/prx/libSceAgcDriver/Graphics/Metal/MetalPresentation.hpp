#pragma once

#include "MetalDevice.hpp"
#include "MetalPresenter.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include <map>
#include <mutex>

namespace AgcDriver::Metal {

class MetalPresentation {
public:
    MetalPresentation(const MetalDevice& backend, id<MTLLibrary> library);
    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
                 void (*gpuReady)(void*), void* completionContext);
    void ReleaseWindow(void* context);

private:
    const MetalDevice& backend;
    MetalPresenter presenter;
    std::mutex mutex;
    std::map<void*, CAMetalLayer*> layers;
};

}
