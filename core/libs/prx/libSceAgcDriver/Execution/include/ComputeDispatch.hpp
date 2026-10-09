#pragma once

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "Recompiler.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver {

struct ComputeDispatchState {
    std::uint64_t programAddress;
    // USER_DATA registers the program declares but the command buffer never wrote read as zero.
    std::vector<std::uint32_t> userData;
    // Includes scratchDwords when COMPUTE_PGM_RSRC2.SCRATCH_EN is set.
    ShaderRecompiler::ShaderComputeStageInfo compute;
    std::uint32_t waveSize;
    std::array<std::uint32_t, 3> groups;
};

// COMPUTE_PGM_LO/HI. Lets a caller find the registered shader (and its AGC header) before decoding
// the rest of the dispatch.
std::uint64_t DecodeComputeProgramAddress(const Registers& shader);

// `header` is the program's registered AGC shader header; a dispatch with SCRATCH_EN takes its
// per-lane scratch size from it and is rejected without one. The shader float mode is not decoded
// here: it comes from the registered shader state (DriverDetail::RegisteredFloatMode).
ComputeDispatchState DecodeComputeDispatch(const QueueState& queue, std::span<const std::uint32_t> packet,
                                         std::span<const std::byte> header = {},
                                         void (*noteComputeRegisterRead)(std::uint32_t) = nullptr);

namespace Graphics {
ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader, std::span<const std::byte> header,
                                                               void (*noteRegisterRead)(std::uint32_t));
}

}
