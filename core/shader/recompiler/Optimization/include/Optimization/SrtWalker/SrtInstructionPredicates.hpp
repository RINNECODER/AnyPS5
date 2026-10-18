#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTINSTRUCTIONPREDICATES_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTINSTRUCTIONPREDICATES_HPP

#include "IntermediateRepresentation/IrProgram.hpp"

namespace ShaderRecompiler::Detail {

bool IsRawRead(const IrResourcePlan& program, const IrValue& inst);
bool IsRawRead(const IrResourcePlan& program, IrOpcode op, std::uint32_t memoryIndex);
bool LoadedFromMemory(const IrValue& handle);
bool IsDescriptorHandle(IrOpcode opcode);
bool IsRuntimeSelect(IrOpcode opcode);
bool IsRuntimeUniformOp(IrOpcode opcode);
bool ActiveMaskImplies(const IrValue* mask, const IrValue* condition);

template<typename TNode>
bool ActiveMaskImplies(const TNode& mask, const TNode& condition) {
    return ActiveMaskImplies(mask.Value(), condition.Value());
}

}

#endif
