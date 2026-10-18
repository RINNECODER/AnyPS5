// C++ port of the pure helpers in core/aot/integer.py: the condition-code alias table.
#pragma once

#include <string>

namespace aot_cpp {

// Mirrors integer.condition: strip the cmov/set prefix, resolve the alias table.
std::string integer_condition(const std::string& mnemonic);

}  // namespace aot_cpp
