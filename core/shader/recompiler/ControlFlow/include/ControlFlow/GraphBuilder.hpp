#ifndef CORE_SHADER_RECOMPILIER_CONTROLFLOW_INCLUDE_CONTROLFLOW_GRAPHBUILDER_HPP
#define CORE_SHADER_RECOMPILIER_CONTROLFLOW_INCLUDE_CONTROLFLOW_GRAPHBUILDER_HPP

#include "ControlFlow/ControlFlowGraph.hpp"
#include "RdnaDecoder/RdnaProgram.hpp"
#include <vector>

namespace ShaderRecompiler {

struct StaticScalarCall {
    std::uint32_t callIndex = 0;
    std::uint32_t linkRegister = 0;
    std::uint32_t targetIndex = 0;
    std::uint32_t targetProgramCounter = 0;
    std::uint32_t returnIndex = 0;
    std::uint32_t returnTargetProgramCounter = 0;
};

class GraphBuilder {
public:
    [[nodiscard]] ControlFlowGraph Build(const RdnaProgram& program) const;

private:
    [[nodiscard]] std::vector<BasicBlock> splitIntoBlocks(const RdnaProgram& program, const std::vector<StaticScalarCall>& calls) const;
    void linkBlocks(std::vector<BasicBlock>& blocks, const RdnaProgram& program, const std::vector<StaticScalarCall>& calls) const;
};

}

#endif
