// C++ port of the pure helpers in core/aot/register_aliases.py.
#pragma once

#include <string>
#include <vector>

#include "aot_cpp/json.hpp"

namespace aot_cpp {

// Mirrors register_aliases.equivalents: every name that shares an alias pair with `name`,
// sorted for a stable encoding (the Python reference returns a set).
std::vector<std::string> register_equivalents(const Json& state, const std::string& name);

}  // namespace aot_cpp
