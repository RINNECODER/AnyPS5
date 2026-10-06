#pragma once

#import <Metal/Metal.h>
#include "BdaAbi.hpp"
#include "prx/libSceAgcDriver/Execution/include/ComputeDispatch.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include <mutex>

namespace AgcDriver::Metal {

class MetalComputeDispatch {
public:
    explicit MetalComputeDispatch(id<MTLDevice> device);
    MetalComputeDispatch(const MetalComputeDispatch&) = delete;
    MetalComputeDispatch& operator=(const MetalComputeDispatch&) = delete;

    [[nodiscard]] ShaderRecompiler::BdaAbi::Fault DispatchSynchronously(
        const ComputeDispatchState& state, std::span<const std::uint32_t> checkedCode,
        std::span<const NativeGuestMemory::BorrowedRange> ranges,
        std::uint64_t checkedHeaderAddress = 0, std::span<const std::byte> checkedHeader = {});

private:
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    std::mutex dispatchMutex;
};

}
