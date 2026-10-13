#include <codegen/x86/ReciprocalLowering.hpp>
#include <array>
#include <cstring>

namespace Codegen {

namespace {

constexpr std::uint8_t kSqrtps = 0x51;
constexpr std::uint8_t kDivps = 0x5E;
constexpr std::uint8_t kMovaps = 0x28;
constexpr std::uint8_t kMovups = 0x10;
constexpr std::uint8_t kMovupsStore = 0x11;
constexpr std::uint8_t kMovss = 0x10;
constexpr std::uint8_t kPrefixScalar = 0xF3;
constexpr std::uint8_t kVexPpNone = 0;
constexpr std::uint8_t kVexPp66 = 1;
constexpr std::uint8_t kVexPpScalar = 2;
constexpr std::uint8_t kVexMap0F = 1;
constexpr std::uint8_t kVexMap0F38 = 2;
// Register 0 encodes the unused vvvv field (1111).
constexpr std::uint8_t kVexUnused = 0;
constexpr std::uint8_t kBroadcastss = 0x18;
constexpr float kOne = 1.0f;
// 32 bytes for the operand, 4 for 1.0f, below the 128-byte red zone.
constexpr std::size_t kWideFrame = 0xC0;
constexpr std::uint8_t kOneSlot = 0x20;

StubConstant _ones() {
    StubConstant constant{};
    for (std::size_t lane = 0; lane < 4; ++lane)
        std::memcpy(constant.data() + lane * sizeof(kOne), &kOne, sizeof(kOne));
    return constant;
}

// The lowest register that is none of the excluded ones.
std::uint8_t _pick(std::initializer_list<std::uint8_t> excluded) {
    std::uint8_t reg = 0;
    for (;;) {
        bool taken = false;
        for (const auto other : excluded)
            taken = taken || other == reg;
        if (!taken)
            return reg;
        ++reg;
    }
}

// Three-byte VEX with register operands: reg = ModRM.reg, rm = ModRM.rm.
void _vex(StubBodyBuilder& body, const std::uint8_t pp, const bool ymm, const std::uint8_t map, const std::uint8_t opcode, const std::uint8_t reg, const std::uint8_t vvvv, const std::uint8_t rm) {
    const std::array<std::uint8_t, 5> bytes{
        0xC4,
        static_cast<std::uint8_t>(((reg & 8) != 0 ? 0x00 : 0x80) | 0x40 | ((rm & 8) != 0 ? 0x00 : 0x20) | map),
        static_cast<std::uint8_t>((((~vvvv) & 0x0F) << 3) | (ymm ? 0x04 : 0x00) | pp),
        opcode,
        static_cast<std::uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7))
    };
    body.Raw(bytes);
}

// Three-byte VEX with the operand at [rsp + disp8] of the stub's own frame.
void _vexFrame(StubBodyBuilder& body, const std::uint8_t pp, const std::uint8_t map, const std::uint8_t opcode, const std::uint8_t reg, const std::uint8_t vvvv, const std::uint8_t disp) {
    const std::array<std::uint8_t, 7> bytes{
        0xC4,
        static_cast<std::uint8_t>(((reg & 8) != 0 ? 0x00 : 0x80) | 0x40 | 0x20 | map),
        static_cast<std::uint8_t>((((~vvvv) & 0x0F) << 3) | 0x04 | pp),
        opcode,
        static_cast<std::uint8_t>(0x44 | ((reg & 7) << 3)),
        0x24,
        disp
    };
    body.Raw(bytes);
}

void _vexMovaps(StubBodyBuilder& body, const std::uint8_t dst, const std::uint8_t src) {
    _vex(body, kVexPpNone, false, kVexMap0F, kMovaps, dst, kVexUnused, src);
}

// movss xmm_dst, xmm_src, xmm_result: bits 31:0 from result, 127:32 from src, the rest zero.
void _vexMovss(StubBodyBuilder& body, const std::uint8_t dst, const std::uint8_t first, const std::uint8_t result) {
    _vex(body, kVexPpScalar, false, kVexMap0F, kMovss, dst, first, result);
}

