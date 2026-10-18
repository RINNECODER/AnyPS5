#include "aot_cpp/abi.hpp"

#include <cctype>

namespace aot_cpp {
namespace {

bool is_object(const Json& value) { return value.type() == Json::Type::Object; }

bool truthy(const Json& value) {
    switch (value.type()) {
        case Json::Type::Null: return false;
        case Json::Type::Bool: return value.as_bool();
        case Json::Type::Int: return value.as_int() != 0;
        case Json::Type::String: return !value.as_string().empty();
        case Json::Type::Array: return !value.as_array().empty();
        case Json::Type::Object: return !value.as_object().empty();
    }
    return false;
}

Json field_or_null(const Json& object, const std::string& key) {
    const Json* found = object.find(key);
    return found ? *found : Json::null();
}

std::string string_field(const Json& object, const std::string& key, const std::string& fallback) {
    const Json* found = object.find(key);
    if (found && found->type() == Json::Type::String) {
        return found->as_string();
    }
    return fallback;
}

bool is_symbol(const Json& value) {
    if (value.type() != Json::Type::String) {
        return false;
    }
    const std::string& name = value.as_string();
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    for (char character : name) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_')) {
            return false;
        }
    }
    return true;
}

Json array_field(const Json& object, const std::string& key) {
    const Json* found = object.find(key);
    if (!found || found->is_null()) {
        return Json::array({});
    }
    if (found->type() != Json::Type::Array) {
        throw AbiError("parameters must be a scalar type list");
    }
    return *found;
}

std::vector<std::string> parameter_kinds(const Json& contract) {
    Json parameters = array_field(contract, "parameters");
    std::vector<std::string> kinds;
    kinds.reserve(parameters.as_array().size());
    for (const Json& item : parameters.as_array()) {
        if (item.type() != Json::Type::String) {
            throw AbiError("parameters must be a scalar type list");
        }
        kinds.push_back(item.as_string());
    }
    return kinds;
}

}  // namespace

bool uniform(const Json& contract) {
    if (!is_object(contract)) {
        throw AbiError("function contract must be an object");
    }
    return string_field(contract, "guest_calling_convention", "") == kUniformConvention ||
           truthy(field_or_null(contract, "uniform"));
}

std::pair<std::string, std::vector<std::string>> signature(const Json& contract) {
    if (!is_object(contract)) {
        throw AbiError("function contract must be an object");
    }
    if (uniform(contract)) {
        if (string_field(contract, "result", "void") != "void" || truthy(field_or_null(contract, "parameters"))) {
            throw AbiError("uniform ABI must declare a void result and no scalar parameters");
        }
        if (!is_symbol(field_or_null(contract, "name"))) {
            throw AbiError("invalid native symbol");
        }
        return {"void", {}};
    }
    if (truthy(field_or_null(contract, "variadic"))) {
        throw AbiError("variadic ABI requires argument and va_list reconstruction");
    }
    if (truthy(field_or_null(contract, "sret")) || truthy(field_or_null(contract, "byval"))) {
        throw AbiError("aggregate ABI requires type layout reconstruction");
    }
    const Json* result_value = contract.find("result");
    if (!result_value || result_value->type() != Json::Type::String || !is_abi_type(result_value->as_string())) {
        throw AbiError("unsupported result type");
    }
    std::string result = result_value->as_string();
    std::vector<std::string> parameters = parameter_kinds(contract);
    std::vector<Location> expected = locations(parameters);
    if (!is_symbol(field_or_null(contract, "name"))) {
        throw AbiError("invalid native symbol");
    }
    std::string convention = string_field(contract, "guest_calling_convention", "x86_64-sysv");
    if (convention != "x86_64-sysv" && convention != "x86_64-llvm-fastcc" && convention != kUniformConvention) {
        throw AbiError("unsupported guest calling convention");
    }
    if (convention != "x86_64-sysv" || contract.find("guest_locations") != nullptr) {
        Json expected_json;
        std::vector<Json> items;
        for (const Location& location : expected) {
            items.push_back(location_json(location));
        }
        expected_json = Json::array(std::move(items));
        if (field_or_null(contract, "guest_locations").dump() != expected_json.dump()) {
            throw AbiError("nonstandard guest ABI requires explicit verified SysV-compatible parameter locations");
        }
        std::optional<std::string> expected_register = return_register(result);
        Json register_json = expected_register ? Json::string(*expected_register) : Json::null();
        if (field_or_null(contract, "guest_return_register").dump() != register_json.dump()) {
            throw AbiError("nonstandard guest ABI requires explicit verified return register");
        }
    }
    return {result, parameters};
}

void validate_import_annotations(const Json& contracts, const Json& annotations) {
    if (!is_object(contracts) || contracts.find("imports") == nullptr || !is_object(*contracts.find("imports"))) {
        throw AbiError("contracts must declare an imports map");
    }
    const Json* records = is_object(annotations) ? annotations.find("imports") : nullptr;
    if (!records || !is_object(*records)) {
        throw AbiError("ABI annotations must declare an imports map");
    }
    for (const auto& entry : contracts.find("imports")->as_object()) {
        const std::string& nid = entry.first;
        const Json& contract = entry.second;
        if (!is_object(contract)) {
            throw AbiError("import " + nid + " contract must be an object");
        }
        // Python: record = records.get(nid) or {} -- a falsy value becomes an empty object,
        // a truthy non-object is then dereferenced like a dict and raises.
        const Json* found = records->find(nid);
        Json empty = Json::object({});
        Json record = (found && truthy(*found)) ? *found : empty;
        if (record.type() != Json::Type::Object) {
            throw AbiError("ABI annotations must declare an imports map");
        }
        if (string_field(record, "status", "") == "verified") {
            if (truthy(field_or_null(contract, "stub"))) {
                throw AbiError("import " + nid + " is verified as " +
                               string_field(record, "hle_name", "") + " but the contracts stub it; " +
                               "regenerate the contracts from the current ABI");
            }
            Json contract_parameters = array_field(contract, "parameters");
            Json record_parameters = array_field(record, "parameters");
            if (field_or_null(contract, "result").dump() != field_or_null(record, "result").dump() ||
                contract_parameters.dump() != record_parameters.dump()) {
                throw AbiError("import " + nid +
                               " ABI disagrees with the contracts; regenerate the contracts from the current ABI");
            }
        } else if (!truthy(field_or_null(contract, "stub"))) {
            throw AbiError("import " + nid + " is not verified in the ABI but the contracts type it; " +
                           "regenerate the contracts from the current ABI");
        }
    }
}

}  // namespace aot_cpp
