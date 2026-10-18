#include "aot_cpp/calls.hpp"

namespace aot_cpp {

CallSignature structural_signature(const Json& contract) {
    std::string result_kind;
    std::vector<std::string> parameters;
    try {
        auto declared = signature(contract);
        result_kind = declared.first;
        parameters = declared.second;
    } catch (const AbiError& error) {
        throw CallError(error.what());
    }

    CallSignature result;
    result.result = result_type(result_kind);
    for (const std::string& kind : parameters) {
        result.parameter_types.push_back(parameter_type(kind));
    }
    if (auto name = return_register(result_kind)) {
        result.has_return_register = true;
        result.return_register = *name;
    }
    result.locations = locations(parameters);
    return result;
}

}  // namespace aot_cpp
