#pragma once

#include "MetalDevice.hpp"
#include "MetalPresenter.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include <map>
#include <vector>
#include <mutex>

namespace AgcDriver::Metal {

class MetalPresentation {
public:
    MetalPresentation(const MetalDevice& backend, id<MTLLibrary> library);
    // `pixels`, when given, are the display buffer's decoded pixels (ReadPixels), read by the
    // caller while it held the guest ranges; otherwise Present reads them itself.
    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
                 void (*gpuReady)(void*), void* completionContext, const std::vector<std::byte>* pixels = nullptr);
    // The scanout pixels of `buffer` from guest memory (borrowed ranges in scope).
    [[nodiscard]] static std::vector<std::byte> ReadPixels(const DisplayBuffer& buffer);
    void ReleaseWindow(void* context);

private:
    const MetalDevice& backend;
    MetalPresenter presenter;
    std::mutex mutex;
    std::map<void*, CAMetalLayer*> layers;
};

}
