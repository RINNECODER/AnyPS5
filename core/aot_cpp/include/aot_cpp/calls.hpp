// C++ port of core/aot/calls.py: the structural call signature used to hash a provider contract.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "aot_cpp/abi.hpp"
#include "aot_cpp/json.hpp"

namespace aot_cpp {

struct CallError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// `abi.signature` composed with the scalar encoders, exactly as calls.structural_signature does.
struct CallSignature {
    std::string result;                        // abi.result_type(kind)
    std::vector<std::string> parameter_types;  // abi.parameter_type(kind) for each parameter
    bool has_return_register = false;          // false when abi.return_register is None
    std::string return_register;               // set when has_return_register
    std::vector<Location> locations;           // abi.locations(parameters)
};

CallSignature structural_signature(const Json& contract);

}  // namespace aot_cpp
