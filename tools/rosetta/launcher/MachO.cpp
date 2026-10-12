#include "MachO.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Aps5Launcher {
namespace {

constexpr std::uint32_t kMagic64 = 0xFEEDFACF;
constexpr std::uint32_t kCpuX86_64 = 0x01000007;
constexpr std::uint32_t kSegment64 = 0x19;
constexpr std::uint32_t kSymtab = 0x2;
constexpr std::uint32_t kDysymtab = 0xB;
constexpr std::uint32_t kLoadDylib = 0xC;
constexpr std::uint32_t kIdDylib = 0xD;
constexpr std::uint32_t kUuid = 0x1B;
constexpr std::uint32_t kLazyLoadDylib = 0x20;
constexpr std::uint32_t kDyldInfo = 0x22;
constexpr std::uint32_t kDyldInfoOnly = 0x80000022;
constexpr std::uint32_t kLoadWeakDylib = 0x80000018;
constexpr std::uint32_t kReexportDylib = 0x8000001F;
constexpr std::uint32_t kLoadUpwardDylib = 0x80000023;
constexpr std::uint32_t kExportsTrie = 0x80000033;
constexpr std::uint32_t kBuildVersion = 0x32;

[[noreturn]] void Malformed(const std::string& what) { throw std::runtime_error("malformed Mach-O image: " + what); }

std::uint32_t U32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) Malformed("truncated load command");
    std::uint32_t value;
    std::memcpy(&value, data.data() + offset, 4);
    return value;
}

std::uint64_t Uleb(const std::uint8_t* data, std::size_t end, std::size_t& i) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    while (true) {
        if (i >= end || shift > 63) Malformed("bad ULEB128");
        const std::uint8_t byte = data[i++];
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        shift += 7;
        if (byte < 0x80) return value;
    }
}

std::string CString(const std::uint8_t* data, std::size_t end, std::size_t& i) {
    const std::size_t start = i;
    while (i < end && data[i] != 0) ++i;
    if (i >= end) Malformed("unterminated string");
    std::string value(reinterpret_cast<const char*>(data + start), i - start);
    ++i;
    return value;
}

// Symbol names in classic bind opcodes (bind, weak bind and lazy bind streams).
void ParseBinds(const std::vector<std::uint8_t>& data, std::uint32_t offset, std::uint32_t size, std::set<std::string>& out) {
    if (size == 0) return;
    if (offset > data.size() || data.size() - offset < size) Malformed("bind opcodes outside the file");
    const std::uint8_t* bytes = data.data();
    std::size_t i = offset;
    const std::size_t end = static_cast<std::size_t>(offset) + size;
    while (i < end) {
        const std::uint8_t opcode = bytes[i++] & 0xF0;
        switch (opcode) {
        case 0x40:  // SET_SYMBOL_TRAILING_FLAGS_IMM
            out.insert(CString(bytes, end, i));
            break;
        case 0x20:  // SET_DYLIB_ORDINAL_ULEB
        case 0x60:  // SET_ADDEND_SLEB (same encoding length)
        case 0x70:  // SET_SEGMENT_AND_OFFSET_ULEB
        case 0x80:  // ADD_ADDR_ULEB
        case 0xA0:  // DO_BIND_ADD_ADDR_ULEB
            Uleb(bytes, end, i);
            break;
        case 0xC0:  // DO_BIND_ULEB_TIMES_SKIPPING_ULEB
            Uleb(bytes, end, i);
            Uleb(bytes, end, i);
            break;
        default:  // DONE, *_IMM, DO_BIND, DO_BIND_ADD_ADDR_IMM_SCALED: no operand
            break;
        }
    }
}

