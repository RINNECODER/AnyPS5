#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/ConstantFolder.hpp"
#include "Optimization/DeadCodeEliminator.hpp"
#include "SpirvBackend/SpirvAnalysis.hpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace ShaderRecompiler;

namespace {

struct Case {
    const char* name;
    std::uint32_t source;
    std::uint32_t bound;
    std::uint32_t mask;
    bool folded;
    std::uint32_t expected;
    bool boundFirst = false;
    bool maskFirst = false;
    bool unbounded = false;
    bool dynamicBound = false;
    bool dynamicSource = false;
    bool retainDs = false;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint32_t evaluate(const IrValue& input, std::uint32_t lane) {
    const auto& value = *input.Resolve();
    if (value.HasImmediate()) {
        return value.ImmediateU32();
    }
    if (value.Opcode() == IrOpcode::LaneId) {
        return lane;
    }
    const auto left = evaluate(*value.Argument(0), lane);
    const auto right = evaluate(*value.Argument(1), lane);
    if (value.Opcode() == IrOpcode::BitwiseAnd32) {
        return left & right;
    }
    require(value.Opcode() == IrOpcode::ShiftRightLogical32 && right < 32u, "unexpected or undefined operation in output evaluation");
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(left) / (std::uint64_t{1} << right));
}

std::uint32_t runCase(const Case& test) {
    IrProgram program;
    program.Resources().stage = IrShaderStage::Compute;
    program.SetWaveSize(64u);
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& bound = test.dynamicBound ? lane : builder.Constant(test.bound);
    auto& limited = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, test.boundFirst ? std::initializer_list<IrValue*>{&bound, &lane} : std::initializer_list<IrValue*>{&lane, &bound});
    auto& source = test.dynamicSource ? lane : builder.Constant(test.source);
    auto& shifted = builder.Emit(IrOpcode::ShiftRightLogical32, IrType::U32, {&source, test.unbounded ? &lane : &limited});
    auto& mask = builder.Constant(test.mask);
    auto& output = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, test.maskFirst ? std::initializer_list<IrValue*>{&mask, &shifted} : std::initializer_list<IrValue*>{&shifted, &mask});
    auto& sink = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&output});
    IrValue* ds = nullptr;
    if (test.retainDs) {
        auto& zero = builder.Constant(0u);
        auto& active = builder.ConstantBool(true);
        ds = &builder.Emit(IrOpcode::BpermuteU32, IrType::U32, {&lane, &zero, &active});
        static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {ds}));
    }
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    require(AnalyzeProgramRequirements(program).subgroupLocalInvocationId, std::string(test.name) + ": pre-fold lane requirement missing");
    ConstantFolder{}.Fold(program);
    DeadCodeEliminator{}.RemoveIdentities(program);
    DeadCodeEliminator{}.Eliminate(program);
    const auto& actual = *sink.Argument(0)->Resolve();
    require(actual.HasImmediate() == test.folded, std::string(test.name) + ": constant result differs from contract");
    if (test.folded) {
        require(actual.Type() == IrType::U32 && actual.ImmediateU32() == test.expected, std::string(test.name) + ": wrong constant output");
    }
    std::uint32_t outputChecks = 0;
    std::uint32_t firstOutput = 0;
    bool varies = false;
    for (std::uint32_t laneIndex = 0; laneIndex < 64u; ++laneIndex) {
        const auto shift = test.unbounded ? laneIndex : laneIndex & (test.dynamicBound ? laneIndex : test.bound);
        if (shift >= 32u) {
            continue;
        }
        const auto input = test.dynamicSource ? laneIndex : test.source;
        const auto expected = static_cast<std::uint32_t>(static_cast<std::uint64_t>(input) / (std::uint64_t{1} << shift)) & test.mask;
        const auto actualOutput = evaluate(actual, laneIndex);
        require(actualOutput == expected, std::string(test.name) + ": observable output changed for lane " + std::to_string(laneIndex));
        if (outputChecks == 0u) {
            firstOutput = actualOutput;
        } else {
            varies = varies || actualOutput != firstOutput;
        }
        ++outputChecks;
    }
    require(test.folded || varies, std::string(test.name) + ": negative fixture has no observable defined output variation");
    const auto requirements = AnalyzeProgramRequirements(program);
    require(requirements.subgroupLocalInvocationId == (!test.folded || test.retainDs), std::string(test.name) + ": incorrect surviving lane requirement");
    bool liveLane = false;
    for (const auto* instruction : block.Instructions()) {
        liveLane = liveLane || instruction->Opcode() == IrOpcode::LaneId;
    }
    require(liveLane == (!test.folded || test.retainDs), std::string(test.name) + ": incorrect DCE of lane dependency");
    require(program.WaveSize() == 64u, std::string(test.name) + ": Wave64 altered");
    require(requirements.subgroupBallot == test.retainDs && requirements.subgroupShuffle == test.retainDs, std::string(test.name) + ": DS subgroup requirements altered");
    if (ds != nullptr) {
        require(ds->Parent() == &block && ds->Opcode() == IrOpcode::BpermuteU32, std::string(test.name) + ": live DS operation changed");
    }
    return outputChecks;
}

} // namespace

int main() {
    const std::array cases{
        Case{"all-ones-low-bit", 0xffffffffu, 31u, 1u, true, 1u},
        Case{"bound-on-left", 0xffffffffu, 31u, 1u, true, 1u, true},
        Case{"mask-on-left", 0xffffffffu, 31u, 1u, true, 1u, false, true},
        Case{"both-commuted", 0xffffffffu, 31u, 1u, true, 1u, true, true},
        Case{"low-byte-bounded", 0xffu, 7u, 1u, true, 1u},
        Case{"two-bits-through-30", 0xffffffffu, 30u, 3u, true, 3u},
        Case{"zero-source", 0u, 31u, 0xfu, true, 0u},
        Case{"zero-mask", 0x89abcdefu, 31u, 0u, true, 0u},
        Case{"high-bit-invariant-zero", 0xffffu, 15u, 0x80000000u, true, 0u},
        Case{"dependent-low-bit", 0x5u, 3u, 1u, false, 0u},
        Case{"interior-dependence", 0x9u, 3u, 1u, false, 0u},
        Case{"bit31-boundary", 0xffffffffu, 31u, 3u, false, 0u},
        Case{"partially-defined-varying-selector", 0x5u, 33u, 1u, false, 0u},
        Case{"unbounded-varying-selector", 0x5u, 31u, 1u, false, 0u, false, false, true},
        Case{"dynamic-varying-bound", 0x5u, 31u, 1u, false, 0u, false, false, false, true},
        Case{"dynamic-source", 0xffffffffu, 31u, 1u, false, 0u, false, false, false, false, true},
        Case{"independent-live-ds", 0xffffffffu, 31u, 1u, true, 1u, false, false, false, false, false, true},
    };
    int failures = 0;
    std::uint32_t outputChecks = 0;
    for (const auto& test : cases) {
        try {
            outputChecks += runCase(test);
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << error.what() << '\n';
        }
    }
    std::cout << "cases=" << cases.size() << " failures=" << failures << '\n';
    std::cout << "output_checks=" << outputChecks << '\n';
    return failures != 0;
}
