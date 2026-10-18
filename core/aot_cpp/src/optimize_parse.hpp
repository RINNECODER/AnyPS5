// Line parsers for the optimize.py port; kept separate so optimize.cpp reads as the algorithm.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aot_cpp::detail {

struct Allocation {
    std::string name;
    std::int64_t size = 0;
};

struct Gep {
    std::string name;
    std::string base;
    std::int64_t delta = 0;
};

struct Store {
    int width_bits = 0;
    std::int64_t value = 0;
    std::string pointer;
};

std::optional<Allocation> parse_allocation(std::string_view line);
std::optional<Gep> parse_gep(std::string_view line);
std::optional<Store> parse_store(std::string_view line);

}  // namespace aot_cpp::detail