std::size_t UlebSize(std::uint64_t value) {
    std::size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

class Buffer {
public:
    void U8(std::uint8_t value) { bytes.push_back(value); }
    void U16(std::uint16_t value) { Raw(&value, 2); }
    void U32(std::uint32_t value) { Raw(&value, 4); }
    void U64(std::uint64_t value) { Raw(&value, 8); }
    void Uleb(std::uint64_t value) {
        do {
            std::uint8_t byte = value & 0x7F;
            value >>= 7;
            if (value != 0) byte |= 0x80;
            bytes.push_back(byte);
        } while (value != 0);
    }
    void Str(std::string_view value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
        bytes.push_back(0);
    }
    void Name16(std::string_view value) {
        char name[16] = {};
        std::memcpy(name, value.data(), std::min<std::size_t>(value.size(), 16));
        Raw(name, 16);
    }
    void Raw(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void Align(std::size_t alignment) {
        while (bytes.size() % alignment != 0) bytes.push_back(0);
    }
    void PadTo(std::size_t size) {
        if (bytes.size() > size) throw std::logic_error("stub dylib layout overflow");
        bytes.resize(size, 0);
    }
    std::size_t Size() const { return bytes.size(); }
    std::vector<std::uint8_t> bytes;
};

constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

struct TrieNode {
    bool terminal = false;
    std::uint64_t address = 0;
    std::vector<std::pair<std::string, std::unique_ptr<TrieNode>>> children;
    std::uint64_t offset = 0;
};

void TrieInsert(TrieNode* node, std::string_view name, std::uint64_t address) {
    while (true) {
        if (name.empty()) {
            if (node->terminal) throw std::invalid_argument("duplicate export");
            node->terminal = true;
            node->address = address;
            return;
        }
        TrieNode* next = nullptr;
        for (auto& [edge, child] : node->children) {
            std::size_t common = 0;
            while (common < edge.size() && common < name.size() && edge[common] == name[common]) ++common;
            if (common == 0) continue;
            if (common < edge.size()) {
                // Split the edge: node -edge[0,common)-> middle -edge[common,)-> child.
                auto middle = std::make_unique<TrieNode>();
                middle->children.emplace_back(edge.substr(common), std::move(child));
                edge.resize(common);
                child = std::move(middle);
            }
            next = child.get();
            name.remove_prefix(common);
            break;
        }
        if (next == nullptr) {
            auto leaf = std::make_unique<TrieNode>();
            leaf->terminal = true;
            leaf->address = address;
            node->children.emplace_back(std::string(name), std::move(leaf));
            return;
        }
        node = next;
    }
}

void TrieCollect(TrieNode* node, std::vector<TrieNode*>& order) {
    order.push_back(node);
    for (auto& [edge, child] : node->children) TrieCollect(child.get(), order);
}

std::size_t TerminalInfoSize(const TrieNode* node) { return node->terminal ? 1 + UlebSize(node->address) : 0; }

}  // namespace

std::set<std::string> ParseExportTrie(const std::uint8_t* trie, std::size_t size) {
    std::set<std::string> names;
    if (size == 0) return names;
    std::vector<std::pair<std::size_t, std::string>> pending{{0, std::string()}};
    std::size_t visited = 0;
    while (!pending.empty()) {
        auto [offset, prefix] = std::move(pending.back());
        pending.pop_back();
        if (offset >= size || ++visited > size) Malformed("export trie");
        std::size_t i = offset;
        const std::uint64_t terminal = Uleb(trie, size, i);
        if (terminal > size - i) Malformed("export trie terminal");
        if (terminal != 0) names.insert(prefix);
        i += terminal;
        if (i >= size) Malformed("export trie children");
        const unsigned count = trie[i++];
        for (unsigned child = 0; child < count; ++child) {
            std::string edge = CString(trie, size, i);
            const std::uint64_t next = Uleb(trie, size, i);
            pending.emplace_back(static_cast<std::size_t>(next), prefix + edge);
        }
    }
    return names;
}

MachOImage ReadMachO(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot read " + path);
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (data.size() < 32 || U32(data, 0) != kMagic64 || U32(data, 4) != kCpuX86_64)
        throw std::runtime_error("not a thin x86_64 Mach-O image: " + path);
    MachOImage image;
    const std::uint32_t count = U32(data, 16);
    std::size_t offset = 32;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint32_t command = U32(data, offset);
        const std::uint32_t size = U32(data, offset + 4);
        if (size < 8 || offset + size > data.size()) Malformed("load command size");
        switch (command) {
        case kDyldInfo:
        case kDyldInfoOnly: {
            ParseBinds(data, U32(data, offset + 16), U32(data, offset + 20), image.binds);
            ParseBinds(data, U32(data, offset + 24), U32(data, offset + 28), image.binds);
            ParseBinds(data, U32(data, offset + 32), U32(data, offset + 36), image.binds);
            const std::uint32_t exportOffset = U32(data, offset + 40), exportSize = U32(data, offset + 44);
            if (exportOffset > data.size() || data.size() - exportOffset < exportSize) Malformed("export trie outside the file");
            image.exports.merge(ParseExportTrie(data.data() + exportOffset, exportSize));
            break;
        }
        case kExportsTrie: {
            const std::uint32_t trieOffset = U32(data, offset + 8), trieSize = U32(data, offset + 12);
            if (trieOffset > data.size() || data.size() - trieOffset < trieSize) Malformed("export trie outside the file");
            image.exports.merge(ParseExportTrie(data.data() + trieOffset, trieSize));
            break;
        }
        case kLoadDylib:
        case kLoadWeakDylib:
        case kReexportDylib:
        case kLazyLoadDylib:
        case kLoadUpwardDylib: {
            std::size_t name = offset + U32(data, offset + 8);
            if (name >= offset + size) Malformed("dylib name");
            image.dylibs.push_back(CString(data.data(), offset + size, name));
            break;
        }
        default:
            break;
        }
        offset += size;
    }
    return image;
}

