#include "aot_cpp/guest_provider_calls.hpp"

namespace aot_cpp {
namespace {

constexpr int kOnceKind = 11;

}  // namespace

const std::vector<std::pair<std::string, ProviderCall>>& guest_provider_calls() {
    static const std::vector<std::pair<std::string, ProviderCall>> table = {
        {"malloc", {1, "Malloc", 1, true}},
        {"calloc", {2, "Calloc", 2, true}},
        {"realloc", {3, "Realloc", 2, true}},
        {"free", {4, "Free", 1, false}},
        {"memcpy", {5, "Memcpy", 3, true}},
        {"memmove", {6, "Memmove", 3, true}},
        {"memset", {7, "Memset", 3, true}},
        {"memcmp", {8, "Memcmp", 3, true}},
        {"strlen", {9, "Strlen", 1, true}},
        {"pthread_getspecific", {10, "GetSpecific", 1, true}},
        {"pthread_once", {kOnceKind, "Once", 2, true}},
    };
    return table;
}

std::vector<std::string> guest_provider_declarations() {
    std::vector<std::string> result = {
        "declare i1 @NativeAotGlibcFastBinding(i64, i64)",
        "declare i1 @NativeAotGlibcOnceReady(i64)",
    };
    for (const auto& entry : guest_provider_calls()) {
        const ProviderCall& call = entry.second;
        if (call.kind == kOnceKind) {
            continue;
        }
        std::string arguments;
        for (int index = 0; index < call.arity; ++index) {
            if (index) arguments += ", ";
            arguments += "i64";
        }
        result.push_back("declare i64 @NativeAotGlibcFast" + call.name + "(" + arguments + ")");
    }
    return result;
}

}  // namespace aot_cpp
