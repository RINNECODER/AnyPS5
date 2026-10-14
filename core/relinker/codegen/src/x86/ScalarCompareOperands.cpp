#include <codegen/x86/ScalarCompareOperands.hpp>
#include <codegen/x86/X64OpcodeConstants.hpp>

namespace Codegen {

namespace {

constexpr std::uint8_t kOpcodeCmp = 0xC2;
constexpr std::uint8_t kVexMap0F = 0x01;
constexpr std::uint8_t kVexPpF3 = 2;
constexpr std::uint8_t kVexPpF2 = 3;
constexpr std::uint8_t kVexPredicateMask = 0x1F;
constexpr std::uint8_t kLegacyPredicateMask = 0x07;

// The predicates Rosetta translates without losing a later load of the destination (one Arm FCMEQ / FCMGT / FCMGE).
constexpr std::uint8_t kEqOq = 0x00;
constexpr std::uint8_t kLtOs = 0x01;
constexpr std::uint8_t kLeOs = 0x02;

// Legacy prefixes that leave a scalar compare a scalar compare: segment and address-size overrides.
bool _neutralPrefix(const std::uint8_t byte) {
    using namespace X64OpcodeConstants;
    return byte == PrefixSegCs || byte == PrefixSegSs || byte == PrefixSegDs || byte == PrefixSegEs || byte == PrefixSegFs || byte == PrefixSegGs ||
        byte == PrefixAddressSize;
}

}

std::optional<ScalarCompare> DecodeScalarCompare(const std::uint8_t* data, const std::size_t length) {
    using namespace X64OpcodeConstants;
    // Legacy prefixes come in any order; the last F2/F3 among them is the mandatory one.
    std::size_t pos = 0;
    std::uint8_t repeat = 0;
    while (pos < length && (_neutralPrefix(data[pos]) || data[pos] == PrefixRep || data[pos] == PrefixRepne)) {
        if (data[pos] == PrefixRep || data[pos] == PrefixRepne)
            repeat = data[pos];
        ++pos;
    }
    if (pos >= length)
        return std::nullopt;
    // The immediate is the instruction's last byte; the ModRM byte (and any SIB/displacement) sits between it and the opcode.
    if (data[pos] == OneByteVex2 || data[pos] == OneByteVex3) {
        if (repeat != 0)
            return std::nullopt;  // F2/F3 before a VEX escape is #UD
        const bool vex3 = data[pos] == OneByteVex3;
        const std::size_t opcodeOffset = pos + (vex3 ? 3 : 2);
        if (opcodeOffset + 2 >= length || data[opcodeOffset] != kOpcodeCmp)
            return std::nullopt;
        if (vex3 && (data[pos + 1] & Vex3MapMask) != kVexMap0F)
            return std::nullopt;
        const auto pp = static_cast<std::uint8_t>(data[pos + (vex3 ? 2 : 1)] & 0x03);
        if (pp != kVexPpF3 && pp != kVexPpF2)
            return std::nullopt;
        return ScalarCompare{true, pp == kVexPpF2, static_cast<std::uint8_t>(data[length - 1] & kVexPredicateMask)};
    }
    if (repeat == 0)
        return std::nullopt;
    const bool isDouble = repeat == PrefixRepne;
    if (pos < length && data[pos] >= RexMin && data[pos] <= RexMax)
        ++pos;
    if (pos + 3 >= length || data[pos] != TwoByteOpcodeEscape || data[pos + 1] != kOpcodeCmp)
        return std::nullopt;
    return ScalarCompare{false, isDouble, static_cast<std::uint8_t>(data[length - 1] & kLegacyPredicateMask)};
}

bool RosettaSplitsScalarCompare(const ScalarCompare& compare) {
    // Measured under Rosetta (rosetta_cpuid_tests runs every predicate of every form, with and without the split).
    // A few double-precision predicates keep the load there; they are split as well: the stub runs the unchanged
    // instruction, so splitting is exact and costs only the two jumps.
    const auto predicate = compare.Predicate;
    return predicate != kEqOq && predicate != kLtOs && predicate != kLeOs;
}

}