std::vector<std::uint8_t> BuildExportTrie(std::vector<ExportEntry> entries) {
    std::sort(entries.begin(), entries.end(), [](const ExportEntry& a, const ExportEntry& b) { return a.name < b.name; });
    TrieNode root;
    for (const auto& entry : entries) {
        if (entry.name.empty()) throw std::invalid_argument("empty export name");
        TrieInsert(&root, entry.name, entry.address);
    }
    std::vector<TrieNode*> order;
    TrieCollect(&root, order);
    // Child offsets are ULEB128, so node sizes depend on later offsets: iterate to a fixed point.
    for (bool changed = true; changed;) {
        changed = false;
        std::uint64_t offset = 0;
        for (TrieNode* node : order) {
            if (node->offset != offset) changed = true;
            node->offset = offset;
            const std::size_t info = TerminalInfoSize(node);
            offset += UlebSize(info) + info + 1;
            if (node->children.size() > 255) throw std::invalid_argument("export trie node fan-out");
            for (const auto& [edge, child] : node->children) offset += edge.size() + 1 + UlebSize(child->offset);
        }
    }
    Buffer out;
    for (const TrieNode* node : order) {
        const std::size_t info = TerminalInfoSize(node);
        out.Uleb(info);
        if (node->terminal) {
            out.Uleb(0);  // flags: regular symbol
            out.Uleb(node->address);
        }
        out.U8(static_cast<std::uint8_t>(node->children.size()));
        for (const auto& [edge, child] : node->children) {
            out.Str(edge);
            out.Uleb(child->offset);
        }
    }
    out.Align(8);
    return out.bytes;
}

