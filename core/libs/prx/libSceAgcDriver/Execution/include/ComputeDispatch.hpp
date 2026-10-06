#pragma once

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver {

struct ComputeDispatchState {
    std::uint64_t programAddress;
    std::vector<std::uint32_t> userData;
    ShaderRecompiler::ShaderComputeStageInfo compute;
    std::uint32_t waveSize;
    std::array<std::uint32_t, 3> groups;
};

ComputeDispatchState DecodeComputeDispatch(const QueueState& queue, std::span<const std::uint32_t> packet,
                                         void (*noteComputeRegisterRead)(std::uint32_t) = nullptr);

namespace Graphics {
ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader,
                                                               void (*noteRegisterRead)(std::uint32_t));
}

}
