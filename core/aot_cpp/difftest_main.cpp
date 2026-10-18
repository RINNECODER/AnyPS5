// Differential harness CLI: aot_cpp_difftest <module> reads newline-delimited JSON cases from
// stdin and writes one JSON result per line to stdout. The Python driver in tests/difftest.py
// feeds the same inputs to the core/aot reference and requires identical output.
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "aot_cpp/abi.hpp"
#include "aot_cpp/atomics.hpp"
#include "aot_cpp/calls.hpp"
#include "aot_cpp/fma.hpp"
#include "aot_cpp/guest_provider_calls.hpp"
#include "aot_cpp/guest_regions.hpp"
#include "aot_cpp/integer.hpp"
#include "aot_cpp/json.hpp"
#include "aot_cpp/optimize.hpp"
#include "aot_cpp/register_aliases.hpp"
#include "aot_cpp/register_values.hpp"
#include "aot_cpp/targets.hpp"

namespace {

using aot_cpp::Json;

const Json& field(const Json& object, const std::string& key) {
    const Json* found = object.find(key);
    if (!found) {
        throw std::runtime_error("missing field: " + key);
    }
    return *found;
}

std::string text(const Json& object, const std::string& key) {
    const Json& value = field(object, key);
    if (value.type() != Json::Type::String) {
        throw std::runtime_error("field is not a string: " + key);
    }
    return value.as_string();
}

std::int64_t number(const Json& value) {
    if (value.type() != Json::Type::Int) {
        throw std::runtime_error("value is not an integer");
    }
    return value.as_int();
}

std::vector<std::string> strings(const Json& value) {
    if (value.type() != Json::Type::Array) {
        throw std::runtime_error("value is not an array");
    }
    std::vector<std::string> result;
    for (const Json& item : value.as_array()) {
        if (item.type() != Json::Type::String) {
            throw std::runtime_error("array element is not a string");
        }
        result.push_back(item.as_string());
    }
    return result;
}

std::optional<std::int64_t> optional_number(const Json& object, const std::string& key) {
    const Json* value = object.find(key);
    if (!value || value->is_null()) {
        return std::nullopt;
    }
    if (value->type() != Json::Type::Int) {
        throw std::runtime_error("field is not an integer: " + key);
    }
    return value->as_int();
}

Json run_targets(const Json& input) {
    std::string op = text(input, "op");
    if (op == "all") {
        Json result;
        for (const auto& entry : aot_cpp::targets()) {
            result.insert(entry.first,
                          Json::object({{"triple", Json::string(entry.second.triple)},
                                        {"readonly_section", Json::string(entry.second.readonly_section)}}));
        }
        return result;
    }
    auto spec = aot_cpp::target_spec(text(input, "name"));
    if (!spec) {
        throw std::runtime_error("unsupported native target");
    }
    return Json::object({{"triple", Json::string(spec->triple)},
                         {"readonly_section", Json::string(spec->readonly_section)}});
}

Json location_list(const std::vector<aot_cpp::Location>& locations) {
    std::vector<Json> items;
    items.reserve(locations.size());
    for (const auto& location : locations) {
        items.push_back(aot_cpp::location_json(location));
    }
    return Json::array(std::move(items));
}

Json run_abi(const Json& input) {
    std::string op = text(input, "op");
    if (op == "bits") return Json::integer(aot_cpp::bits(text(input, "kind")));
    if (op == "llvm_type") return Json::string(aot_cpp::llvm_type(text(input, "kind")));
    if (op == "extension") return Json::string(aot_cpp::extension(text(input, "kind")));
    if (op == "parameter_type") return Json::string(aot_cpp::parameter_type(text(input, "kind")));
    if (op == "result_type") return Json::string(aot_cpp::result_type(text(input, "kind")));
    if (op == "return_register") {
        auto name = aot_cpp::return_register(text(input, "kind"));
        return name ? Json::string(*name) : Json::null();
    }
    if (op == "locations") return location_list(aot_cpp::locations(strings(field(input, "parameters"))));
    if (op == "uniform_locations") return location_list(aot_cpp::uniform_locations());
    if (op == "uniform_stack_end") return Json::integer(aot_cpp::uniform_stack_end());
    if (op == "uniform") return Json::boolean(aot_cpp::uniform(field(input, "contract")));
    if (op == "signature") {
        auto [result, parameters] = aot_cpp::signature(field(input, "contract"));
        std::vector<Json> items;
        for (const std::string& kind : parameters) {
            items.push_back(Json::string(kind));
        }
        return Json::object({{"result", Json::string(result)}, {"parameters", Json::array(std::move(items))}});
    }
    if (op == "validate_import_annotations") {
        aot_cpp::validate_import_annotations(field(input, "contracts"), field(input, "annotations"));
        return Json::null();
    }
    throw std::runtime_error("unknown abi op");
}

Json run_guest_regions(const Json& input) {
    std::string op = text(input, "op");
    if (op == "immutable_ranges") {
        std::vector<aot_cpp::GuestSegment> loads;
        for (const Json& segment : field(input, "loads").as_array()) {
            loads.push_back({number(field(segment, "vaddr")), number(field(segment, "memsz")),
                             number(field(segment, "flags"))});
        }
        const Json& relro = field(input, "relro");
        auto ranges = aot_cpp::immutable_ranges(loads, number(relro.as_array().at(0)), number(relro.as_array().at(1)));
        std::vector<Json> items;
        for (const auto& range : ranges) {
            items.push_back(Json::array({Json::integer(range.low), Json::integer(range.high)}));
        }
        return Json::array(std::move(items));
    }
    if (op == "available_bytes") {
        std::vector<aot_cpp::GuestRange> ranges;
        for (const Json& range : field(input, "ranges").as_array()) {
            ranges.push_back({number(range.as_array().at(0)), number(range.as_array().at(1))});
        }
        return Json::integer(aot_cpp::available_bytes(ranges, number(field(input, "address"))));
    }
    throw std::runtime_error("unknown guest_regions op");
}

Json run_guest_provider_calls(const Json& input) {
    std::string op = text(input, "op");
    if (op == "declarations") {
        std::vector<Json> items;
        for (const std::string& line : aot_cpp::guest_provider_declarations()) {
            items.push_back(Json::string(line));
        }
        return Json::array(std::move(items));
    }
    if (op == "calls") {
        std::vector<Json> items;
        for (const auto& entry : aot_cpp::guest_provider_calls()) {
            const aot_cpp::ProviderCall& call = entry.second;
            items.push_back(Json::array({Json::string(entry.first),
                                         Json::array({Json::integer(call.kind), Json::string(call.name),
                                                      Json::integer(call.arity),
                                                      Json::boolean(call.returns_value)})}));
        }
        return Json::array(std::move(items));
    }
    throw std::runtime_error("unknown guest_provider_calls op");
}

Json run_optimize(const Json& input) {
    auto result = aot_cpp::merge_constant_stores(text(input, "ir"));
    return Json::object({{"ir", Json::string(result.ir)}, {"count", Json::integer(result.count)}});
}

Json run_atomics(const Json& input) {
    std::string op = text(input, "op");
    if (op == "ordered_load") {
        return Json::string(aot_cpp::ordered_load(number(field(input, "bits")), text(input, "pointer"),
                                                  optional_number(input, "align")));
    }
    if (op == "ordered_store") {
        return Json::string(aot_cpp::ordered_store(number(field(input, "bits")), text(input, "value"),
                                                   text(input, "pointer"), optional_number(input, "align")));
    }
    if (op == "accumulator") {
        auto value = aot_cpp::accumulator(number(field(input, "size")));
        return Json::array({Json::string(value.name), Json::integer(value.bits), Json::integer(value.shift)});
    }
    throw std::runtime_error("unknown atomics op");
}

Json run_calls(const Json& input) {
    std::string op = text(input, "op");
    if (op != "structural_signature") {
        throw std::runtime_error("unknown calls op");
    }
    auto signature = aot_cpp::structural_signature(field(input, "contract"));
    std::vector<Json> parameters;
    for (const std::string& kind : signature.parameter_types) {
        parameters.push_back(Json::string(kind));
    }
    std::vector<Json> locations;
    for (const aot_cpp::Location& location : signature.locations) {
        std::vector<Json> slot = {Json::string(location.kind)};
        slot.push_back(location.kind == "register" ? Json::string(location.name) : Json::null());
        slot.push_back(location.kind == "stack" ? Json::integer(location.offset) : Json::null());
        slot.push_back(Json::integer(location.width));
        locations.push_back(Json::array(std::move(slot)));
    }
    Json register_name = signature.has_return_register ? Json::string(signature.return_register) : Json::null();
    return Json::array({Json::string(signature.result), Json::array(std::move(parameters)), register_name,
                        Json::array(std::move(locations))});
}

Json run_fma(const Json& input) {
    std::string op = text(input, "op");
    if (op == "parts") {
        auto parts = aot_cpp::fma_parts(text(input, "mnemonic"));
        if (!parts) {
            return Json::null();
        }
        std::vector<Json> items;
        for (const std::string& group : *parts) {
            items.push_back(Json::string(group));
        }
        return Json::array(std::move(items));
    }
    if (op == "vector_type") {
        return Json::string(aot_cpp::fma_vector_type(number(field(input, "bits")), text(input, "element")));
    }
    throw std::runtime_error("unknown fma op");
}

Json run_integer(const Json& input) {
    std::string op = text(input, "op");
    if (op != "condition") {
        throw std::runtime_error("unknown integer op");
    }
    return Json::string(aot_cpp::integer_condition(text(input, "mnemonic")));
}

Json run_register_values(const Json& input) {
    std::string op = text(input, "op");
    if (op != "low_prefix") {
        throw std::runtime_error("unknown register_values op");
    }
    return Json::integer(aot_cpp::low_prefix(number(field(input, "mask"))));
}

Json run_register_aliases(const Json& input) {
    std::string op = text(input, "op");
    if (op != "equivalents") {
        throw std::runtime_error("unknown register_aliases op");
    }
    std::vector<Json> items;
    for (const std::string& name : aot_cpp::register_equivalents(field(input, "state"), text(input, "name"))) {
        items.push_back(Json::string(name));
    }
    return Json::array(std::move(items));
}

Json run(const std::string& module, const Json& input) {
    if (module == "targets") return run_targets(input);
    if (module == "abi") return run_abi(input);
    if (module == "guest_regions") return run_guest_regions(input);
    if (module == "guest_provider_calls") return run_guest_provider_calls(input);
    if (module == "optimize") return run_optimize(input);
    if (module == "atomics") return run_atomics(input);
    if (module == "calls") return run_calls(input);
    if (module == "fma") return run_fma(input);
    if (module == "integer") return run_integer(input);
    if (module == "register_values") return run_register_values(input);
    if (module == "register_aliases") return run_register_aliases(input);
    throw std::runtime_error("unknown module: " + module);
}

std::vector<Json> read_cases(std::istream& input) {
    std::vector<Json> cases;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        std::string error;
        auto parsed = aot_cpp::parse_json(line, error);
        if (!parsed) {
            throw std::runtime_error("invalid bench input: " + error);
        }
        cases.push_back(*parsed);
    }
    return cases;
}

