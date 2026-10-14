#ifndef CODEGEN_X86_SCALARCOMPAREOPERANDS_HPP
#define CODEGEN_X86_SCALARCOMPAREOPERANDS_HPP

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Codegen {

// CMPSS / CMPSD (F3 / F2 0F C2 /r ib) and their VEX forms (VCMPSS / VCMPSD).
struct ScalarCompare {
    bool Vex;
    bool Double;
    // imm8[4:0] for VEX, imm8[2:0] for the legacy encodings.
    std::uint8_t Predicate;
};

[[nodiscard]] std::optional<ScalarCompare> DecodeScalarCompare(const std::uint8_t* data, std::size_t length);

// Rosetta 2 translates a scalar compare whose predicate is not EQ_OQ, LT_OS or LE_OS into several Arm
// instructions, and when a full 128-bit VEX load (VMOVAPS/VMOVUPS/VMOVDQA) then overwrites the compare's
// destination in the same translated block, a following scalar FP read of that register (VUCOMISS, VADDSS, ...)
// gets the compare's mask instead of the loaded value. Running the compare out of line (its own block) avoids it.
[[nodiscard]] bool RosettaSplitsScalarCompare(const ScalarCompare& compare);

}

#endif
