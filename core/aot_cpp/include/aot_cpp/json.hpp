// Minimal dependency-free JSON value used by the AOT C++ port differential harness.
//
// The port is checked against the Python reference in core/aot on synthetic inputs: the
// driver pipes newline-delimited JSON cases to the C++ CLI and compares the JSON it prints
// back. Only the subset the harness needs is implemented (null, bool, int64, string, array,
// object); floats are rejected so integer formatting cannot drift between the two languages.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aot_cpp {

class Json {
public:
    enum class Type { Null, Bool, Int, String, Array, Object };

    Json() = default;
    static Json null() { return Json(); }
    static Json boolean(bool value);
    static Json integer(std::int64_t value);
    static Json string(std::string value);
    static Json array(std::vector<Json> values);
    static Json object(std::vector<std::pair<std::string, Json>> members);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool as_bool() const { return boolean_; }
    std::int64_t as_int() const { return integer_; }
    const std::string& as_string() const { return string_; }
    const std::vector<Json>& as_array() const { return array_; }
    const std::vector<std::pair<std::string, Json>>& as_object() const { return object_; }

    // Returns nullptr when this value is not an object or the key is absent.
    const Json* find(std::string_view key) const;

    // Appends a member to an object; later keys with the same name win on dump().
    void insert(std::string key, Json value);

    // Canonical, whitespace-free encoding. Object keys are sorted so the encoding is stable.
    std::string dump() const;

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    std::int64_t integer_ = 0;
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

// Parses one JSON value. On failure returns std::nullopt and fills `error`.
std::optional<Json> parse_json(std::string_view text, std::string& error);

}  // namespace aot_cpp
