#include "IntermediateRepresentation/IrBuilder.hpp"
#include "Optimization/ConstantFolder.hpp"
#include <cstdint>
#include <cstdio>
#include <exception>

using namespace ShaderRecompiler;

namespace {

bool foldsTo(std::uint32_t word, bool expected, bool maskedShift) {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& shift = maskedShift ? builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&lane, &builder.Constant(31u)}) : lane;
    auto& shifted = builder.Emit(IrOpcode::ShiftRightLogical32, IrType::U32, {&builder.Constant(word), &shift});
    auto& bit = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&shifted, &builder.Constant(1u)});
    auto& test = builder.Emit(IrOpcode::INotEqual32, IrType::Bool, {&bit, &builder.Constant(0u)});
    auto& user = builder.Emit(IrOpcode::Reference, IrType::Void, {&test});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* result = user.Argument(0)->Resolve();
    if (!maskedShift || (word != 0u && word != 0xffffffffu)) return result->Opcode() == IrOpcode::INotEqual32;
    return result->HasImmediate() && result->ImmediateBool() == expected;
}

bool masksFold(std::uint32_t inner, std::uint32_t outer) {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& sum = builder.Emit(IrOpcode::IAdd32, IrType::U32, {&builder.Constant(1u), &lane});
    auto& first = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&sum, &builder.Constant(inner)});
    auto& second = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&first, &builder.Constant(outer)});
    static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&first}));
    auto& user = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&second});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* result = user.Argument(0)->Resolve();
    if ((inner & ~outer) == 0u) return result == &first;
    return result->Opcode() == IrOpcode::BitwiseAnd32 && result->Argument(0)->Resolve() == &sum && result->Argument(1)->Resolve()->ImmediateU32() == outer;
}

bool mergedHalfMask() {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& sum = builder.Emit(IrOpcode::IAdd32, IrType::U32, {&builder.Constant(1u), &lane});
    auto& low = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&sum, &builder.Constant(0xffffu)});
    auto& high = builder.Emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&lane, &builder.Constant(16u)});
    auto& merged = builder.Emit(IrOpcode::BitwiseOr32, IrType::U32, {&low, &high});
    auto& masked = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&merged, &builder.Constant(0x7fffu)});
    static_cast<void>(builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&low}));
    auto& user = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&masked});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* result = user.Argument(0)->Resolve();
    return result->Opcode() == IrOpcode::BitwiseAnd32 && result->Argument(0)->Resolve() == &sum;
}

bool halfExtractions() {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& low = builder.Emit(IrOpcode::IAdd32, IrType::U32, {&builder.Constant(3u), &lane});
    auto& high = builder.Emit(IrOpcode::IAdd32, IrType::U32, {&builder.Constant(5u), &lane});
    auto& lowHalf = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&low, &builder.Constant(0xffffu)});
    auto& highHalf = builder.Emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&high, &builder.Constant(16u)});
    auto& merged = builder.Emit(IrOpcode::BitwiseOr32, IrType::U32, {&lowHalf, &highHalf});
    auto& top = builder.Emit(IrOpcode::ShiftRightLogical32, IrType::U32, {&merged, &builder.Constant(16u)});
    auto& field = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&merged, &builder.Constant(16u), &builder.Constant(16u)});
    auto& bottom = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&merged, &builder.Constant(0u), &builder.Constant(16u)});
    auto& topUser = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&top});
    auto& fieldUser = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&field});
    auto& bottomUser = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&bottom});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* topResult = topUser.Argument(0)->Resolve();
    const IrValue* fieldResult = fieldUser.Argument(0)->Resolve();
    const IrValue* bottomResult = bottomUser.Argument(0)->Resolve();
    const bool topMasked = topResult->Opcode() == IrOpcode::BitwiseAnd32 && topResult->Argument(0)->Resolve() == &high && topResult->Argument(1)->Resolve()->ImmediateU32() == 0xffffu;
    const bool fieldThrough = fieldResult->Opcode() == IrOpcode::BitFieldUExtract && fieldResult->Argument(0)->Resolve() == &high && fieldResult->Argument(1)->Resolve()->ImmediateU32() == 0u;
    const bool bottomThrough = bottomResult->Opcode() == IrOpcode::BitFieldUExtract && bottomResult->Argument(0)->Resolve() == &low;
    return topMasked && fieldThrough && bottomThrough;
}

