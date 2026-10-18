#include "aot_cpp/abi.hpp"

#include <algorithm>

namespace aot_cpp {

const char* const kUniformConvention = "x86_64-uniform";

namespace {

const std::vector<std::string> kIntegerRegisters = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
const std::vector<std::string> kFloatRegisters = {"xmm0", "xmm1", "xmm2", "xmm3",
                                                  "xmm4", "xmm5", "xmm6", "xmm7"};
const int kUniformGprs = 16;
const int kUniformVectors = 16;
const int kUniformStackSlots = 32;
const int kUniformStackBase = 8;

const std::vector<std::string>& integer_types() {
    static const std::vector<std::string> types = {"i8", "u8", "i16", "u16", "i32",
                                                   "u32", "i64", "u64", "ptr"};
    return types;
}

const std::vector<std::string>& float_types() {
    static const std::vector<std::string> types = {"f32", "f64"};
    return types;
}

bool contains(const std::vector<std::string>& values, const std::string& needle) {
    return std::find(values.begin(), values.end(), needle) != values.end();
}

[[noreturn]] void fail(const std::string& message) { throw AbiError(message); }

}  // namespace

bool is_integer_type(const std::string& kind) { return contains(integer_types(), kind); }
bool is_float_type(const std::string& kind) { return contains(float_types(), kind); }
bool is_abi_type(const std::string& kind) { return kind == "void" || is_integer_type(kind) || is_float_type(kind); }

int bits(const std::string& kind) {
    if (!is_abi_type(kind) || kind == "void") {
        fail("unsupported scalar ABI type: " + kind);
    }
    return kind == "ptr" ? 64 : std::stoi(kind.substr(1));
}

std::string llvm_type(const std::string& kind) {
    if (!is_abi_type(kind)) {
        fail("unsupported scalar ABI type: " + kind);
    }
    if (kind == "f32") return "float";
    if (kind == "f64") return "double";
    if (kind[0] == 'u') return "i" + kind.substr(1);
    return kind;
}

std::string extension(const std::string& kind) {
    if (kind == "i8" || kind == "i16") return "signext";
    if (kind == "u8" || kind == "u16") return "zeroext";
    return "";
}

std::string parameter_type(const std::string& kind) {
    if (kind == "void") {
        fail("void is not a parameter type");
    }
    std::string type = llvm_type(kind);
    std::string attribute = extension(kind);
    return attribute.empty() ? type : type + " " + attribute;
}

std::string result_type(const std::string& kind) {
    std::string type = llvm_type(kind);
    std::string attribute = extension(kind);
    return attribute.empty() ? type : attribute + " " + type;
}

std::optional<std::string> return_register(const std::string& kind) {
    llvm_type(kind);
    if (kind == "void") return std::nullopt;
    return is_float_type(kind) ? std::optional<std::string>("xmm0") : std::optional<std::string>("rax");
}

Json location_json(const Location& location) {
    Json result;
    result.insert("type", Json::string(location.type));
    result.insert("bits", Json::integer(location.width));
    result.insert("kind", Json::string(location.kind));
    if (location.kind == "register") {
        result.insert("name", Json::string(location.name));
    } else {
        result.insert("offset", Json::integer(location.offset));
    }
    return result;
}

std::vector<Location> locations(const std::vector<std::string>& parameters) {
    std::size_t integer_index = 0;
    std::size_t float_index = 0;
    std::int64_t offset = 8;
    std::vector<Location> result;
    result.reserve(parameters.size());
    for (const std::string& kind : parameters) {
        Location descriptor;
        descriptor.type = kind;
        descriptor.width = bits(kind);
        if (is_integer_type(kind) && integer_index < kIntegerRegisters.size()) {
            descriptor.kind = "register";
            descriptor.name = kIntegerRegisters[integer_index++];
        } else if (is_float_type(kind) && float_index < kFloatRegisters.size()) {
            descriptor.kind = "register";
            descriptor.name = kFloatRegisters[float_index++];
        } else {
            descriptor.kind = "stack";
            descriptor.offset = offset;
            offset += 8;
        }
        result.push_back(std::move(descriptor));
    }
    return result;
}

std::vector<Location> uniform_locations() {
    std::vector<Location> result;
    for (int index = 0; index < kUniformGprs; ++index) {
        static const std::vector<std::string> gprs = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                                      "rsi", "rdi", "r8", "r9", "r10", "r11",
                                                      "r12", "r13", "r14", "r15"};
        result.push_back({"register", "ptr", 64, gprs[index], 0});
    }
    for (int index = 0; index < kUniformVectors; ++index) {
        result.push_back({"register", "f64", 128, "xmm" + std::to_string(index), 0});
    }
    for (int index = 0; index < kUniformVectors; ++index) {
        result.push_back({"register", "f64", 128, "ymm_hi" + std::to_string(index), 0});
    }
    for (int index = 0; index < kUniformStackSlots; ++index) {
        result.push_back({"stack", "i64", 64, "", kUniformStackBase + 8 * index});
    }
    return result;
}

std::int64_t uniform_stack_end() { return kUniformStackBase + 8 * kUniformStackSlots; }

}  // namespace aot_cpp
