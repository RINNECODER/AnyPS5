#pragma once

#include <cpu/SceVideoOutImports.hpp>
#include <cstdint>
#include <memory>
#include <stop_token>

struct VideoOutConfig;
struct VideoOutCompletionCallbacks;

namespace AgcDriver {
struct PresentationWindow;
}

namespace Cpu {

class SceNativeVideoOutBackend {
public:
    SceNativeVideoOutBackend(Machine& machine, const AgcDriver::PresentationWindow& window,
                             const VideoOutCompletionCallbacks& completion,
                             std::stop_token processStop = {});
    ~SceNativeVideoOutBackend();
    SceNativeVideoOutBackend(const SceNativeVideoOutBackend&) = delete;
    SceNativeVideoOutBackend& operator=(const SceNativeVideoOutBackend&) = delete;
    SceVideoOutBackend GetCallbacks() const;
    std::shared_ptr<VideoOutConfig> GetConfig(std::int32_t handle) const;
    void RequestStop();
    void Shutdown();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
