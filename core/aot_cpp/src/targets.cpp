#include "aot_cpp/targets.hpp"

namespace aot_cpp {

const std::vector<std::pair<std::string, TargetSpec>>& targets() {
    static const std::vector<std::pair<std::string, TargetSpec>> table = {
        {"apple-arm64", {"arm64-apple-macosx13.0.0", "__DATA_CONST,__aot_imports"}},
        {"linux-arm64", {"aarch64-unknown-linux-gnu", ".data.rel.ro.aot_imports"}},
    };
    return table;
}

std::optional<TargetSpec> target_spec(std::string_view name) {
    for (const auto& entry : targets()) {
        if (entry.first == name) {
            return entry.second;
        }
    }
    return std::nullopt;
}

}  // namespace aot_cpp
