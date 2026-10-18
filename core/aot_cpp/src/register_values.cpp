#include "aot_cpp/register_values.hpp"

namespace aot_cpp {

std::int64_t low_prefix(std::int64_t mask) {
    const std::uint64_t bits = static_cast<std::uint64_t>(mask);
    std::int64_t count = 0;
    while (count < 64 && (bits & (std::uint64_t(1) << count)) != 0) {
        ++count;
    }
    return count;
}

}  // namespace aot_cpp