int bench(const std::string& module, int iterations) {
    std::vector<Json> cases = read_cases(std::cin);
    std::int64_t sink = 0;
    auto start = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        for (const Json& test : cases) {
            try {
                sink += static_cast<std::int64_t>(run(module, test).dump().size());
            } catch (const std::exception&) {
                sink += 1;
            }
        }
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    Json output;
    output.insert("microseconds", Json::integer(micros));
    output.insert("cases", Json::integer(static_cast<std::int64_t>(cases.size())));
    output.insert("iterations", Json::integer(iterations));
    output.insert("sink", Json::integer(sink));
    std::cout << output.dump() << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && !(argc == 4 && std::string(argv[2]) == "--bench")) {
        std::cerr << "usage: aot_cpp_difftest <module> [--bench <iterations>]\n";
        return 2;
    }
    std::string module = argv[1];
    if (argc == 4) {
        return bench(module, std::stoi(argv[3]));
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        std::string error;
        auto input = aot_cpp::parse_json(line, error);
        Json output;
        if (!input) {
            output = Json::object({{"error", Json::boolean(true)}});
        } else {
            try {
                output = run(module, *input);
            } catch (const std::exception&) {
                output = Json::object({{"error", Json::boolean(true)}});
            }
        }
        std::cout << output.dump() << '\n';
    }
    return 0;
}
