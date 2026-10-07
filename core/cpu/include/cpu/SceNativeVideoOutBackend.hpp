#pragma once

#include <cpu/SceVideoOutImports.hpp>
#include <cstdint>
#include <memory>
#include <stop_token>
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"

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

// These spans must be the actual driver mappings, owned by Owner. All later
// publication uses MutateBorrowedRanges; direct driver mapping mutation is not
// a supported production path while this VideoOut backend is active.
struct SceVideoOutMemoryConfiguration {
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> Ranges;
    std::shared_ptr<const void> Owner;
    std::uint64_t Generation = 0;
};

class SceNativeVideoOutBackend {
public:
    SceNativeVideoOutBackend(Machine& machine, const AgcDriver::PresentationWindow& window,
                             const VideoOutCompletionCallbacks& completion,
                             std::stop_token processStop = {},
                             const SceVideoOutMemoryConfiguration& memory = {});
    ~SceNativeVideoOutBackend();
    SceNativeVideoOutBackend(const SceNativeVideoOutBackend&) = delete;
    SceNativeVideoOutBackend& operator=(const SceNativeVideoOutBackend&) = delete;
    SceVideoOutBackend GetCallbacks() const;
    std::shared_ptr<VideoOutConfig> GetConfig(std::int32_t handle) const;
    // Preserve every registered extent through mapping generations. Removal,
    // rebind or identity change is rejected before either CPU or Metal mutates.
    void MutateBorrowedRanges(std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
        std::uint64_t generation, const std::function<void()>& mutateCpu,
        std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner);
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
        std::uint64_t videoOutGateBase = 0x7ffdfd000000,
        std::span<const VideoOutAbiAdmission> admissions = {});
    ~SceNativeGraphicsSession();
    SceNativeGraphicsSession(const SceNativeGraphicsSession&) = delete;
    SceNativeGraphicsSession& operator=(const SceNativeGraphicsSession&) = delete;
    AnyPS5::Host::NativeHostWindow& Window();
    AgcDriver::Metal::MetalDriver& Driver();
    std::uint64_t ResolveVideoOut(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize);
    std::uint64_t ResolveVideoOutPublicFixture(const SceImport& import);
    void MutateBorrowedRanges(std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,
        std::uint64_t generation, const std::function<void()>& mutateCpu,
        std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner);
    void RequestStop();
    void ShutdownAfterCpuStoppedMainThread();
private:
    struct Impl;
    explicit SceNativeGraphicsSession(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};

}
