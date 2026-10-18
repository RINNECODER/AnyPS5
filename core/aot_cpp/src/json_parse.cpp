#include "aot_cpp/json.hpp"

#include <cstddef>

namespace aot_cpp {
namespace {

bool append_utf8(std::uint32_t code, std::string& out) {
    if (code <= 0x7f) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code <= 0x10ffff) {
        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        return false;
    }
    return true;
}

struct Reader {
    std::string_view text;
    std::size_t position = 0;
    std::string error;

    bool done() const { return position >= text.size(); }

    char peek() const { return position < text.size() ? text[position] : '\0'; }

    void skip() {
        while (!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++position;
        }
    }

    bool fail(const std::string& message) {
        if (error.empty()) {
            error = message + " at offset " + std::to_string(position);
        }
        return false;
    }

    bool literal(std::string_view word) {
        if (text.compare(position, word.size(), word) != 0) {
            return fail("unexpected token");
        }
        position += word.size();
        return true;
    }

    bool hex4(std::uint32_t& value) {
        if (position + 4 > text.size()) {
            return fail("truncated \\u escape");
        }
        value = 0;
        for (int index = 0; index < 4; ++index) {
            char digit = text[position++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') value |= static_cast<std::uint32_t>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') value |= static_cast<std::uint32_t>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') value |= static_cast<std::uint32_t>(digit - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool parse_string(std::string& out) {
        if (peek() != '"') {
            return fail("expected string");
        }
        ++position;
        while (true) {
            if (done()) return fail("unterminated string");
            unsigned char byte = static_cast<unsigned char>(text[position++]);
            if (byte == '"') return true;
            if (byte != '\\') {
                out.push_back(static_cast<char>(byte));
                continue;
            }
            if (done()) return fail("unterminated escape");
            char escape = text[position++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!hex4(code)) return false;
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (position + 1 >= text.size() || text[position] != '\\' || text[position + 1] != 'u') {
                            return fail("lone high surrogate");
                        }
                        position += 2;
                        std::uint32_t low = 0;
                        if (!hex4(low)) return false;
                        if (low < 0xdc00 || low > 0xdfff) return fail("invalid low surrogate");
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    }
                    if (!append_utf8(code, out)) return fail("code point out of range");
                    break;
                }
                default: return fail("invalid escape");
            }
        }
    }

    bool parse_number(Json& value) {
        std::size_t begin = position;
        if (peek() == '-') ++position;
        if (done() || peek() < '0' || peek() > '9') return fail("invalid number");
        if (peek() == '0') {
            ++position;
        } else {
            while (!done() && peek() >= '0' && peek() <= '9') ++position;
        }
        if (!done() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
            return fail("only integer JSON numbers are supported");
        }
        std::string digits(text.substr(begin, position - begin));
        std::int64_t number = 0;
        try {
            number = std::stoll(digits);
        } catch (const std::exception&) {
            return fail("integer out of range");
        }
        value = Json::integer(number);
        return true;
    }

    bool parse_value(Json& value) {
        skip();
        if (done()) return fail("unexpected end of input");
        char lead = peek();
        if (lead == '{') return parse_object(value);
        if (lead == '[') return parse_array(value);
        if (lead == '"') {
            std::string text_value;
            if (!parse_string(text_value)) return false;
            value = Json::string(std::move(text_value));
            return true;
        }
        if (lead == 't') { if (!literal("true")) return false; value = Json::boolean(true); return true; }
        if (lead == 'f') { if (!literal("false")) return false; value = Json::boolean(false); return true; }
        if (lead == 'n') { if (!literal("null")) return false; value = Json::null(); return true; }
        return parse_number(value);
    }

    bool parse_array(Json& value) {
        ++position;
        std::vector<Json> items;
        skip();
        if (peek() == ']') { ++position; value = Json::array(std::move(items)); return true; }
        while (true) {
            Json item;
            if (!parse_value(item)) return false;
            items.push_back(std::move(item));
            skip();
            if (peek() == ',') { ++position; continue; }
            if (peek() == ']') { ++position; break; }
            return fail("expected ',' or ']'");
        }
        value = Json::array(std::move(items));
        return true;
    }

    bool parse_object(Json& value) {
        ++position;
        std::vector<std::pair<std::string, Json>> members;
        skip();
        if (peek() == '}') { ++position; value = Json::object(std::move(members)); return true; }
        while (true) {
            std::string key;
            skip();
            if (!parse_string(key)) return false;
            skip();
            if (peek() != ':') return fail("expected ':'");
            ++position;
            Json member;
            if (!parse_value(member)) return false;
            members.emplace_back(std::move(key), std::move(member));
            skip();
            if (peek() == ',') { ++position; continue; }
            if (peek() == '}') { ++position; break; }
            return fail("expected ',' or '}'");
        }
        value = Json::object(std::move(members));
        return true;
    }
};

}  // namespace

std::optional<Json> parse_json(std::string_view text, std::string& error) {
    Reader reader;
    reader.text = text;
    Json value;
    if (!reader.parse_value(value)) {
        error = reader.error;
        return std::nullopt;
    }
    reader.skip();
    if (!reader.done()) {
        error = "trailing data";
        return std::nullopt;
    }
    return value;
}

}  // namespace aot_cpp
