#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP

#import <Metal/Metal.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

namespace AgcDriver::Metal {

// A committed command buffer failed, the device could not create a command
// buffer or encoder, or work that already ran on the GPU faulted or could not
// be completed: the GPU or the guest memory it wrote is in doubt. The driver
// keeps this sticky; per-draw and per-dispatch failures before any result is
// copied back to guest memory (unsupported state, shader translation, pipeline
// creation) are skipped instead.
class MetalGpuExecutionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Runs the completion of a committed command buffer (fault readback, copy-back
// to guest memory). The GPU work has executed by then, so any failure is a
// MetalGpuExecutionError rather than a skippable packet failure.
template <typename Work>
decltype(auto) CompleteCommittedWork(Work&& work) {
    try {
        return work();
    } catch (const MetalGpuExecutionError&) {
        throw;
    } catch (const std::exception& error) {
        throw MetalGpuExecutionError(error.what());
    }
}

class MetalDevice {
public:
    explicit MetalDevice(id<MTLDevice> device, id<MTLLibrary> library);
    MetalDevice(const MetalDevice&) = delete;
    MetalDevice& operator=(const MetalDevice&) = delete;

    [[nodiscard]] id<MTLBuffer> Buffer(std::size_t bytes) const;
    [[nodiscard]] id<MTLCommandBuffer> CommandBuffer() const;
    void Encode(id<MTLCommandBuffer> commands, NSString* kernel, NSArray<id<MTLBuffer>>* buffers,
                const void* parameters, std::size_t parameterBytes, MTLSize grid,
                const std::array<std::uint32_t, 22>* constants = nullptr) const;
    void Wait(id<MTLCommandBuffer> commands) const;
    [[nodiscard]] id<MTLDevice> Device() const;
    [[nodiscard]] id<MTLCommandQueue> Queue() const;

private:
    struct PipelineKey {
        std::string kernel;
        std::array<std::uint32_t, 22> constants{};
        bool specialized = false;
        auto operator<=>(const PipelineKey&) const = default;
    };

    id<MTLComputePipelineState> pipeline(NSString* kernel,
                                       const std::array<std::uint32_t, 22>* constants) const;
    void validateCommands(id<MTLCommandBuffer> commands) const;

    id<MTLDevice> device;
    id<MTLLibrary> library;
    id<MTLCommandQueue> queue;
    mutable std::mutex pipelineMutex;
    mutable std::map<PipelineKey, id<MTLComputePipelineState>> pipelines;
};

}

#endif