bool halvesRejoin() {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& word = builder.Emit(IrOpcode::IAdd32, IrType::U32, {&builder.Constant(7u), &lane});
    auto& low = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&word, &builder.Constant(0u), &builder.Constant(16u)});
    auto& high = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&word, &builder.Constant(16u), &builder.Constant(16u)});
    auto& raised = builder.Emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&high, &builder.Constant(16u)});
    auto& joined = builder.Emit(IrOpcode::BitwiseOr32, IrType::U32, {&low, &raised});
    auto& shifted = builder.Emit(IrOpcode::ShiftLeftLogical32, IrType::U32, {&word, &builder.Constant(16u)});
    auto& back = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&shifted, &builder.Constant(16u), &builder.Constant(16u)});
    auto& joinedUser = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&joined});
    auto& backUser = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&back});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* rejoined = joinedUser.Argument(0)->Resolve();
    const IrValue* field = backUser.Argument(0)->Resolve();
    const bool fieldBack = field->Opcode() == IrOpcode::BitFieldUExtract && field->Argument(0)->Resolve() == &word && field->Argument(1)->Resolve()->ImmediateU32() == 0u;
    return rejoined == &word && fieldBack;
}

bool factorFieldKept(bool multiplied) {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& lane = builder.Emit(IrOpcode::LaneId, IrType::U32, {});
    auto& sum = builder.Emit(IrOpcode::IAdd32, IrType::U32, {&builder.Constant(1u), &lane});
    auto& masked = builder.Emit(IrOpcode::BitwiseAnd32, IrType::U32, {&sum, &builder.Constant(0xffffu)});
    auto& field = builder.Emit(IrOpcode::BitFieldUExtract, IrType::U32, {&masked, &builder.Constant(0u), &builder.Constant(24u)});
    auto& used = multiplied ? builder.IMul(field, field) : field;
    auto& user = builder.Emit(IrOpcode::ReferenceU32, IrType::Void, {&used});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* result = user.Argument(0)->Resolve();
    if (!multiplied) return result == &masked;
    return result->Opcode() == IrOpcode::IMul32 && result->Argument(0)->Resolve() == &field && field.Opcode() == IrOpcode::BitFieldUExtract && field.Argument(0)->Resolve() == &masked;
}

bool reversesTo(std::uint32_t word, std::uint32_t expected) {
    IrProgram program;
    auto& block = program.CreateBlock();
    program.SetEntryBlock(block);
    program.BlockOrder().push_back(&block);
    IrBuilder builder(program);
    builder.SetInsertionPoint(block);
    auto& reversed = builder.Emit(IrOpcode::BitReverse32, IrType::U32, {&builder.Constant(word)});
    auto& test = builder.Emit(IrOpcode::IEqual32, IrType::Bool, {&reversed, &builder.Constant(expected)});
    auto& user = builder.Emit(IrOpcode::Reference, IrType::Void, {&test});
    static_cast<void>(builder.Emit(IrOpcode::Return, IrType::Void, {}));
    ConstantFolder().Fold(program);
    const IrValue* result = user.Argument(0)->Resolve();
    return result->HasImmediate() && result->ImmediateBool();
}

bool run(const char* name, bool passed) {
    if (!passed) std::fprintf(stderr, "%s\n", name);
    return passed;
}

}

int main() {
    try {
        bool passed = true;
        passed &= run("an all-zero word must fold the lane bit test to false", foldsTo(0u, false, true));
        passed &= run("an all-one word must fold the lane bit test to true", foldsTo(0xffffffffu, true, true));
        passed &= run("a mixed word must keep the lane bit test", foldsTo(0x0000ffffu, false, true));
        passed &= run("an unmasked lane number must keep the lane bit test", foldsTo(0xffffffffu, true, false));
        passed &= run("a reversed zero must fold to zero", reversesTo(0x00000000u, 0x00000000u));
        passed &= run("a reversed single bit must fold to the opposite end", reversesTo(0x00000001u, 0x80000000u));
        passed &= run("a reversed high bit must fold to bit zero", reversesTo(0x80000000u, 0x00000001u));
        passed &= run("a reversed mixed word must fold to its mirror image", reversesTo(0x12345678u, 0x1e6a2c48u));
        passed &= run("a reversed descriptor dword must fold to its mirror image", reversesTo(0xdeadbeefu, 0xf77db57bu));
        passed &= run("a repeated mask folds into the first one", masksFold(0xffffu, 0xffffu));
        passed &= run("a narrower mask reads through the wider one", masksFold(0xffffu, 0x7fffu));
        passed &= run("a wider mask after a narrower one is dropped", masksFold(0x00ffu, 0xffffu));
        passed &= run("a mask under a merge of the halves reads through it", mergedHalfMask());
        passed &= run("half extractions read the half that was merged in", halfExtractions());
        passed &= run("the two halves of one word rejoin into the word", halvesRejoin());
        passed &= run("a 24-bit field read of a value masked to 16 bits folds into the value", factorFieldKept(false));
        passed &= run("a 24-bit field read that a multiply uses is kept, even for a factor masked to 16 bits", factorFieldKept(true));
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