std::vector<std::uint8_t> BuildStubDylib(const StubDylib& spec) {
    const bool tables = !spec.runtimePath.empty();
    const std::size_t stubCount = spec.stubs.size(), tracedCount = spec.traced.size();
    if (spec.stubLabels.size() != stubCount || spec.tracedLabels.size() != tracedCount)
        throw std::invalid_argument("stub labels do not match the stubs");
    if (!tables && stubCount + tracedCount != 0) throw std::invalid_argument("stubs need the stub runtime");

    const std::uint32_t idSize = static_cast<std::uint32_t>(AlignUp(24 + spec.installName.size() + 1, 8));
    const std::uint32_t runtimeSize = tables ? static_cast<std::uint32_t>(AlignUp(24 + spec.runtimePath.size() + 1, 8)) : 0;
    const std::uint32_t commandsSize = (72 + 2 * 80) + (72 + 80) + 72 + idSize + 48 + 24 + 80 + runtimeSize + 24 + 24;
    const std::uint32_t commandCount = tables ? 10 : 9;

    // __TEXT: header, load commands, __text (16 bytes per stub), __cstring.
    const std::uint64_t textStart = AlignUp(32 + commandsSize, 16);
    const std::uint64_t codeSize = std::max<std::uint64_t>(16, 16 * (stubCount + tracedCount));
    Buffer cstrings;
    std::vector<std::uint64_t> labelOffsets, tracedLabelOffsets, tracedSymbolOffsets;
    for (const auto& label : spec.stubLabels) labelOffsets.push_back(cstrings.Size()), cstrings.Str(label);
    for (std::size_t i = 0; i < tracedCount; ++i) {
        tracedLabelOffsets.push_back(cstrings.Size());
        cstrings.Str(spec.tracedLabels[i]);
        tracedSymbolOffsets.push_back(cstrings.Size());
        cstrings.Str(spec.traced[i]);
    }
    if (cstrings.Size() == 0) cstrings.U8(0);
    const std::uint64_t cstringStart = textStart + codeSize;
    const std::uint64_t textSize = AlignUp(cstringStart + cstrings.Size(), 4096);

    // __DATA: the three name tables (zero-terminated), the two counts, then two
    // pointers bound to _aps5_stub_common and _aps5_trace_common in the runtime.
    const std::uint64_t dataStart = textSize;
    const std::uint64_t stubNames = 0, traceNames = stubNames + 8 * (stubCount + 1);
    const std::uint64_t traceSymbols = traceNames + 8 * (tracedCount + 1);
    const std::uint64_t counts = traceSymbols + 8 * (tracedCount + 1);
    const std::uint64_t got = counts + 8;
    const std::uint64_t dataUsed = tables ? got + 16 : 8;
    const std::uint64_t dataSize = AlignUp(dataUsed, 4096);
    Buffer data;
    if (tables) {
        for (auto offset : labelOffsets) data.U64(cstringStart + offset);
        data.U64(0);
        for (auto offset : tracedLabelOffsets) data.U64(cstringStart + offset);
        data.U64(0);
        for (auto offset : tracedSymbolOffsets) data.U64(cstringStart + offset);
        data.U64(0);
        data.U32(static_cast<std::uint32_t>(stubCount));
        data.U32(static_cast<std::uint32_t>(tracedCount));
        data.U64(0);
        data.U64(0);
    }
    data.PadTo(dataSize);

    Buffer code;
    for (std::size_t i = 0; i < stubCount + tracedCount; ++i) {
        const bool traced = i >= stubCount;
        const std::uint64_t address = textStart + 16 * i;
        std::uint64_t jump;
        if (traced) {
            code.U8(0x41), code.U8(0xBB), code.U32(static_cast<std::uint32_t>(i - stubCount));  // movl $index, %r11d
            jump = address + 6;
        } else {
            code.U8(0xB8), code.U32(static_cast<std::uint32_t>(i));  // movl $index, %eax
            jump = address + 5;
        }
        const std::uint64_t slot = dataStart + got + (traced ? 8 : 0);
        code.U8(0xFF), code.U8(0x25);  // jmp *slot(%rip)
        code.U32(static_cast<std::uint32_t>(static_cast<std::int64_t>(slot) - static_cast<std::int64_t>(jump + 6)));
        while (code.Size() % 16 != 0) code.U8(0xCC);
    }
    if (code.Size() == 0) code.U8(0xC3);
    code.PadTo(codeSize);

    // __LINKEDIT: rebase, bind, export trie, symbol table, string table.
    const std::uint64_t linkeditStart = textSize + dataSize;
    Buffer rebase, bind;
    if (tables) {
        rebase.U8(0x11);  // SET_TYPE_IMM pointer
        const std::pair<std::uint64_t, std::size_t> runs[] = {{stubNames, stubCount}, {traceNames, tracedCount}, {traceSymbols, tracedCount}};
        for (const auto& [start, length] : runs) {
            if (length == 0) continue;
            rebase.U8(0x21);  // SET_SEGMENT_AND_OFFSET_ULEB, segment 1 (__DATA)
            rebase.Uleb(start);
            rebase.U8(0x60);  // DO_REBASE_ULEB_TIMES
            rebase.Uleb(length);
        }
        bind.U8(0x11);  // SET_DYLIB_ORDINAL_IMM 1: the stub runtime
        bind.U8(0x51);  // SET_TYPE_IMM pointer
        const char* targets[] = {"_aps5_stub_common", "_aps5_trace_common"};
        for (int slot = 0; slot < 2; ++slot) {
            bind.U8(0x40);
            bind.Str(targets[slot]);
            bind.U8(0x71);  // SET_SEGMENT_AND_OFFSET_ULEB, segment 1
            bind.Uleb(got + 8 * slot);
            bind.U8(0x90);  // DO_BIND
        }
    }
    rebase.U8(0x00), rebase.Align(8);
    bind.U8(0x00), bind.Align(8);

    std::vector<ExportEntry> exports;
    for (std::size_t i = 0; i < stubCount; ++i) exports.push_back({spec.stubs[i], textStart + 16 * i});
    for (std::size_t i = 0; i < tracedCount; ++i) exports.push_back({spec.traced[i], textStart + 16 * (stubCount + i)});
    if (tables) {
        exports.push_back({"_aps5_stub_names", dataStart + stubNames});
        exports.push_back({"_aps5_trace_names", dataStart + traceNames});
        exports.push_back({"_aps5_trace_symbols", dataStart + traceSymbols});
        exports.push_back({"_aps5_stub_count", dataStart + counts});
        exports.push_back({"_aps5_trace_count", dataStart + counts + 4});
    }
    const std::vector<std::uint8_t> trie = exports.empty() ? std::vector<std::uint8_t>{0, 0, 0, 0, 0, 0, 0, 0} : BuildExportTrie(exports);

    std::sort(exports.begin(), exports.end(), [](const ExportEntry& a, const ExportEntry& b) { return a.name < b.name; });
    Buffer symbols, strings;
    strings.U8(' '), strings.U8(0);
    for (const auto& entry : exports) {
        symbols.U32(static_cast<std::uint32_t>(strings.Size()));
        strings.Str(entry.name);
        symbols.U8(0x0F);  // N_SECT | N_EXT
        symbols.U8(entry.address >= dataStart ? 3 : 1);
        symbols.U16(0);
        symbols.U64(entry.address);
    }
    const std::size_t undefinedCount = tables ? 2 : 0;
    if (tables) {
        for (const char* name : {"_aps5_stub_common", "_aps5_trace_common"}) {
            symbols.U32(static_cast<std::uint32_t>(strings.Size()));
            strings.Str(name);
            symbols.U8(0x01);  // N_UNDF | N_EXT
            symbols.U8(0);
            symbols.U16(1 << 8);  // library ordinal 1
            symbols.U64(0);
        }
    }
    strings.Align(8);

    const std::uint64_t rebaseOffset = linkeditStart;
    const std::uint64_t bindOffset = rebaseOffset + rebase.Size();
    const std::uint64_t trieOffset = bindOffset + bind.Size();
    const std::uint64_t symbolOffset = trieOffset + trie.size();
    const std::uint64_t stringOffset = symbolOffset + symbols.Size();
    const std::uint64_t linkeditSize = stringOffset + strings.Size() - linkeditStart;

    Buffer out;
    out.U32(kMagic64), out.U32(kCpuX86_64), out.U32(3), out.U32(6 /* MH_DYLIB */);
    out.U32(commandCount), out.U32(commandsSize);
    out.U32(0x4 | 0x80 | 0x100000 | (tables ? 0 : 0x1));  // DYLDLINK | TWOLEVEL | NO_REEXPORTED_DYLIBS [| NOUNDEFS]
    out.U32(0);

    auto segment = [&](const char* name, std::uint64_t address, std::uint64_t vmSize, std::uint64_t fileOffset,
                       std::uint64_t fileSize, std::uint32_t protection, std::uint32_t sections) {
        out.U32(kSegment64), out.U32(72 + 80 * sections);
        out.Name16(name);
        out.U64(address), out.U64(vmSize), out.U64(fileOffset), out.U64(fileSize);
        out.U32(protection), out.U32(protection), out.U32(sections), out.U32(0);
    };
    auto section = [&](const char* name, const char* segmentName, std::uint64_t address, std::uint64_t size,
                       std::uint32_t alignment, std::uint32_t flags) {
        out.Name16(name), out.Name16(segmentName);
        out.U64(address), out.U64(size);
        out.U32(static_cast<std::uint32_t>(address)), out.U32(alignment);
        out.U32(0), out.U32(0), out.U32(flags), out.U32(0), out.U32(0), out.U32(0);
    };
    segment("__TEXT", 0, textSize, 0, textSize, 5, 2);
    section("__text", "__TEXT", textStart, codeSize, 4, 0x80000400);
    section("__cstring", "__TEXT", cstringStart, cstrings.Size(), 0, 0x2);
    segment("__DATA", dataStart, dataSize, dataStart, dataSize, 3, 1);
    section("__data", "__DATA", dataStart, dataUsed, 3, 0);
    segment("__LINKEDIT", linkeditStart, AlignUp(linkeditSize, 4096), linkeditStart, linkeditSize, 1, 0);

    auto dylibCommand = [&](std::uint32_t command, std::uint32_t size, const std::string& name) {
        const std::size_t start = out.Size();
        out.U32(command), out.U32(size), out.U32(24), out.U32(2), out.U32(0x10000), out.U32(0x10000);
        out.Str(name);
        out.PadTo(start + size);
    };
    dylibCommand(kIdDylib, idSize, spec.installName);
    out.U32(kDyldInfoOnly), out.U32(48);
    out.U32(static_cast<std::uint32_t>(rebaseOffset)), out.U32(static_cast<std::uint32_t>(rebase.Size()));
    out.U32(static_cast<std::uint32_t>(bindOffset)), out.U32(static_cast<std::uint32_t>(bind.Size()));
    out.U32(0), out.U32(0), out.U32(0), out.U32(0);
    out.U32(static_cast<std::uint32_t>(trieOffset)), out.U32(static_cast<std::uint32_t>(trie.size()));
    out.U32(kSymtab), out.U32(24);
    out.U32(static_cast<std::uint32_t>(symbolOffset)), out.U32(static_cast<std::uint32_t>(exports.size() + undefinedCount));
    out.U32(static_cast<std::uint32_t>(stringOffset)), out.U32(static_cast<std::uint32_t>(strings.Size()));
    out.U32(kDysymtab), out.U32(80);
    out.U32(0), out.U32(0);  // locals
    out.U32(0), out.U32(static_cast<std::uint32_t>(exports.size()));  // external definitions
    out.U32(static_cast<std::uint32_t>(exports.size())), out.U32(static_cast<std::uint32_t>(undefinedCount));
    for (int field = 0; field < 12; ++field) out.U32(0);
    if (tables) dylibCommand(kLoadDylib, runtimeSize, spec.runtimePath);
    out.U32(kBuildVersion), out.U32(24), out.U32(1 /* macOS */), out.U32(0x000B0000), out.U32(0x000B0000), out.U32(0);
    // A UUID derived from the content (FNV-1a), so identical inputs give identical bytes.
    std::uint64_t hash[2] = {0xcbf29ce484222325ull, 0x84222325cbf29ce4ull};
    auto mix = [&](std::string_view text) {
        for (unsigned char c : text) {
            hash[0] = (hash[0] ^ c) * 0x100000001b3ull;
            hash[1] = (hash[1] ^ c) * 0x100000001b3ull + 1;
        }
    };
    mix(spec.installName), mix(spec.runtimePath);
    for (const auto& name : spec.stubs) mix(name);
    for (const auto& name : spec.traced) mix(name);
    std::uint8_t uuid[16];
    std::memcpy(uuid, hash, 16);
    uuid[6] = (uuid[6] & 0x0F) | 0x40, uuid[8] = (uuid[8] & 0x3F) | 0x80;
    out.U32(kUuid), out.U32(24), out.Raw(uuid, 16);
    if (out.Size() != 32 + commandsSize) throw std::logic_error("stub dylib load command size");

    out.PadTo(textStart);
    out.Raw(code.bytes.data(), code.Size());
    out.Raw(cstrings.bytes.data(), cstrings.Size());
    out.PadTo(textSize);
    out.Raw(data.bytes.data(), data.Size());
    out.Raw(rebase.bytes.data(), rebase.Size());
    out.Raw(bind.bytes.data(), bind.Size());
    out.Raw(trie.data(), trie.size());
    out.Raw(symbols.bytes.data(), symbols.Size());
    out.Raw(strings.bytes.data(), strings.Size());
    return out.bytes;
}

}  // namespace Aps5Launcher
