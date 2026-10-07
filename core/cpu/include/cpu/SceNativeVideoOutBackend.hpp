#pragma once

#include <cpu/SceVideoOutImports.hpp>
#include <cstdint>
#include <memory>
#include <stop_token>

struct VideoOutConfig;
struct VideoOutCompletionCallbacks;

namespace AgcDriver {
struct PresentationWindow;
namespace Metal { class MetalDriver; }
}

namespace AnyPS5::Host {
class NativeHostWindow;
struct NativeMetalSessionConfiguration;
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

// Graphics lifetime owner. Create/destroy on the AppKit main thread. The CPU and
// all guest-memory publishers must be stopped and joined before shutdown or
// destruction; the Machine and completion callback context must outlive this.
class SceNativeGraphicsSession {
public:
    static std::unique_ptr<SceNativeGraphicsSession> CreateMainThread(
        Machine& machine, const AnyPS5::Host::NativeMetalSessionConfiguration& configuration,
        const VideoOutCompletionCallbacks& completion, std::stop_token processStop = {},
        std::uint64_t videoOutGateBase = 0x7ffdfd000000);
    ~SceNativeGraphicsSession();
    SceNativeGraphicsSession(const SceNativeGraphicsSession&) = delete;
    SceNativeGraphicsSession& operator=(const SceNativeGraphicsSession&) = delete;
    AnyPS5::Host::NativeHostWindow& Window();
    AgcDriver::Metal::MetalDriver& Driver();
    std::uint64_t ResolveVideoOut(const SceImport& import);
    void RequestStop();
    void ShutdownAfterCpuStoppedMainThread();
private:
    struct Impl;
    explicit SceNativeGraphicsSession(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};

}
