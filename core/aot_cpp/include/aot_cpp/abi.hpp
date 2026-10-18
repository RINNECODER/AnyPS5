// C++ port of core/aot/abi.py: the scalar guest/native ABI tables and encoders.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "aot_cpp/json.hpp"

namespace aot_cpp {

struct AbiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

extern const char* const kUniformConvention;

bool is_integer_type(const std::string& kind);
bool is_float_type(const std::string& kind);
bool is_abi_type(const std::string& kind);

int bits(const std::string& kind);
std::string llvm_type(const std::string& kind);
std::string extension(const std::string& kind);
std::string parameter_type(const std::string& kind);
std::string result_type(const std::string& kind);
// std::nullopt for void, otherwise the SysV scalar return register name.
std::optional<std::string> return_register(const std::string& kind);

// A scalar parameter or return slot: a register or an ordered stack offset.
struct Location {
    std::string kind;   // "register" or "stack"
    std::string type;   // the scalar ABI kind
    int width = 0;
    std::string name;    // set for kind == "register"
    std::int64_t offset = 0;  // set for kind == "stack"
};

Json location_json(const Location& location);
std::vector<Location> locations(const std::vector<std::string>& parameters);
std::vector<Location> uniform_locations();
std::int64_t uniform_stack_end();

bool uniform(const Json& contract);
std::pair<std::string, std::vector<std::string>> signature(const Json& contract);
void validate_import_annotations(const Json& contracts, const Json& annotations);

}  // namespace aot_cpp
