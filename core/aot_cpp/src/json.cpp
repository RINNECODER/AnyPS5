#include "aot_cpp/json.hpp"

#include <algorithm>
#include <cstdio>

namespace aot_cpp {

Json Json::boolean(bool value) {
    Json result;
    result.type_ = Type::Bool;
    result.boolean_ = value;
    return result;
}

Json Json::integer(std::int64_t value) {
    Json result;
    result.type_ = Type::Int;
    result.integer_ = value;
    return result;
}

Json Json::string(std::string value) {
    Json result;
    result.type_ = Type::String;
    result.string_ = std::move(value);
    return result;
}

Json Json::array(std::vector<Json> values) {
    Json result;
    result.type_ = Type::Array;
    result.array_ = std::move(values);
    return result;
}

Json Json::object(std::vector<std::pair<std::string, Json>> members) {
    Json result;
    result.type_ = Type::Object;
    result.object_ = std::move(members);
    return result;
}

const Json* Json::find(std::string_view key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (auto member = object_.rbegin(); member != object_.rend(); ++member) {
        if (member->first == key) {
            return &member->second;
        }
    }
    return nullptr;
}

void Json::insert(std::string key, Json value) {
    type_ = Type::Object;
    object_.emplace_back(std::move(key), std::move(value));
}

namespace {

void escape_into(const std::string& text, std::string& out) {
    out.push_back('"');
    for (unsigned char byte : text) {
        switch (byte) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", byte);
                    out += buffer;
                } else {
                    out.push_back(static_cast<char>(byte));
                }
        }
    }
    out.push_back('"');
}

}  // namespace

std::string Json::dump() const {
    std::string out;
    switch (type_) {
        case Type::Null: out = "null"; break;
        case Type::Bool: out = boolean_ ? "true" : "false"; break;
        case Type::Int: out = std::to_string(integer_); break;
        case Type::String: escape_into(string_, out); break;
        case Type::Array: {
            out.push_back('[');
            for (std::size_t index = 0; index < array_.size(); ++index) {
                if (index) out.push_back(',');
                out += array_[index].dump();
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            std::vector<std::pair<std::string, const Json*>> members;
            members.reserve(object_.size());
            for (const auto& member : object_) {
                members.emplace_back(member.first, &member.second);
            }
            std::stable_sort(members.begin(), members.end(),
                             [](const auto& left, const auto& right) { return left.first < right.first; });
            out.push_back('{');
            bool first = true;
            for (const auto& member : members) {
                if (!first) out.push_back(',');
                first = false;
                escape_into(member.first, out);
                out.push_back(':');
                out += member.second->dump();
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

}  // namespace aot_cpp
