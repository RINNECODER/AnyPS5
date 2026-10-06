#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALDEVICE_HPP

#import <Metal/Metal.h>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace AgcDriver::Metal {

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
