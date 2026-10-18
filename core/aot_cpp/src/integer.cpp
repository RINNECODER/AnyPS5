#include "aot_cpp/integer.hpp"

#include <unordered_map>

namespace aot_cpp {

std::string integer_condition(const std::string& mnemonic) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"z", "e"},  {"nz", "ne"}, {"c", "b"},   {"nc", "ae"}, {"na", "be"}, {"nbe", "a"},
        {"nb", "ae"}, {"nae", "b"}, {"ng", "le"}, {"nge", "l"}, {"nl", "ge"}, {"nle", "g"},
        {"pe", "p"}, {"po", "np"},
    };
    std::string suffix;
    if (mnemonic.rfind("cmov", 0) == 0) {
        suffix = mnemonic.size() >= 4 ? mnemonic.substr(4) : std::string();
    } else {
        suffix = mnemonic.size() >= 3 ? mnemonic.substr(3) : std::string();
    }
    auto found = aliases.find(suffix);
    return found == aliases.end() ? suffix : found->second;
}

}  // namespace aot_cpp
