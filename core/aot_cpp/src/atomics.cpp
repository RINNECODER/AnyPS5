#include "aot_cpp/atomics.hpp"

namespace aot_cpp {
namespace {

bool is_atomic_width(std::int64_t bits) {
    return bits == 8 || bits == 16 || bits == 32 || bits == 64;
}

}  // namespace

std::string ordered_load(std::int64_t bits, const std::string& pointer,
                         std::optional<std::int64_t> align) {
    if (!is_atomic_width(bits)) {
        return "load i" + std::to_string(bits) + ", ptr " + pointer + ", align 1";
    }
    const std::int64_t width = bits / 8;
    if (!align || *align >= width) {
        return "load atomic i" + std::to_string(bits) + ", ptr " + pointer + " acquire, align " +
               std::to_string(width);
    }
    return "load i" + std::to_string(bits) + ", ptr " + pointer + ", align " +
           std::to_string(*align) + "\n  fence acquire";
}

std::string ordered_store(std::int64_t bits, const std::string& value, const std::string& pointer,
                          std::optional<std::int64_t> align) {
    if (!is_atomic_width(bits)) {
        return "store i" + std::to_string(bits) + " " + value + ", ptr " + pointer + ", align 1";
    }
    const std::int64_t width = bits / 8;
    if (!align || *align >= width) {
        return "store atomic i" + std::to_string(bits) + " " + value + ", ptr " + pointer +
               " release, align " + std::to_string(width);
    }
    return "  fence release\n  store i" + std::to_string(bits) + " " + value + ", ptr " + pointer +
           ", align " + std::to_string(*align);
}

Accumulator accumulator(std::int64_t size) {
    return {"rax", size * 8, 0};
}

}  // namespace aot_cpp
