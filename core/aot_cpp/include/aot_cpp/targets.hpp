// C++ port of core/aot/targets.py (native target descriptors).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aot_cpp {

struct TargetSpec {
    std::string triple;
    std::string readonly_section;
};

// Every supported target name, in the table's declaration order.
const std::vector<std::pair<std::string, TargetSpec>>& targets();

// The descriptor for `name`, or std::nullopt when the target is unsupported.
std::optional<TargetSpec> target_spec(std::string_view name);

}  // namespace aot_cpp
