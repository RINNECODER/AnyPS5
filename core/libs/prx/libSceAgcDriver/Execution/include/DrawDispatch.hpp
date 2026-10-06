#pragma once

#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace AgcDriver {

struct DrawDispatchProgram {
    ShaderRecompiler::ShaderBinary binary;
    std::uint32_t userDataBase;
    std::uint32_t firstUserSgpr = 8;
    std::vector<std::uint32_t> userData;
    std::array<ShaderRecompiler::MemoryRegion, 2> memory;
    std::shared_ptr<const DriverDetail::ShaderSnapshot> snapshot;
    std::size_t codeOffset = 0;
};

struct DrawDispatchState {
    Graphics::State state;
    ShaderRecompiler::ShaderPixelStageInfo pixel;
    std::vector<DrawDispatchProgram> programs;
    std::vector<ShaderRecompiler::ProgramRole> roles;
};

DrawDispatchState DecodeDrawDispatch(const QueueState& queue, const DriverDetail::ShaderRegistry& shaders,
                                    std::uint64_t nullPixelProgramAddress = 0);

ShaderRecompiler::RecompileRequest BuildDrawRecompileRequest(
    const ShaderRecompiler::ShaderBinary& binary, std::uint32_t firstUserSgpr,
    std::span<const std::uint32_t> userData, const Graphics::State& graphics,
    const ShaderRecompiler::ShaderPixelStageInfo& pixel,
    const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo,
    const ShaderRecompiler::SpirvTarget& target, std::uint32_t pushOffset,
    const Pm4::DrawParameters& drawParameters, std::span<const ShaderRecompiler::MemoryRegion> memory,
    std::span<const ShaderRecompiler::LinkedProgram> linked);

void FoldDrawOffsets(const ShaderRecompiler::RecompileResult& main, std::uint32_t firstUserSgpr,
                     std::span<const std::uint32_t> userData, Pm4::DrawParameters& parameters);

}
