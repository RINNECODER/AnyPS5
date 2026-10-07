#include "Translation/InstructionTranslator.hpp"
#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaImageOpDecoder.hpp"
#include "Translation/DispatchInstructions.hpp"
#include "Translation/TranslationContext.hpp"
#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace ShaderRecompiler {

namespace {

IrShaderStage toIrShaderStage(ShaderStageKind stage) {
    switch (stage) {
    case ShaderStageKind::Unknown:
        return IrShaderStage::Unknown;
    case ShaderStageKind::Vertex:
        return IrShaderStage::Vertex;
    case ShaderStageKind::Pixel:
        return IrShaderStage::Pixel;
    case ShaderStageKind::Fetch:
        return IrShaderStage::Fetch;
    case ShaderStageKind::Compute:
        return IrShaderStage::Compute;
    case ShaderStageKind::Mesh:
        return IrShaderStage::Mesh;
    case ShaderStageKind::Local:
        return IrShaderStage::Local;
    case ShaderStageKind::TessellationControl:
        return IrShaderStage::TessellationControl;
    case ShaderStageKind::TessellationEvaluation:
        return IrShaderStage::TessellationEvaluation;
    }
    throw std::runtime_error("InstructionTranslator::Translate unknown shader stage kind");
}

struct InstructionRegisterAccess {
    std::set<std::uint32_t> reads;
    std::set<std::uint32_t> writes;
    bool writesExec = false;
};

bool isInactiveDestinationPreservation(const IrValue& value) {
    if (value.OperandUses().size() != 1u) return false;
    const auto& use = value.OperandUses().front();
    const auto* select = use.user;
    if (use.operand != 2u || select->Opcode() != IrOpcode::SelectU32 ||
        select->Argument(0u)->Opcode() != IrOpcode::GetExec || select->OperandUses().size() != 1u) return false;
    const auto& stored = select->OperandUses().front();
    return stored.operand == 1u && stored.user->Opcode() == IrOpcode::SetVectorRegister &&
           stored.user->Argument(0u)->Register() == value.Argument(0u)->Register();
}

InstructionRegisterAccess instructionRegisterAccess(const IrBlock& block, std::list<IrValue*>::const_iterator first) {
    InstructionRegisterAccess access;
    const auto& instructions = block.Instructions();
    for (auto at = first; at != instructions.end(); ++at) {
        const auto& value = **at;
        switch (value.Opcode()) {
        case IrOpcode::GetVectorRegister:
            if (!isInactiveDestinationPreservation(value)) access.reads.insert(value.Argument(0u)->Register().index);
            break;
        case IrOpcode::SetVectorRegister:
            access.writes.insert(value.Argument(0u)->Register().index);
            break;
        case IrOpcode::SetExec:
        case IrOpcode::SetExecLo:
        case IrOpcode::SetExecHi:
            access.writesExec = true;
            break;
        default:
            break;
        }
    }
    return access;
}

void validateFixedFunctionInterpolation(const RdnaProgram& decoded, const ControlFlowGraph& cfg, const ShaderPixelInputInfo& pixel, const std::vector<InstructionRegisterAccess>& accesses) {
    std::set<std::uint32_t> live;
    std::set<std::uint32_t> centers;
    for (const auto input : {PixelInput::PerspectiveSample, PixelInput::PerspectiveCenter,
                             PixelInput::PerspectiveCentroid, PixelInput::PerspectivePullModel,
                             PixelInput::LinearSample, PixelInput::LinearCenter, PixelInput::LinearCentroid}) {
        const auto base = pixel.psInputVgpr[static_cast<std::size_t>(input)];
        if (base == ShaderPixelInputInfo::NoPixelInputVgpr) continue;
        live.insert(base);
        live.insert(base + 1u);
        if (input == PixelInput::PerspectivePullModel) live.insert(base + 2u);
        if (input == PixelInput::PerspectiveCenter || input == PixelInput::LinearCenter) centers.insert(base);
    }
    const bool hasInterpolation = std::any_of(decoded.instructions.begin(), decoded.instructions.end(), [](const RdnaInstruction& instruction) {
        return instruction.op == RdnaOpcode::VInterpP1F32 || instruction.op == RdnaOpcode::VInterpP2F32;
    });
    if (!hasInterpolation && live.empty()) return;
    std::uint32_t pc = 0u;
    const auto fail = [&] {
        throw std::runtime_error("fixed-function interpolation requires unmodified center I/J pairs and complete P1/P2 sequences in an unconditional entry block (pc=" + std::to_string(pc) + ")");
    };
    const auto entry = std::find_if(cfg.blocks.begin(), cfg.blocks.end(), [&](const BasicBlock& block) { return block.id == cfg.entryBlock; });
    if (cfg.unsupported || cfg.irreducible || entry == cfg.blocks.end() || entry != cfg.blocks.begin() ||
        entry->instructionBegin != 0u || entry->instructionEnd == 0u || entry->instructionEnd > decoded.instructions.size() || !entry->predecessors.empty()) fail();
    struct Pair { std::uint32_t base, attribute, component; };
    std::map<std::uint32_t, Pair> pending;
    const auto plain = [](const RdnaOperand& operand) {
        return !operand.negate && !operand.negateHi && !operand.absolute && !operand.clamp && !operand.omod &&
               !operand.opSel && !operand.opSelHi && !operand.dpp && !operand.dpp8 && !operand.explicitSdwaDst &&
               !operand.sdwaSext && operand.sdwaSel == 6u;
    };
    for (std::size_t index = 0u; index < decoded.instructions.size(); ++index) {
        const auto& instruction = decoded.instructions[index];
        pc = instruction.programCounter;
        if (index >= entry->instructionEnd && cfg.blocks.size() > 1u && (!live.empty() || !pending.empty())) fail();
        if (instruction.op == RdnaOpcode::VInterpMovF32) {
            if ((!live.empty() || !pending.empty()) && (instruction.destination.kind != RdnaOperandKind::VectorRegister ||
                !plain(instruction.destination) || live.contains(instruction.destination.reg) || pending.contains(instruction.destination.reg))) fail();
            continue;
        }
        if (instruction.op == RdnaOpcode::VInterpP1F32 || instruction.op == RdnaOpcode::VInterpP2F32) {
            if (index >= entry->instructionEnd || instruction.source0.kind != RdnaOperandKind::VectorRegister ||
                instruction.destination.kind != RdnaOperandKind::VectorRegister || !plain(instruction.source0) ||
                !plain(instruction.source1) || !plain(instruction.source2) || !plain(instruction.destination) || instruction.clampResult ||
                instruction.source1.value >= 32u || instruction.source1.value >= pixel.inputNum ||
                instruction.source2.value >= 4u || pixel.InputIsCustom(instruction.source1.value)) fail();
            if (instruction.op == RdnaOpcode::VInterpP1F32) {
                if (!centers.contains(instruction.source0.reg) || !live.contains(instruction.source0.reg) ||
                    !live.contains(instruction.source0.reg + 1u) || pending.contains(instruction.destination.reg) ||
                    (live.contains(instruction.destination.reg) && instruction.destination.reg != instruction.source0.reg)) fail();
                pending.emplace(instruction.destination.reg, Pair{instruction.source0.reg, instruction.source1.value, instruction.source2.value});
            } else {
                const auto pair = pending.find(instruction.destination.reg);
                if (pair == pending.end() || instruction.source0.reg != pair->second.base + 1u ||
                    !live.contains(instruction.source0.reg) || pending.contains(instruction.source0.reg) ||
                    pair->second.attribute != instruction.source1.value || pair->second.component != instruction.source2.value) fail();
                pending.erase(pair);
            }
            live.erase(instruction.destination.reg);
            continue;
        }
        if (live.empty() && pending.empty()) continue;
        const bool scalarAlu = IsScalarAluOpcode(instruction.op);
        const bool vectorFamily = instruction.family == RdnaInstructionFamily::VOP1 || instruction.family == RdnaInstructionFamily::VOP2 ||
                                  instruction.family == RdnaInstructionFamily::VOP3 || instruction.family == RdnaInstructionFamily::VOP3P ||
                                  instruction.family == RdnaInstructionFamily::VOPC;
        const bool indirectVector = instruction.op == RdnaOpcode::VMovrelsB32 || instruction.op == RdnaOpcode::VMovreldB32 ||
                                    instruction.op == RdnaOpcode::VMovrelsdB32 || instruction.op == RdnaOpcode::VMovrelsd2B32 ||
                                    instruction.op == RdnaOpcode::VSwaprelB32 || instruction.op == RdnaOpcode::VSwapB32 ||
                                    instruction.op == RdnaOpcode::VReadfirstlaneB32 || instruction.op == RdnaOpcode::VReadlaneB32 ||
                                    instruction.op == RdnaOpcode::VWritelaneB32 || instruction.op == RdnaOpcode::VPermlane16B32 ||
                                    instruction.op == RdnaOpcode::VPermlanex16B32;
        const bool vectorAlu = vectorFamily && !indirectVector;
        const bool memory = instruction.family == RdnaInstructionFamily::SMEM || instruction.family == RdnaInstructionFamily::MUBUF ||
                            instruction.family == RdnaInstructionFamily::MTBUF || instruction.family == RdnaInstructionFamily::FLAT ||
                            instruction.family == RdnaInstructionFamily::DS || instruction.family == RdnaInstructionFamily::MIMG;
        if (!scalarAlu && !vectorAlu && !memory && instruction.op != RdnaOpcode::Exp &&
            instruction.op != RdnaOpcode::SEndpgm && instruction.op != RdnaOpcode::SWaitcnt &&
            instruction.op != RdnaOpcode::SWaitcntDepctr && instruction.op != RdnaOpcode::SNop) fail();
        const auto writesExec = [](const RdnaOperand& operand) {
            return operand.kind == RdnaOperandKind::ExecLo || operand.kind == RdnaOperandKind::ExecHi;
        };
        if ((!live.empty() || !pending.empty()) && (writesExec(instruction.destination) || writesExec(instruction.destination2))) fail();
        const auto& access = accesses.at(index);
        if (access.writesExec) fail();
        std::set<std::uint32_t> partials;
        for (const auto& pair : pending) partials.insert(pair.first);
        for (const auto reg : access.reads) {
            if (live.contains(reg) || partials.contains(reg)) fail();
        }
        for (const auto reg : access.writes) {
            if (partials.contains(reg)) fail();
        }
        for (const auto& source : {instruction.source0, instruction.source1, instruction.source2, instruction.source3}) {
            if (source.dpp || source.dpp8) fail();
        }
        for (const auto reg : access.writes) {
            if (!live.contains(reg)) continue;
            if (!vectorAlu || access.writes.size() != 1u || instruction.destination.kind != RdnaOperandKind::VectorRegister ||
                instruction.destination.reg != reg || !plain(instruction.destination)) fail();
            live.erase(reg);
        }
    }
    if (!pending.empty()) fail();
}

void validateTranslateOptions(const TranslateOptions& options) {
    if (options.userDataBaseRegister >= NumScalarRegs || options.userDataCount > NumScalarRegs - options.userDataBaseRegister) {
        throw std::runtime_error("shader user data exceeds the scalar register bank");
    }
    if (options.waveSize != 32u && options.waveSize != 64u) {
        throw std::runtime_error("shader translation requires wave32 or wave64, got " + std::to_string(options.waveSize));
    }
    if (options.embeddedFetch != nullptr && options.stage != ShaderStageKind::Vertex && options.stage != ShaderStageKind::Local) {
        throw std::runtime_error("embedded vertex fetch requires a vertex or local shader");
    }
    switch (options.stage) {
    case ShaderStageKind::Vertex:
    case ShaderStageKind::Local:
    case ShaderStageKind::TessellationControl:
    case ShaderStageKind::TessellationEvaluation:
    case ShaderStageKind::Mesh:
        if (options.inputInfo.vertex == nullptr) {
            throw std::runtime_error("vertex shader translation has no vertex input metadata");
        }
        return;
    case ShaderStageKind::Pixel:
        if (options.inputInfo.pixel == nullptr) {
            throw std::runtime_error("pixel shader translation has no pixel input metadata");
        }
        return;
    case ShaderStageKind::Compute:
        if (options.inputInfo.compute == nullptr) {
            throw std::runtime_error("compute shader translation has no compute input metadata");
        }
        return;
    case ShaderStageKind::Unknown:
    case ShaderStageKind::Fetch:
        break;
    }
    throw std::runtime_error(
        "shader translation has an unsupported stage: options.stage=" +
        std::to_string(static_cast<int>(options.stage))
    );
}

const ShaderWorkgroupInputInfo* shaderWorkgroupInput(ShaderStageKind stage, const ShaderStageInputInfo& inputInfo) {
    switch (stage) {
    case ShaderStageKind::Compute:
        return inputInfo.compute;
    case ShaderStageKind::Mesh:
        return inputInfo.vertex != nullptr ? &inputInfo.vertex->mesh : nullptr;
    default:
        return nullptr;
    }
}

bool isCodeTableLoad(const ControlFlowGraph& cfg, std::uint32_t programCounter) {
    return std::find(cfg.codeTableLoadProgramCounters.begin(), cfg.codeTableLoadProgramCounters.end(), programCounter) != cfg.codeTableLoadProgramCounters.end();
}

const EmbeddedFetchLoad* findEmbeddedFetchLoad(const EmbeddedFetchPlan* plan, std::uint32_t programCounter) {
    if (plan == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(plan->loads.begin(), plan->loads.end(), [programCounter](const EmbeddedFetchLoad& load) {
        return load.programCounter == programCounter;
    });
    return found != plan->loads.end() ? &*found : nullptr;
}

int resolveEmbeddedFetchResource(const ShaderVertexInputInfo& input, const EmbeddedFetchLoad& load) {
    if (load.attributeId >= 0 && load.attributeId < input.resourcesNum && input.resourcesDst[load.attributeId].attrId == load.attributeId) {
        return load.attributeId;
    }
    for (int index = 0; index < input.resourcesNum; index++) {
        const auto& destination = input.resourcesDst[index];
        if (destination.attrId == load.attributeId && load.componentCount <= static_cast<std::uint32_t>(std::max(destination.registersNum, 1))) {
            return index;
        }
    }
    for (int index = 0; index < input.resourcesNum; index++) {
        if (input.resourcesDst[index].attrId == load.attributeId) {
            return index;
        }
    }
    return -1;
}

bool isBufferDwordLoad(RdnaOpcode opcode) {
    switch (opcode) {
    case RdnaOpcode::BufferLoadFormatX:
    case RdnaOpcode::BufferLoadFormatXy:
    case RdnaOpcode::BufferLoadFormatXyz:
    case RdnaOpcode::BufferLoadFormatXyzw:
    case RdnaOpcode::BufferLoadDword:
    case RdnaOpcode::BufferLoadDwordx2:
    case RdnaOpcode::BufferLoadDwordx3:
    case RdnaOpcode::BufferLoadDwordx4:
    case RdnaOpcode::TbufferLoadFormatX:
    case RdnaOpcode::TbufferLoadFormatXy:
    case RdnaOpcode::TbufferLoadFormatXyz:
    case RdnaOpcode::TbufferLoadFormatXyzw:
        return true;
    default:
        return false;
    }
}

void includeInstructionVectorRegisters(const RdnaInstruction& instruction, std::uint32_t& vectorLimit) {
    const auto includeVector = [&vectorLimit](const RdnaOperand& operand, std::uint32_t count = 1u) {
        if (operand.kind == RdnaOperandKind::VectorRegister) {
            vectorLimit = std::min(NumVectorRegs, std::max(vectorLimit, operand.reg + count));
        }
    };
    const bool memoryFamily = instruction.family == RdnaInstructionFamily::MUBUF || instruction.family == RdnaInstructionFamily::MTBUF || instruction.family == RdnaInstructionFamily::FLAT || instruction.family == RdnaInstructionFamily::DS || instruction.family == RdnaInstructionFamily::MIMG;
    includeVector(instruction.destination, memoryFamily ? std::max(instruction.dataDwordCount, 1u) : 1u);
    includeVector(instruction.destination2);
    includeVector(instruction.source0);
    includeVector(instruction.source1);
    includeVector(instruction.source2);
    includeVector(instruction.source3);
    if (instruction.family == RdnaInstructionFamily::FLAT) {
        const bool compare = instruction.op == RdnaOpcode::FlatAtomicCmpswap || instruction.op == RdnaOpcode::FlatAtomicCmpswapX2 || instruction.op == RdnaOpcode::FlatAtomicFcmpswap || instruction.op == RdnaOpcode::FlatAtomicFcmpswapX2;
        includeVector(instruction.source2, std::max(instruction.dataDwordCount, 1u) * (compare ? 2u : 1u));
    }
    if (instruction.family == RdnaInstructionFamily::DS) {
        switch (instruction.op) {
        case RdnaOpcode::DsWriteB64:
        case RdnaOpcode::DsWriteB96:
        case RdnaOpcode::DsWriteB128:
            includeVector(instruction.source1, instruction.dataDwordCount);
            break;
        case RdnaOpcode::DsWrite2B32:
        case RdnaOpcode::DsWrite2st64B32:
        case RdnaOpcode::DsWrite2B64:
        case RdnaOpcode::DsWrite2st64B64: {
            const std::uint32_t width = std::max(instruction.dataDwordCount / 2u, 1u);
            includeVector(instruction.source1, width);
            includeVector(instruction.source2, width);
            break;
        }
        default:
            break;
        }
    }
    for (std::uint32_t index = 0; index + 1u < instruction.imageAddressComponents && index < MaxRdnaImageNsaAddressComponents; index++) {
        vectorLimit = std::min(NumVectorRegs, std::max(vectorLimit, instruction.imageNsaVectorRegisters[index] + 1u));
    }
}

void emitEntryPrologue(IrProgram& program, IrBlock& entryBlock, const TranslateOptions& options) {
    IrBuilder entryIr(program);
    entryIr.SetInsertionPoint(entryBlock);

    const auto builtin = [&entryIr](StageInputKind kind, std::uint32_t component = 0u) -> IrValue& {
        return entryIr.Emit(IrOpcode::GetBuiltin, IrOpcodeType(IrOpcode::GetBuiltin), {&entryIr.Constant(static_cast<std::uint32_t>(kind)), &entryIr.Constant(component)});
    };

    for (std::uint32_t index = 0; index < options.userDataCount; index++) {
        const auto reg = static_cast<ScalarReg>(options.userDataBaseRegister + index);
        IrValue& value = entryIr.GetUserData(reg);
        entryIr.SetScalarReg(reg, value);
        entryIr.SetScalarMaskTag(reg, entryIr.ConstantBool(false));
    }

    IrValue* initialExec = &entryIr.ConstantBool(true);
    if (options.stage == ShaderStageKind::Pixel) {
        initialExec = &entryIr.IEqual(builtin(StageInputKind::HelperInvocation), entryIr.Constant(0u));
    }
    std::uint32_t totalThreads = 0;
    const auto* workgroup = shaderWorkgroupInput(options.stage, options.inputInfo);
    if (workgroup != nullptr) {
        totalThreads = std::max(workgroup->threadsNum[0], 1u) * std::max(workgroup->threadsNum[1], 1u) * std::max(workgroup->threadsNum[2], 1u);
        if (options.waveSize == 64u && workgroup->hostSubgroupSize == 32u && totalThreads % 64u != 0u) {
            initialExec = &entryIr.ULessThan(builtin(StageInputKind::LocalInvocationIndex), entryIr.Constant(totalThreads));
        }
    }
    if (options.stage == ShaderStageKind::Compute && options.inputInfo.compute->partialGroups) {
        for (std::uint32_t axis = 0; axis < 3u; axis++) {
            initialExec = &entryIr.LogicalAnd(*initialExec, entryIr.ULessThan(builtin(StageInputKind::GlobalInvocationId, axis), builtin(StageInputKind::DispatchThreadLimit, axis)));
        }
    }
    entryIr.SetExec(*initialExec);
    IrValue& initialMask = entryIr.Emit(IrOpcode::Ballot, IrOpcodeType(IrOpcode::Ballot), {initialExec});
    entryIr.SetExecLo(entryIr.CompositeExtract(initialMask, 0u));
    entryIr.SetExecHi(options.waveSize == 64u ? entryIr.CompositeExtract(initialMask, 1u) : entryIr.Constant(0u));

    if (options.stage == ShaderStageKind::Compute) {
        const auto* cs = options.inputInfo.compute;
        const std::uint32_t threadIds = cs->threadIdsNum > 0 ? std::min<std::uint32_t>(static_cast<std::uint32_t>(cs->threadIdsNum), 3u) : 0u;
        for (std::uint32_t index = 0; index < threadIds; index++) {
            entryIr.SetVectorReg(static_cast<VectorReg>(index), builtin(StageInputKind::LocalInvocationId, index));
        }
        std::uint32_t regOffset = 0;
        for (std::uint32_t index = 0; index < 3u; index++) {
            if (cs->groupId[index]) {
                entryIr.SetScalarReg(static_cast<ScalarReg>(cs->workgroupRegister + regOffset++), builtin(StageInputKind::WorkgroupId, index));
            }
        }
        if (cs->tgSizeEn) {
            const std::uint32_t waveSize = cs->waveSize != 0u ? cs->waveSize : 64u;
            const std::uint32_t waves = std::min((totalThreads + waveSize - 1u) / waveSize, 0x3fu);
            IrValue& localIndex = builtin(StageInputKind::LocalInvocationIndex);
            IrValue& waveId = entryIr.Emit(IrOpcode::UDiv32, IrOpcodeType(IrOpcode::UDiv32), {&localIndex, &entryIr.Constant(waveSize)});
            IrValue& waveBits = entryIr.ShiftLeftLogical(waveId, entryIr.Constant(20u));
            IrValue& firstBit = entryIr.Select(entryIr.IEqual(waveId, entryIr.Constant(0u)), entryIr.Constant(0x80000000u), entryIr.Constant(0u));
            entryIr.SetScalarReg(static_cast<ScalarReg>(cs->workgroupRegister + regOffset), entryIr.BitwiseOr(entryIr.BitwiseOr(waveBits, entryIr.Constant(waves)), firstBit));
        }
    } else if (options.stage == ShaderStageKind::Mesh) {
        const auto& mesh = options.inputInfo.vertex->mesh;
        const std::uint32_t size = mesh.InputPrimitiveSize();
        const std::uint32_t stepCount = mesh.InputPrimitiveStep();
        if (options.waveSize != 64u || mesh.primitivesPerGroup == 0u || mesh.verticesPerGroup != mesh.InputVertexCount(mesh.primitivesPerGroup) || mesh.verticesPerGroup > totalThreads || mesh.primitivesPerGroup > totalThreads || totalThreads % 64u != 0u || totalThreads > 15u * 64u || mesh.esgsItemSize == 0u || mesh.esgsItemSize * mesh.verticesPerGroup > 0xffffu) {
            throw std::runtime_error("mesh shader translation configuration is not supported (wave " + std::to_string(options.waveSize) + ", primitives per group " + std::to_string(mesh.primitivesPerGroup) + ", vertices per group " + std::to_string(mesh.verticesPerGroup) + ", threads " + std::to_string(totalThreads) + ", ESGS item size " + std::to_string(mesh.esgsItemSize) + ")");
        }
        constexpr std::uint32_t kTriFanPrimitiveType = 5u;
        constexpr std::uint32_t kTriStripPrimitiveType = 6u;
        const bool fan = mesh.inputPrimitive == kTriFanPrimitiveType;
        const auto u32 = [&entryIr](std::uint32_t value) -> IrValue& {
            return entryIr.Constant(value);
        };
        const auto draw = [&entryIr](std::uint32_t index) -> IrValue& {
            return entryIr.Emit(IrOpcode::MeshDrawParameter, IrOpcodeType(IrOpcode::MeshDrawParameter), {&entryIr.Constant(index)});
        };
        const auto argument = [&entryIr](std::uint32_t index) -> IrValue& {
            return entryIr.Emit(IrOpcode::MeshArgument, IrOpcodeType(IrOpcode::MeshArgument), {&entryIr.Constant(index)});
        };
        const auto minimum = [&entryIr](IrValue& lhs, IrValue& rhs) -> IrValue& {
            return entryIr.Emit(IrOpcode::UMin32, IrOpcodeType(IrOpcode::UMin32), {&lhs, &rhs});
        };
        const auto subtractSaturate = [&entryIr, &minimum](IrValue& lhs, IrValue& rhs) -> IrValue& {
            return entryIr.ISub(lhs, minimum(lhs, rhs));
        };
        IrValue& local = builtin(StageInputKind::LocalInvocationIndex);
        IrValue& firstPrimitive = entryIr.IMul(builtin(StageInputKind::WorkgroupId, 0u), u32(mesh.primitivesPerGroup));
        IrValue& step = u32(stepCount);
        IrValue& firstVertex = entryIr.IMul(firstPrimitive, step);
        IrValue& indirect = entryIr.INotEqual(entryIr.BitwiseOr(draw(MeshArgumentAddressDword), draw(MeshArgumentAddressDword + 1u)), u32(0u));
        IrValue& indexCount = entryIr.Select(indirect, argument(MeshArgumentIndexCountDword), draw(0u));
        IrValue& firstIndex = argument(MeshArgumentFirstIndexDword);
        IrValue& vertices = minimum(subtractSaturate(indexCount, firstVertex), u32(mesh.verticesPerGroup));
        IrValue& primitives = entryIr.Select(entryIr.ULessThan(vertices, u32(size)), u32(0u), entryIr.IAdd(entryIr.Emit(IrOpcode::UDiv32, IrOpcodeType(IrOpcode::UDiv32), {&subtractSaturate(vertices, u32(size)), &step}), u32(1u)));
        entryIr.SetScalarReg(static_cast<ScalarReg>(2), entryIr.BitwiseOr(entryIr.ShiftLeftLogical(vertices, u32(12u)), entryIr.ShiftLeftLogical(primitives, u32(22u))));
        IrValue& wave = entryIr.ShiftRightLogical(local, u32(6u));
        IrValue& waveBase = entryIr.BitwiseAnd(local, u32(~63u));
        IrValue& vertexCount = minimum(subtractSaturate(vertices, waveBase), u32(64u));
        IrValue& primitiveCount = minimum(subtractSaturate(primitives, waveBase), u32(64u));
        IrValue& waveInfo = entryIr.BitwiseOr(entryIr.ShiftLeftLogical(wave, u32(24u)), u32((totalThreads / 64u) << 28u));
        entryIr.SetScalarReg(static_cast<ScalarReg>(3), entryIr.BitwiseOr(waveInfo, entryIr.BitwiseOr(entryIr.ShiftLeftLogical(primitiveCount, u32(8u)), vertexCount)));
        IrValue& parity = mesh.inputPrimitive == kTriStripPrimitiveType ? entryIr.BitwiseAnd(entryIr.IAdd(firstPrimitive, local), u32(1u)) : u32(0u);
        IrValue& vertex = entryIr.IMul(local, step);
        IrValue& item = u32(mesh.esgsItemSize);
        IrValue& first = fan ? entryIr.IMul(entryIr.IAdd(vertex, u32(1u)), item) : entryIr.IMul(entryIr.IAdd(vertex, parity), item);
        IrValue& second = fan ? entryIr.IMul(entryIr.IAdd(vertex, u32(2u)), item) : size >= 2u ? entryIr.IMul(entryIr.ISub(entryIr.IAdd(vertex, u32(1u)), parity), item) : u32(0u);
        IrValue& third = fan ? u32(0u) : size == 3u ? entryIr.IMul(entryIr.IAdd(vertex, u32(2u)), item) : u32(0u);
        entryIr.SetVectorReg(static_cast<VectorReg>(0), entryIr.BitwiseOr(entryIr.BitwiseAnd(first, u32(0xffffu)), entryIr.ShiftLeftLogical(second, u32(16u))));
        entryIr.SetVectorReg(static_cast<VectorReg>(1), entryIr.BitwiseAnd(third, u32(0xffffu)));
        entryIr.SetVectorReg(static_cast<VectorReg>(2), entryIr.IAdd(firstPrimitive, local));
        entryIr.SetVectorReg(static_cast<VectorReg>(3), u32(0u));
        entryIr.SetVectorReg(static_cast<VectorReg>(4), u32(0u));
        if (options.userDataBaseRegister != 0u || options.userDataCount < 8u) {
            throw std::runtime_error("mesh shader translation requires the merged program's eight hidden user words");
        }
        IrValue& inputVertex = fan ? entryIr.Select(entryIr.IEqual(local, u32(0u)), u32(0u), entryIr.IAdd(firstVertex, local)) : entryIr.IAdd(firstVertex, local);
        IrValue& indexBytes = draw(3u);
        IrValue& indexed = entryIr.INotEqual(indexBytes, u32(0u));
        IrValue& byteOffset = entryIr.IMul(entryIr.IAdd(inputVertex, firstIndex), indexBytes);
        IrValue& indexResource = entryIr.Emit(IrOpcode::GetBufferResource, IrOpcodeType(IrOpcode::GetBufferResource), {&entryIr.GetUserData(static_cast<ScalarReg>(4)), &entryIr.GetUserData(static_cast<ScalarReg>(5)), &entryIr.GetUserData(static_cast<ScalarReg>(6)), &entryIr.GetUserData(static_cast<ScalarReg>(7))});
        const std::uint32_t memoryIndex = static_cast<std::uint32_t>(program.Resources().memoryInfo.size());
        program.Resources().memoryInfo.push_back(MemoryInfo{.kind = ResourceKind::Buffer, .resource = 1u, .offen = true});
        IrValue& packedIndex = entryIr.Emit(IrOpcode::LoadBufferU32, IrOpcodeType(IrOpcode::LoadBufferU32), {&indexResource, &u32(0u), &entryIr.BitwiseAnd(byteOffset, u32(~3u)), &u32(0u), &entryIr.LogicalAnd(indexed, entryIr.ULessThan(local, vertices))}, MemoryFlags{.index = memoryIndex});
        IrValue& index = entryIr.Emit(IrOpcode::BitFieldUExtract, IrOpcodeType(IrOpcode::BitFieldUExtract), {&packedIndex, &entryIr.IMul(entryIr.BitwiseAnd(byteOffset, u32(3u)), u32(8u)), &entryIr.IMul(indexBytes, u32(8u))});
        for (std::uint32_t reg = 4u; reg < 8u; reg++) {
            entryIr.SetScalarReg(static_cast<ScalarReg>(reg), u32(0u));
        }
        entryIr.SetVectorReg(static_cast<VectorReg>(5), entryIr.IAdd(draw(1u), entryIr.Select(indexed, index, inputVertex)));
        entryIr.SetVectorReg(static_cast<VectorReg>(6), u32(0u));
        entryIr.SetVectorReg(static_cast<VectorReg>(7), u32(0u));
        entryIr.SetVectorReg(static_cast<VectorReg>(8), entryIr.IAdd(draw(2u), builtin(StageInputKind::WorkgroupId, 1u)));
    } else if (options.stage == ShaderStageKind::Local) {
        entryIr.SetScalarReg(static_cast<ScalarReg>(3), entryIr.Constant(64u));
        entryIr.SetVectorReg(static_cast<VectorReg>(2), builtin(StageInputKind::VertexIndex));
        entryIr.SetVectorReg(static_cast<VectorReg>(3), entryIr.Constant(0u));
        entryIr.SetVectorReg(static_cast<VectorReg>(5), builtin(StageInputKind::InstanceIndex));
    } else if (options.stage == ShaderStageKind::TessellationControl) {
        const auto& tess = options.inputInfo.vertex->tess;
        entryIr.SetScalarReg(static_cast<ScalarReg>(2), entryIr.Emit(IrOpcode::TessellationBase, IrOpcodeType(IrOpcode::TessellationBase), {&entryIr.Constant(0u)}));
        entryIr.SetScalarReg(static_cast<ScalarReg>(4), entryIr.Emit(IrOpcode::TessellationBase, IrOpcodeType(IrOpcode::TessellationBase), {&entryIr.Constant(1u)}));
        entryIr.SetScalarReg(static_cast<ScalarReg>(3), entryIr.Constant(0x81010000u | tess.inputControlPoints | (tess.outputControlPoints << 8u)));
        entryIr.SetVectorReg(static_cast<VectorReg>(0), builtin(StageInputKind::PrimitiveId));
        entryIr.SetVectorReg(static_cast<VectorReg>(1), entryIr.ShiftLeftLogical(builtin(StageInputKind::InvocationId), entryIr.Constant(8u)));
    } else if (options.stage == ShaderStageKind::TessellationEvaluation) {
        entryIr.SetScalarReg(static_cast<ScalarReg>(3), entryIr.Constant(64u));
        entryIr.SetScalarReg(static_cast<ScalarReg>(4), entryIr.Emit(IrOpcode::TessellationBase, IrOpcodeType(IrOpcode::TessellationBase), {&entryIr.Constant(0u)}));
        entryIr.SetVectorReg(static_cast<VectorReg>(5), builtin(StageInputKind::TessCoord, 0u));
        entryIr.SetVectorReg(static_cast<VectorReg>(6), builtin(StageInputKind::TessCoord, 1u));
        entryIr.SetVectorReg(static_cast<VectorReg>(7), entryIr.Constant(0u));
        entryIr.SetVectorReg(static_cast<VectorReg>(8), builtin(StageInputKind::PrimitiveId));
    } else if (options.stage == ShaderStageKind::Pixel) {
        const auto* ps = options.inputInfo.pixel;
        const auto vgpr = [&](PixelInput input) { return ps->psInputVgpr[static_cast<std::uint32_t>(input)]; };
        const auto loaded = [&](PixelInput input) { return vgpr(input) != ShaderPixelInputInfo::NoPixelInputVgpr; };
        if (options.fragmentShaderBarycentricEnabled) {
            for (const auto [input, kind] : {std::pair{PixelInput::PerspectiveCenter, StageInputKind::BaryCoordSmooth}, std::pair{PixelInput::PerspectiveCentroid, StageInputKind::BaryCoordSmooth},
                                             std::pair{PixelInput::LinearCenter, StageInputKind::BaryCoordNoPerspective}, std::pair{PixelInput::LinearCentroid, StageInputKind::BaryCoordNoPerspective}}) {
                if (!loaded(input)) continue;
                entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(input)), builtin(kind, 0u));
                entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(input) + 1u), builtin(kind, 1u));
            }
        }
        if (loaded(PixelInput::PositionX)) {
            entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(PixelInput::PositionX)), builtin(StageInputKind::FragCoord, 0u));
        }
        if (loaded(PixelInput::PositionY)) {
            entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(PixelInput::PositionY)), builtin(StageInputKind::FragCoord, 1u));
        }
        if (loaded(PixelInput::PositionZ)) {
            entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(PixelInput::PositionZ)), builtin(StageInputKind::FragCoord, 2u));
        }
        if (loaded(PixelInput::PositionW)) {
            IrValue& reciprocalW = entryIr.BitCastF32(builtin(StageInputKind::FragCoord, 3u));
            IrValue& w = entryIr.Emit(IrOpcode::FPRecip32, IrOpcodeType(IrOpcode::FPRecip32), {&reciprocalW});
            entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(PixelInput::PositionW)), entryIr.BitCastU32(w));
        }
        if (loaded(PixelInput::FrontFace)) {
            entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(PixelInput::FrontFace)), builtin(StageInputKind::FrontFacing));
        }
        if (loaded(PixelInput::Ancillary)) {
            entryIr.SetVectorReg(static_cast<VectorReg>(vgpr(PixelInput::Ancillary)), builtin(StageInputKind::PackedAncillary));
        }
    } else if (options.stage == ShaderStageKind::Vertex) {
        entryIr.SetVectorReg(static_cast<VectorReg>(5), builtin(StageInputKind::VertexIndex));
        entryIr.SetVectorReg(static_cast<VectorReg>(8), builtin(StageInputKind::InstanceIndex));
    }
}

}

