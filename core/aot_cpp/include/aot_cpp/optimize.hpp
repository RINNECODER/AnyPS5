// C++ port of core/aot/optimize.py: reconstruct constant stores into memcpy'd constants.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aot_cpp {

struct OptimizeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ConstantStoreResult {
    std::string ir;
    std::int64_t count = 0;
};

// Returns the rewritten IR (with a trailing newline) and the number of constants replaced.
// Throws OptimizeError when the input is not little-endian Apple/Linux ARM64 IR or has already
// been optimized.
ConstantStoreResult merge_constant_stores(std::string_view ir);

}  // namespace aot_cpp
