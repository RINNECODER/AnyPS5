#pragma once

#include "prx/libSceVideoOut/include/NativeHostWindow.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <filesystem>
#include <memory>
#include <span>

namespace AnyPS5::Host {

struct NativeMetalSessionConfiguration {
    WindowConfiguration window;
    std::filesystem::path utilityMetallib;
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> initialRanges;
    std::shared_ptr<const void> initialRangeOwner;
    std::uint64_t initialGeneration = 0;
    AgcDriver::Metal::MetalDriver::EopInterrupt eopInterrupt;
};

class NativeMetalSession {
public:
    static std::unique_ptr<NativeMetalSession> CreateMainThread(
        const NativeMetalSessionConfiguration& configuration);
    ~NativeMetalSession();
    NativeMetalSession(const NativeMetalSession&) = delete;
    NativeMetalSession& operator=(const NativeMetalSession&) = delete;
    AgcDriver::Metal::MetalDriver& Driver();
    NativeHostWindow& Window();
    void ShutdownAfterCpuStoppedMainThread();

private:
    struct Impl;
    explicit NativeMetalSession(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};

}
