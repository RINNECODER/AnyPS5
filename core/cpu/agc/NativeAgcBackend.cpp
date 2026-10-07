#include "NativeAgcBackend.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <vector>

namespace Cpu {
SceAgcBackend MakeNativeAgcBackend(AgcDriver::Metal::MetalDriver& driver) {
    SceAgcBackend backend;
    backend.Submit = [&driver](std::uint64_t address, std::uint32_t words, std::uint8_t flags, std::uint32_t queue) {
        driver.SubmitCommandBuffer(address, words, flags, queue);
    };
    backend.RegisterShader = [&driver](std::uint64_t address, std::span<const AgcReadableRange> ranges,
                                      const std::function<void()>& publish, const std::function<void()>& rollback) {
        std::vector<AgcDriver::Metal::ReadableGuestRange> native;
        native.reserve(ranges.size());
        for (const auto& range : ranges) native.push_back({range.Address, range.Size});
        driver.RegisterShaderWithPublication(address, native, publish, rollback);
    };
    backend.Suspend = [&driver] { driver.SuspendPoint(); };
    return backend;
}
}