void _emitPacked128(StubBodyBuilder& body, const ReciprocalOperands& operands) {
    const auto dst = operands.Destination;
    const auto source = operands.Memory ? dst : operands.Source;
    const auto scratch = _pick({dst, source});
    body.Spill(scratch);
    if (operands.Memory)
        body.SseMemory(0, {0x0F, kMovups}, dst, *operands.Memory);
    if (operands.Operation == ReciprocalOperation::ReciprocalSquareRoot) {
        body.SsePlain({0x0F, kSqrtps}, scratch, source);
        body.RipOperand({0x0F, kMovaps}, dst, _ones());
        body.SsePlain({0x0F, kDivps}, dst, scratch);
        if (operands.Vex)
            _vexMovaps(body, dst, dst);
    } else {
        body.RipOperand({0x0F, kMovaps}, scratch, _ones());
        body.SsePlain({0x0F, kDivps}, scratch, source);
        if (operands.Vex)
            _vexMovaps(body, dst, scratch);
        else
            body.SsePlain({0x0F, kMovaps}, dst, scratch);
    }
    body.Restore(scratch);
}

// No scratch register: the operand goes through the stub's frame, so every ymm register but the destination stays untouched.
void _emitPacked256(StubBodyBuilder& body, const ReciprocalOperands& operands) {
    const auto dst = operands.Destination;
    body.Reserve(kWideFrame);
    auto source = operands.Source;
    if (operands.Memory) {
        body.VexMemory(kVexPpNone, true, kMovups, dst, kVexUnused, *operands.Memory);
        source = dst;
    }
    if (operands.Operation == ReciprocalOperation::ReciprocalSquareRoot) {
        _vex(body, kVexPpNone, true, kVexMap0F, kSqrtps, dst, kVexUnused, source);
        source = dst;
    }
    _vexFrame(body, kVexPpNone, kVexMap0F, kMovupsStore, source, kVexUnused, 0);
    const std::array<std::uint8_t, 8> storeOne{0xC7, 0x44, 0x24, kOneSlot, 0x00, 0x00, 0x80, 0x3F};
    body.Raw(storeOne);
    _vexFrame(body, kVexPp66, kVexMap0F38, kBroadcastss, dst, kVexUnused, kOneSlot);
    _vexFrame(body, kVexPpNone, kVexMap0F, kDivps, dst, dst, 0);
    body.Release(kWideFrame);
}

void _emitScalar(StubBodyBuilder& body, const ReciprocalOperands& operands) {
    const auto dst = operands.Destination;
    const auto first = operands.FirstSource;
    const auto source = operands.Memory ? dst : operands.Source;
    const auto scratch = _pick({dst, first, source});
    const auto second = _pick({dst, first, source, scratch});
    const bool rsqrt = operands.Operation == ReciprocalOperation::ReciprocalSquareRoot;
    const auto sourceOperation = [&](const std::uint8_t opcode, const std::uint8_t target) {
        if (operands.Memory)
            body.SseMemory(kPrefixScalar, {0x0F, opcode}, target, *operands.Memory);
        else
            body.Sse(kPrefixScalar, {0x0F, opcode}, target, source);
    };
    body.Spill(scratch);
    auto result = scratch;
    if (rsqrt) {
        body.Spill(second);
        sourceOperation(kSqrtps, scratch);
        body.RipOperand({0x0F, kMovaps}, second, _ones());
        body.Sse(kPrefixScalar, {0x0F, kDivps}, second, scratch);
        result = second;
    } else {
        body.RipOperand({0x0F, kMovaps}, scratch, _ones());
        sourceOperation(kDivps, scratch);
    }
    if (operands.Vex)
        _vexMovss(body, dst, first, result);
    else
        body.Sse(kPrefixScalar, {0x0F, kMovss}, dst, result);
    if (rsqrt)
        body.Restore(second);
    body.Restore(scratch);
}

}

void ReciprocalLowering::EmitOutOfLine(StubBodyBuilder& body, const ReciprocalOperands& operands) const {
    switch (operands.Width) {
    case ReciprocalWidth::Packed128:
        _emitPacked128(body, operands);
        break;
    case ReciprocalWidth::Packed256:
        _emitPacked256(body, operands);
        break;
    case ReciprocalWidth::Scalar:
        _emitScalar(body, operands);
        break;
    }
}

LoweredBody ReciprocalLowering::LowerOutOfLine(const ReciprocalOperands& operands) const {
    StubBodyBuilder body;
    EmitOutOfLine(body, operands);
    return body.Finish();
}

}
