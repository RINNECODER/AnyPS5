// C++ port of the pure helpers in core/aot/fma.py: the VFMADD/VFMSUB mnemonic grammar.
// The lifter itself (analyze/lower) needs the compiler's IR emitter and is not ported yet.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace aot_cpp {

// Mirrors fma._parts: the four capture groups of
// ^vf(n?)(maddsub|msubadd|madd|msub)(132|213|231)(ss|sd|ps|pd)$ or std::nullopt.
std::optional<std::array<std::string, 4>> fma_parts(const std::string& mnemonic);

// Mirrors fma._vector_type.
std::string fma_vector_type(std::int64_t bits, const std::string& element);

}  // namespace aot_cpp
