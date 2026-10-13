#ifndef CODEGEN_X86_RECIPROCALOPERANDS_HPP
#define CODEGEN_X86_RECIPROCALOPERANDS_HPP

#include <codegen/x86/StubBodyBuilder.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Codegen {

enum class ReciprocalOperation : std::uint8_t {
    Reciprocal,
    ReciprocalSquareRoot
};

enum class ReciprocalWidth : std::uint8_t {
    Packed128,
    Packed256,
    Scalar
};

struct ReciprocalOperands {
    ReciprocalOperation Operation;
    std::uint8_t Destination;
    // The register source; unused when Memory is set.
    std::uint8_t Source;
    bool Vex;
    ReciprocalWidth Width = ReciprocalWidth::Packed128;
    // VEX scalar forms only: the register that supplies bits 127:32 of the result.
    std::uint8_t FirstSource = 0;
    std::optional<MemoryOperand> Memory = std::nullopt;
};

[[nodiscard]] std::optional<ReciprocalOperands> DecodeReciprocal(const std::uint8_t* data, std::size_t length);

}

#endif
