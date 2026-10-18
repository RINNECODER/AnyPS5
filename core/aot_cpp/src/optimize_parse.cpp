#include "optimize_parse.hpp"

#include <string>
#include <vector>

namespace aot_cpp::detail {
namespace {

bool ident_char(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-' || character == '.';
}

bool consume(std::string_view text, std::size_t& position, std::string_view token) {
    if (text.substr(position, token.size()) != token) {
        return false;
    }
    position += token.size();
    return true;
}

void skip_space(std::string_view text, std::size_t& position) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t')) {
        ++position;
    }
}

bool parse_identifier(std::string_view text, std::size_t& position, std::string& out) {
    if (position >= text.size() || text[position] != '%') {
        return false;
    }
    std::size_t begin = position++;
    while (position < text.size() && ident_char(text[position])) {
        ++position;
    }
    if (position == begin + 1) {
        return false;
    }
    out = std::string(text.substr(begin, position - begin));
    return true;
}

bool parse_integer(std::string_view text, std::size_t& position, std::int64_t& out) {
    std::size_t begin = position;
    bool negative = position < text.size() && text[position] == '-';
    if (negative) {
        ++position;
    }
    std::size_t digits = position;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        ++position;
    }
    if (position == digits) {
        return false;
    }
    out = std::stoll(std::string(text.substr(begin, position - begin)));
    return true;
}

}  // namespace

std::optional<Allocation> parse_allocation(std::string_view line) {
    std::size_t position = 0;
    skip_space(line, position);
    Allocation result;
    if (!parse_identifier(line, position, result.name) ||
        !consume(line, position, " = alloca [") ||
        !parse_integer(line, position, result.size) ||
        !consume(line, position, " x i8],")) {
        return std::nullopt;
    }
    return result;
}

std::optional<Gep> parse_gep(std::string_view line) {
    std::size_t position = 0;
    skip_space(line, position);
    Gep result;
    if (!parse_identifier(line, position, result.name) ||
        !consume(line, position, " = getelementptr")) {
        return std::nullopt;
    }
    if (!consume(line, position, " inbounds")) {
        consume(line, position, " nuw");
    }
    if (!consume(line, position, " i8, ptr ") ||
        !parse_identifier(line, position, result.base) ||
        !consume(line, position, ", i64 ") ||
        !parse_integer(line, position, result.delta) ||
        position != line.size()) {
        return std::nullopt;
    }
    return result;
}

std::optional<Store> parse_store(std::string_view line) {
    std::size_t position = 0;
    skip_space(line, position);
    if (!consume(line, position, "store ")) {
        return std::nullopt;
    }
    consume(line, position, "atomic ");
    if (position >= line.size() || line[position] != 'i') {
        return std::nullopt;
    }
    ++position;
    std::int64_t width = 0;
    if (!parse_integer(line, position, width)) {
        return std::nullopt;
    }
    if (width != 8 && width != 16 && width != 32 && width != 64) {
        return std::nullopt;
    }
    if (!consume(line, position, " ")) {
        return std::nullopt;
    }
    Store result;
    if (!parse_integer(line, position, result.value) ||
        !consume(line, position, ", ptr ") ||
        !parse_identifier(line, position, result.pointer)) {
        return std::nullopt;
    }
    if (!consume(line, position, ", align ")) {
        if (!consume(line, position, " ")) {
            return std::nullopt;
        }
        static const std::vector<std::string_view> orderings = {"monotonic", "acquire", "release",
                                                                "acq_rel", "seq_cst"};
        bool matched = false;
        for (std::string_view ordering : orderings) {
            if (consume(line, position, ordering)) {
                matched = true;
                break;
            }
        }
        if (!matched || !consume(line, position, ", align ")) {
            return std::nullopt;
        }
    }
    std::int64_t alignment = 0;
    if (!parse_integer(line, position, alignment) || position != line.size()) {
        return std::nullopt;
    }
    result.width_bits = static_cast<int>(width);
    return result;
}

}  // namespace aot_cpp::detail
