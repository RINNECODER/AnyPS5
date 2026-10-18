// C++ port of core/aot/guest_provider_calls.py: the glibc fast-binding call table.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace aot_cpp {

struct ProviderCall {
    int kind = 0;
    std::string name;
    int arity = 0;
    bool returns_value = false;
};

// Every glibc fast binding, in the Python table's declaration order.
const std::vector<std::pair<std::string, ProviderCall>>& guest_provider_calls();

// The `declare` lines the image emits for the fast bindings and their readiness helpers.
std::vector<std::string> guest_provider_declarations();

}  // namespace aot_cpp
