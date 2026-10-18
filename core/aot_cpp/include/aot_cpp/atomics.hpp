// C++ port of core/aot/atomics.py: the TSO ordering policy for scalar guest accesses.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aot_cpp {

// The TSO load of a guest scalar: acquire, so later accesses cannot move before it.
std::string ordered_load(std::int64_t bits, const std::string& pointer,
                         std::optional<std::int64_t> align);

// The TSO store of a guest scalar: release, so earlier accesses cannot move after it.
std::string ordered_store(std::int64_t bits, const std::string& value, const std::string& pointer,
                          std::optional<std::int64_t> align);

// cmpxchg's implicit accumulator: AL / AX / EAX / RAX for 1 / 2 / 4 / 8 bytes.
struct Accumulator {
    std::string name;
    std::int64_t bits;
    std::int64_t shift;
};
Accumulator accumulator(std::int64_t size);

}  // namespace aot_cpp
