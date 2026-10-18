#include "aot_cpp/fma.hpp"

#include <string_view>

namespace aot_cpp {
namespace {

// Returns the first alternative that matches at `index`, or "" (advancing `index` on success).
std::string match_at(const std::string_view text, std::size_t& index,
                     std::initializer_list<const char*> alternatives) {
    for (const char* alternative : alternatives) {
        const std::string_view candidate(alternative);
        if (index + candidate.size() <= text.size() &&
            text.compare(index, candidate.size(), candidate) == 0) {
            index += candidate.size();
            return std::string(candidate);
        }
    }
    return "";
}

}  // namespace

std::optional<std::array<std::string, 4>> fma_parts(const std::string& mnemonic) {
    const std::string_view text(mnemonic);
    if (text.size() < 2 || text.compare(0, 2, "vf") != 0) {
        return std::nullopt;
    }
    std::size_t index = 2;
    std::string negate;
    if (index < text.size() && text[index] == 'n') {
        negate = "n";
        ++index;
    }
    std::string operation = match_at(text, index, {"maddsub", "msubadd", "madd", "msub"});
    if (operation.empty()) {
        return std::nullopt;
    }
    std::string order = match_at(text, index, {"132", "213", "231"});
    if (order.empty()) {
        return std::nullopt;
    }
    std::string suffix = match_at(text, index, {"ss", "sd", "ps", "pd"});
    if (suffix.empty() || index != text.size()) {
        return std::nullopt;
    }
    return std::array<std::string, 4>{negate, operation, order, suffix};
}

std::string fma_vector_type(std::int64_t bits, const std::string& element) {
    const std::int64_t width = element == "float" ? 32 : 64;
    return "<" + std::to_string(bits / width) + " x " + element + ">";
}

}  // namespace aot_cpp
