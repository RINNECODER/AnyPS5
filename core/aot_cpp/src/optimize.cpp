#include "aot_cpp/optimize.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "optimize_parse.hpp"

namespace aot_cpp {
namespace {

struct StoreRecord {
    std::size_t line_index = 0;
    std::int64_t offset = 0;
    std::vector<std::uint8_t> raw;
};

struct Pointer {
    std::string root;
    std::int64_t offset = 0;
    std::int64_t size = 0;
};

std::vector<std::string> split_lines(std::string_view ir) {
    std::vector<std::string> lines;
    std::string current;
    for (char character : ir) {
        if (character == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }
    return lines;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool accepts_target(std::string_view ir) {
    for (std::string_view triple : {"target triple = \"arm64-apple-", "target triple = \"aarch64-apple-",
                                    "target triple = \"arm64-unknown-linux-gnu",
                                    "target triple = \"aarch64-unknown-linux-gnu"}) {
        if (ir.find(triple) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

ConstantStoreResult merge_constant_stores(std::string_view ir) {
    if (!accepts_target(ir)) {
        throw OptimizeError("constant-store reconstruction requires little-endian Apple or Linux ARM64 IR");
    }
    if (ir.find("__aps5_aot_bytes_") != std::string_view::npos) {
        throw OptimizeError("constant-store reconstruction has already run");
    }

    std::vector<std::string> lines = split_lines(ir);
    std::vector<std::string> output;
    std::vector<std::string> constants;
    std::vector<std::string> pending;
    std::map<std::string, Pointer> pointers;
    std::vector<std::pair<std::string, std::vector<StoreRecord>>> groups;
    std::int64_t count = 0;

    auto group_for = [&groups](const std::string& root) -> std::vector<StoreRecord>& {
        for (auto& entry : groups) {
            if (entry.first == root) {
                return entry.second;
            }
        }
        groups.emplace_back(root, std::vector<StoreRecord>{});
        return groups.back().second;
    };

    auto flush = [&]() {
        std::vector<std::string> replacements;
        std::set<std::size_t> removed;
        for (auto& entry : groups) {
            const std::string& root = entry.first;
            std::vector<StoreRecord>& stores = entry.second;
            std::map<std::int64_t, std::uint8_t> memory;
            for (const StoreRecord& store : stores) {
                for (std::size_t index = 0; index < store.raw.size(); ++index) {
                    memory[store.offset + static_cast<std::int64_t>(index)] = store.raw[index];
                }
            }
            std::vector<std::pair<std::int64_t, std::int64_t>> spans;
            for (const auto& cell : memory) {
                if (!spans.empty() && spans.back().second == cell.first) {
                    spans.back().second += 1;
                } else {
                    spans.emplace_back(cell.first, cell.first + 1);
                }
            }
            for (const auto& span : spans) {
                std::int64_t begin = span.first;
                std::int64_t end = span.second;
                if (end - begin < 16) {
                    continue;
                }
                std::vector<std::size_t> contributing;
                for (std::size_t index = 0; index < stores.size(); ++index) {
                    const StoreRecord& store = stores[index];
                    if (begin <= store.offset &&
                        store.offset + static_cast<std::int64_t>(store.raw.size()) <= end) {
                        contributing.push_back(index);
                    }
                }
                if (contributing.size() < 2) {
                    continue;
                }
                std::string value;
                for (std::int64_t address = begin; address < end; ++address) {
                    char buffer[5];
                    std::snprintf(buffer, sizeof(buffer), "\\%02X", memory[address]);
                    value += buffer;
                }
                std::size_t size = static_cast<std::size_t>(end - begin);
                std::string name = "__aps5_aot_bytes_" + std::to_string(constants.size());
                constants.push_back("@" + name + " = private unnamed_addr constant [" + std::to_string(size) +
                                    " x i8] c\"" + value + "\", align 16");
                replacements.push_back("  %" + name + "_dest = getelementptr i8, ptr " + root + ", i64 " +
                                       std::to_string(begin));
                replacements.push_back("  call void @llvm.memcpy.p0.p0.i64(ptr %" + name + "_dest, ptr @" + name +
                                       ", i64 " + std::to_string(size) + ", i1 false)");
                for (std::size_t index : contributing) {
                    removed.insert(stores[index].line_index);
                }
                ++count;
            }
        }
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (!removed.count(index)) {
                output.push_back(pending[index]);
            }
        }
        output.insert(output.end(), replacements.begin(), replacements.end());
        pending.clear();
        groups.clear();
    };

    for (const std::string& line : lines) {
        if (auto allocation = detail::parse_allocation(line)) {
            flush();
            pointers[allocation->name] = {allocation->name, 0, allocation->size};
            output.push_back(line);
        } else if (auto gep = detail::parse_gep(line); gep && pointers.count(gep->base)) {
            const Pointer base = pointers[gep->base];
            pointers[gep->name] = {base.root, base.offset + gep->delta, base.size};
            pending.push_back(line);
        } else if (auto store = detail::parse_store(line); store && pointers.count(store->pointer)) {
            const Pointer pointer = pointers[store->pointer];
            std::int64_t width = store->width_bits / 8;
            if (!(0 <= pointer.offset && pointer.offset <= pointer.size - width)) {
                flush();
                output.push_back(line);
                continue;
            }
            std::uint64_t value = static_cast<std::uint64_t>(store->value);
            std::vector<std::uint8_t> raw(static_cast<std::size_t>(width));
            for (std::int64_t index = 0; index < width; ++index) {
                raw[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value >> (8 * index));
            }
            group_for(pointer.root).push_back({pending.size(), pointer.offset, std::move(raw)});
            pending.push_back(line);
        } else {
            flush();
            output.push_back(line);
            if (starts_with(line, "define ") || line == "}") {
                pointers.clear();
            }
        }
    }
    flush();

    if (count) {
        bool declared = false;
        for (const std::string& line : lines) {
            if (starts_with(line, "declare void @llvm.memcpy.p0.p0.i64(")) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            output.push_back(
                "declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, ptr noalias nocapture "
                "readonly, i64, i1 immarg)");
        }
        output.insert(output.end(), constants.begin(), constants.end());
    }

    std::string joined;
    for (std::size_t index = 0; index < output.size(); ++index) {
        if (index) joined.push_back('\n');
        joined += output[index];
    }
    joined.push_back('\n');
    return {joined, count};
}

}  // namespace aot_cpp
