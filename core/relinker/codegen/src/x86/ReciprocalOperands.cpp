#include <codegen/x86/ReciprocalOperands.hpp>
#include <codegen/x86/X64OpcodeConstants.hpp>
#include <codegen/CodegenException.hpp>
#include <utility>
#include <vector>

namespace Codegen {

namespace {

constexpr std::uint8_t kVexMap0F = 0x01;
constexpr std::uint8_t kVexUnusedRegister = 0x0F;
constexpr std::uint8_t kVexPpNone = 0;
constexpr std::uint8_t kVexPpScalar = 2;
constexpr std::uint8_t kOpcodeRsqrt = 0x52;
constexpr std::uint8_t kOpcodeRcp = 0x53;
constexpr std::uint8_t kRexX = 0x02;
constexpr std::uint8_t kRexB = 0x01;

bool _segmentPrefix(const std::uint8_t byte) {
    using namespace X64OpcodeConstants;
    return byte == PrefixSegCs || byte == PrefixSegSs || byte == PrefixSegDs || byte == PrefixSegEs || byte == PrefixSegFs || byte == PrefixSegGs;
}

// Finishes the decode once the opcode and ModRM positions are known. The instruction must end with the operand.
std::optional<ReciprocalOperands> _operands(const std::uint8_t* data, const std::size_t length, const std::size_t opcodeOffset, const bool registerExtension, const bool rmExtension, const std::uint8_t rexIndexBase, std::vector<std::uint8_t> prefixes) {
    using namespace X64OpcodeConstants;
    const auto opcode = data[opcodeOffset];
    if (opcode != kOpcodeRsqrt && opcode != kOpcodeRcp)
        return std::nullopt;
    const auto modrmOffset = opcodeOffset + 1;
    if (modrmOffset >= length)
        return std::nullopt;
    const auto modrm = data[modrmOffset];
    ReciprocalOperands operands{};
    operands.Operation = opcode == kOpcodeRsqrt ? ReciprocalOperation::ReciprocalSquareRoot : ReciprocalOperation::Reciprocal;
    operands.Destination = static_cast<std::uint8_t>(((modrm >> ModRmRegShift) & ModRmRegMask) | (registerExtension ? 8 : 0));
    if (((modrm >> ModRmModShift) & ModRmModMask) == ModRmModRegister) {
        if (modrmOffset + 1 != length)
            return std::nullopt;
        operands.Source = static_cast<std::uint8_t>((modrm & ModRmRmMask) | (rmExtension ? 8 : 0));
        return operands;
    }
    try {
        operands.Memory = DecodeMemoryOperand(data, length, modrmOffset, rexIndexBase, std::move(prefixes));
    } catch (const CodegenException&) {
        return std::nullopt;
    }
    if (modrmOffset + operands.Memory->EncodedSize != length)
        return std::nullopt;
    return operands;
}

}

std::optional<ReciprocalOperands> DecodeReciprocal(const std::uint8_t* data, const std::size_t length) {
    using namespace X64OpcodeConstants;
    std::size_t pos = 0;
    std::vector<std::uint8_t> prefixes;
    bool scalarPrefix = false;
    while (pos < length) {
        if (_segmentPrefix(data[pos])) {
            prefixes.push_back(data[pos]);
        } else if (data[pos] == PrefixRep && !scalarPrefix) {
            scalarPrefix = true;
        } else {
            break;
        }
        ++pos;
    }
    if (pos >= length)
        return std::nullopt;
    if (data[pos] == OneByteVex2 || data[pos] == OneByteVex3) {
        if (scalarPrefix)
            return std::nullopt;
        const bool vex3 = data[pos] == OneByteVex3;
        const std::size_t header = vex3 ? 3 : 2;
        if (pos + header + 2 > length)
            return std::nullopt;
        bool rmExtension = false;
        std::uint8_t rexIndexBase = 0;
        if (vex3) {
            if ((data[pos + 1] & Vex3MapMask) != kVexMap0F)
                return std::nullopt;
            rmExtension = (data[pos + 1] & 0x20) == 0;
            rexIndexBase = static_cast<std::uint8_t>(((data[pos + 1] & 0x40) == 0 ? kRexX : 0) | (rmExtension ? kRexB : 0));
        }
        const bool registerExtension = (data[pos + 1] & 0x80) == 0;
        const auto payload = data[pos + (vex3 ? 2 : 1)];
        const auto vvvv = static_cast<std::uint8_t>((payload >> 3) & 0x0F);
        const bool ymm = (payload & 0x04) != 0;
        const auto pp = static_cast<std::uint8_t>(payload & 0x03);
        if (pp != kVexPpNone && pp != kVexPpScalar)
            return std::nullopt;
        if (pp == kVexPpNone && vvvv != kVexUnusedRegister)
            return std::nullopt;
        auto operands = _operands(data, length, pos + header, registerExtension, rmExtension, rexIndexBase, std::move(prefixes));
        if (!operands)
            return std::nullopt;
        operands->Vex = true;
        if (pp == kVexPpScalar) {
            operands->Width = ReciprocalWidth::Scalar;
            operands->FirstSource = static_cast<std::uint8_t>(~vvvv & 0x0F);
        } else {
            operands->Width = ymm ? ReciprocalWidth::Packed256 : ReciprocalWidth::Packed128;
        }
        return operands;
    }
    std::uint8_t rex = 0;
    if (data[pos] >= RexMin && data[pos] <= RexMax)
        rex = data[pos++];
    if (pos + 2 >= length || data[pos] != TwoByteOpcodeEscape)
        return std::nullopt;
    auto operands = _operands(data, length, pos + 1, (rex & 0x04) != 0, (rex & kRexB) != 0, static_cast<std::uint8_t>(rex & (kRexX | kRexB)), std::move(prefixes));
    if (!operands)
        return std::nullopt;
    operands->Vex = false;
    operands->Width = scalarPrefix ? ReciprocalWidth::Scalar : ReciprocalWidth::Packed128;
    operands->FirstSource = operands->Destination;
    return operands;
}

}
