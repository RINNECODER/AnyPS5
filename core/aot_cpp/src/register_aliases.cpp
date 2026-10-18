#include "aot_cpp/register_aliases.hpp"

#include <set>

namespace aot_cpp {

std::vector<std::string> register_equivalents(const Json& state, const std::string& name) {
    std::set<std::string> result{name};
    const Json* aliases = state.find("register_aliases");
    if (aliases && aliases->type() == Json::Type::Array) {
        for (const Json& pair : aliases->as_array()) {
            if (pair.type() != Json::Type::Array) {
                continue;
            }
            bool contains = false;
            std::vector<std::string> members;
            for (const Json& item : pair.as_array()) {
                if (item.type() != Json::Type::String) {
                    continue;
                }
                members.push_back(item.as_string());
                if (item.as_string() == name) {
                    contains = true;
                }
            }
            if (contains) {
                result.insert(members.begin(), members.end());
            }
        }
    }
    return std::vector<std::string>(result.begin(), result.end());
}

}  // namespace aot_cpp
