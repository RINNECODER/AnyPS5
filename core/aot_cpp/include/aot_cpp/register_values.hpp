// C++ port of the pure helpers in core/aot/register_values.py.
#pragma once

#include <cstdint>

namespace aot_cpp {

// Mirrors register_values.low_prefix: the count of consecutive low set bits (capped at 64).
std::int64_t low_prefix(std::int64_t mask);

}  // namespace aot_cpp