IrProgram InstructionTranslator::Translate(const RdnaProgram& decoded, const ControlFlowGraph& cfg, const TranslateOptions& options) const {
    validateTranslateOptions(options);
    if (cfg.blocks.empty()) {
        throw std::runtime_error("cannot translate an empty control flow graph");
    }

    std::uint32_t vectorLimit = 1u;
    for (const auto& cfgBlock : cfg.blocks) {
        for (std::uint32_t index = cfgBlock.instructionBegin; index < cfgBlock.instructionEnd; index++) {
            if (index >= decoded.instructions.size()) {
                throw std::runtime_error("control flow graph block " + std::to_string(cfgBlock.id) + " references instruction " + std::to_string(index) + " outside decoded program of size " + std::to_string(decoded.instructions.size()));
            }
            const auto& instruction = decoded.instructions[index];
            if (isCodeTableLoad(cfg, instruction.programCounter)) {
                continue;
            }
            includeInstructionVectorRegisters(instruction, vectorLimit);
        }
    }

    const bool fixedFunctionInterpolation = options.stage == ShaderStageKind::Pixel && !options.fragmentShaderBarycentricEnabled;
    std::vector<InstructionRegisterAccess> interpolationAccesses(fixedFunctionInterpolation ? decoded.instructions.size() : 0u);

    IrProgram program;
    program.SetWaveSize(options.waveSize);
    program.Resources().stage = toIrShaderStage(options.stage);
    program.Resources().shaderHash = options.shaderHash;
    program.Resources().userDataBase = options.userDataBaseRegister;
    program.Resources().userDataCount = options.userDataCount;
    program.Info().scratchDwords = options.scratchDwords;
    if (options.embeddedFetch != nullptr) {
        program.Info().vertexOffsetSgpr = options.embeddedFetch->vertexOffsetSgpr;
        program.Info().instanceOffsetSgpr = options.embeddedFetch->instanceOffsetSgpr;
        program.Info().vertexOffsetShared = options.embeddedFetch->vertexOffsetShared;
        program.Info().instanceOffsetShared = options.embeddedFetch->instanceOffsetShared;
        program.Info().vertexOffsetConflict = options.embeddedFetch->vertexOffsetConflict;
        program.Info().instanceOffsetConflict = options.embeddedFetch->instanceOffsetConflict;
    }
    program.Metadata().cfgFailureKind = cfg.failureKind;
    program.Metadata().failureReason = cfg.unsupportedReason;

    const auto maxIdIterator = std::max_element(cfg.blocks.begin(), cfg.blocks.end(), [](const BasicBlock& lhs, const BasicBlock& rhs) {
        return lhs.id < rhs.id;
    });
    if (maxIdIterator == cfg.blocks.end() || maxIdIterator->id == InvalidControlFlowId) {
        throw std::runtime_error("cannot allocate a synthetic entry block id");
    }
    const std::uint32_t entryBlockId = maxIdIterator->id + 1u;

    std::vector<IrBlock*> blocks;
    std::vector<BlockInfo> blockInfos;
    blocks.reserve(cfg.blocks.size() + 1u);
    blockInfos.reserve(cfg.blocks.size() + 1u);

    Terminator entryTerminator;
    entryTerminator.kind = TerminatorKind::Branch;
    entryTerminator.trueBlock = cfg.blocks.front().id;
    blocks.push_back(&program.CreateBlock());
    blockInfos.push_back(BlockInfo{entryBlockId, cfg.blocks.front().startProgramCounter, cfg.blocks.front().startProgramCounter, entryTerminator});

    std::unordered_map<std::uint32_t, std::size_t> blockIndices;
    blockIndices.reserve(cfg.blocks.size());
    for (const auto& sourceBlock : cfg.blocks) {
        if (!blockIndices.emplace(sourceBlock.id, blocks.size()).second) {
            throw std::runtime_error("control flow graph contains duplicate block id " + std::to_string(sourceBlock.id));
        }
        blocks.push_back(&program.CreateBlock());
        blockInfos.push_back(BlockInfo{sourceBlock.id, sourceBlock.startProgramCounter, sourceBlock.endProgramCounter, sourceBlock.terminator});
    }

    for (const auto& sourceBlock : cfg.blocks) {
        const auto sourceIndex = blockIndices.at(sourceBlock.id);
        for (const auto successor : sourceBlock.successors) {
            const auto target = blockIndices.find(successor);
            if (target == blockIndices.end()) {
                throw std::runtime_error("control flow graph block " + std::to_string(sourceBlock.id) + " has unknown successor " + std::to_string(successor));
            }
            blocks[sourceIndex]->AddBranch(blocks[target->second]);
        }
    }
    blocks.front()->AddBranch(blocks[blockIndices.at(cfg.blocks.front().id)]);

    emitEntryPrologue(program, *blocks.front(), options);

    for (const auto& cfgBlock : cfg.blocks) {
        const auto typedIndex = blockIndices.at(cfgBlock.id);
        TranslationContext context(program, *blocks[typedIndex], vectorLimit);
        context.SetPixelInput(options.inputInfo.pixel, options.fragmentShaderBarycentricEnabled);
        for (std::uint32_t index = cfgBlock.instructionBegin; index < cfgBlock.instructionEnd; index++) {
            const auto& instruction = decoded.instructions[index];
            if (isCodeTableLoad(cfg, instruction.programCounter)) {
                const auto table = std::find_if(cfg.codeTableLoads.begin(), cfg.codeTableLoads.end(), [&](const auto& entry) { return entry.programCounter == instruction.programCounter; });
                if (table == cfg.codeTableLoads.end()) throw std::runtime_error("missing shader code table values");
                context.TranslateCodeTableLoad(instruction, *table);
                continue;
            }
            const auto* embedded = findEmbeddedFetchLoad(options.embeddedFetch, instruction.programCounter);
            if (embedded != nullptr && isBufferDwordLoad(instruction.op) && instruction.dataDwordCount == embedded->componentCount && instruction.destination.kind == RdnaOperandKind::VectorRegister) {
                if (options.inputInfo.vertex == nullptr) {
                    throw std::runtime_error("embedded vertex fetch requires vertex input metadata");
                }
                const auto resource = resolveEmbeddedFetchResource(*options.inputInfo.vertex, *embedded);
                if (resource < 0 || resource >= options.inputInfo.vertex->resourcesNum) {
                    throw std::runtime_error("embedded vertex fetch at program counter " + std::to_string(instruction.programCounter) + " has no resource for attribute " + std::to_string(embedded->attributeId));
                }
                context.TranslateEmbeddedFetch(instruction, static_cast<std::uint32_t>(resource), embedded->componentCount, options.inputInfo.vertex->resources[resource]);
                continue;
            }
            const auto& instructions = blocks[typedIndex]->Instructions();
            const auto previous = instructions.empty() ? instructions.end() : std::prev(instructions.end());
            context.TranslateInstruction(instruction);
            if (fixedFunctionInterpolation) interpolationAccesses[index] = instructionRegisterAccess(*blocks[typedIndex], previous == instructions.end() ? instructions.begin() : std::next(previous));
        }
        context.AddBranchCondition(cfgBlock, blockInfos[typedIndex]);
    }

    if (fixedFunctionInterpolation) validateFixedFunctionInterpolation(decoded, cfg, *options.inputInfo.pixel, interpolationAccesses);

    program.Metadata().blockInfo = std::move(blockInfos);
    program.BlockOrder() = blocks;

    ValidateProgram(program, false);
    return program;
}

void InstructionTranslator::translateInstruction(IrBuilder& builder, const RdnaInstruction& instruction, const ControlFlowGraph& cfg, const TranslateOptions& options) const {
    DispatchInstruction(builder, instruction, cfg, options);
}

TranslationContext::TranslationContext(IrProgram& program, IrBlock& block, std::uint32_t vectorLimit) : program(program), ir(program), block(block), instructionBranchCondition(IrU1(program.CreateValue(IrOpcode::Void, IrType::Bool))), currentVectorLimit(vectorLimit) {
    ir.SetInsertionPoint(block);
}

}
