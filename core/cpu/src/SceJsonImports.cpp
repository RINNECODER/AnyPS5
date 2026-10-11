#include <cpu/SceJsonImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Ported from upstream boykopovar/AnyPS5 core/libs/prx/libSceJson2/Export.cpp. Upstream
// keeps the tree in host memory behind host pointers; here the same structures live in
// a provider-owned guest heap (see SceJsonImports.hpp) and are reached through Machine.
// The parser and serialiser are upstream's, run on a host copy of the tree.

namespace Cpu {
namespace {

enum ValueType : std::int32_t {
    TypeNull = 0, TypeBoolean = 1, TypeInteger = 2, TypeUInteger = 3,
    TypeReal = 4, TypeString = 5, TypeArray = 6, TypeObject = 7,
};

constexpr std::uint32_t JsonErrorParse = 0x80920101;
constexpr std::uint64_t Zero = 0;

// Guest layouts.
constexpr std::uint64_t NodeSize = 0x38, NodeBoolean = 4, NodeInteger = 8, NodeUInteger = 0x10, NodeReal = 0x18,
    NodeString = 0x20, NodeArray = 0x28, NodeObject = 0x30;
constexpr std::uint64_t StringText = 0x10;
constexpr std::uint64_t ListSize = 0, ListHead = 8, ListTail = 0x10, ListHeaderSize = 0x18;
constexpr std::uint64_t ElementNext = 0, ElementPrev = 8, ArrayElementValue = 0x10, ArrayElementSize = 0x18;
constexpr std::uint64_t PairKey = 0x10, PairValue = 0x20, PairElementSize = 0x28;
constexpr std::size_t MaxGuestText = 64u << 20;

constexpr std::uint64_t HeapChunk = 1u << 20;
constexpr std::uint64_t HeapReserve = 512ull << 20;

// Host copy of a guest tree, for upstream's parser and serialiser.
struct HostNode {
    ValueType type = TypeNull;
    bool boolean = false;
    std::int64_t integer = 0;
    std::uint64_t uinteger = 0;
    double real = 0;
    std::string string;
    std::vector<HostNode> items;
    std::vector<std::pair<std::string, HostNode>> members;
};

void SetType(HostNode& n, ValueType type) {
    n = HostNode{};
    n.type = type;
}

// --- upstream serialiser -------------------------------------------------------------
void AppendEscaped(std::string& out, const std::string& text) {
    out += '"';
    for (const unsigned char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                    out += buffer;
                } else out += static_cast<char>(c);
        }
    }
    out += '"';
}

std::string RealText(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("sce::Json: cannot serialize a non-finite number");
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

void Serialize(std::string& out, const HostNode& n) {
    switch (n.type) {
        case TypeNull: out += "null"; return;
        case TypeBoolean: out += n.boolean ? "true" : "false"; return;
        case TypeInteger: out += std::to_string(n.integer); return;
        case TypeUInteger: out += std::to_string(n.uinteger); return;
        case TypeReal: out += RealText(n.real); return;
        case TypeString: AppendEscaped(out, n.string); return;
        case TypeArray: {
            out += '[';
            bool first = true;
            for (const auto& item : n.items) {
                if (!first) out += ',';
                first = false;
                Serialize(out, item);
            }
            out += ']';
            return;
        }
        case TypeObject: {
            out += '{';
            bool first = true;
            for (const auto& [key, value] : n.members) {
                if (!first) out += ',';
                first = false;
                AppendEscaped(out, key);
                out += ':';
                Serialize(out, value);
            }
            out += '}';
            return;
        }
    }
    throw std::runtime_error("sce::Json: invalid value type");
}

// --- upstream parser -----------------------------------------------------------------
class Parser {
public:
    Parser(const char* text, std::size_t size) : _text(text), _end(text + size) {}

    bool Parse(HostNode& out) {
        skip();
        if (!value(out)) return false;
        skip();
        return _text == _end || *_text == '\0';
    }

private:
    static constexpr std::size_t MaxNestingDepth = 512;

    const char* _text;
    const char* _end;
    std::size_t _depth = 0;

    void skip() { while (_text < _end && (*_text == ' ' || *_text == '\t' || *_text == '\n' || *_text == '\r')) ++_text; }
    bool literal(const char* word) {
        const auto length = std::strlen(word);
        if (static_cast<std::size_t>(_end - _text) < length || std::memcmp(_text, word, length) != 0) return false;
        _text += length;
        return true;
    }
    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3f)); }
        else if (code < 0x10000) { out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f)); }
        else { out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3f)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f)); }
    }
    bool hex4(std::uint32_t& code) {
        if (_end - _text < 4) return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *_text++;
            code <<= 4;
            if (c >= '0' && c <= '9') code |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') code |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') code |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (_text >= _end || *_text != '"') return false;
        ++_text;
        while (_text < _end && *_text != '"') {
            const char c = *_text++;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (_text >= _end) return false;
            switch (*_text++) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t code = 0;
                    if (!hex4(code)) return false;
                    if (code >= 0xd800 && code < 0xdc00) {
                        std::uint32_t low = 0;
                        if (!literal("\\u") || !hex4(low) || low < 0xdc00 || low >= 0xe000) return false;
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else if (code >= 0xdc00 && code < 0xe000) return false;
                    appendUtf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        if (_text >= _end) return false;
        ++_text;
        return true;
    }
    bool number(HostNode& out) {
        const char* start = _text;
        if (_text < _end && *_text == '-') ++_text;
        if (_text >= _end || *_text < '0' || *_text > '9') return false;
        if (*_text == '0') ++_text;
        else while (_text < _end && *_text >= '0' && *_text <= '9') ++_text;
        bool integral = true;
        if (_text < _end && *_text == '.') {
            integral = false;
            ++_text;
            if (_text >= _end || *_text < '0' || *_text > '9') return false;
            while (_text < _end && *_text >= '0' && *_text <= '9') ++_text;
        }
        if (_text < _end && (*_text == 'e' || *_text == 'E')) {
            integral = false;
            ++_text;
            if (_text < _end && (*_text == '+' || *_text == '-')) ++_text;
            if (_text >= _end || *_text < '0' || *_text > '9') return false;
            while (_text < _end && *_text >= '0' && *_text <= '9') ++_text;
        }
        const std::string text(start, _text);
        if (integral) {
            errno = 0;
            if (text[0] == '-') {
                const long long value = std::strtoll(text.c_str(), nullptr, 10);
                if (errno == 0) { SetType(out, TypeInteger); out.integer = value; return true; }
            } else {
                const unsigned long long value = std::strtoull(text.c_str(), nullptr, 10);
                if (errno == 0) {
                    if (value <= static_cast<unsigned long long>(INT64_MAX)) { SetType(out, TypeInteger); out.integer = static_cast<std::int64_t>(value); }
                    else { SetType(out, TypeUInteger); out.uinteger = value; }
                    return true;
                }
            }
        }
        const double real = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(real)) throw std::runtime_error("sce::Json::Parser::parse: number " + text + " overflows a double; the console's behaviour is unverified");
        SetType(out, TypeReal);
        out.real = real;
        return true;
    }
    bool value(HostNode& out) {
        if (_text >= _end) return false;
        switch (*_text) {
            case 'n': if (!literal("null")) return false; SetType(out, TypeNull); return true;
            case 't': if (!literal("true")) return false; SetType(out, TypeBoolean); out.boolean = true; return true;
            case 'f': if (!literal("false")) return false; SetType(out, TypeBoolean); return true;
            case '"': {
                std::string text;
                if (!string(text)) return false;
                SetType(out, TypeString);
                out.string = std::move(text);
                return true;
            }
            case '[': {
                if (_depth == MaxNestingDepth) return false;
                ++_text;
                ++_depth;
                SetType(out, TypeArray);
                skip();
                if (_text < _end && *_text == ']') { ++_text; --_depth; return true; }
                for (;;) {
                    out.items.emplace_back();
                    skip();
                    if (!value(out.items.back())) return false;
                    skip();
                    if (_text < _end && *_text == ',') { ++_text; continue; }
                    if (_text < _end && *_text == ']') { ++_text; --_depth; return true; }
                    return false;
                }
            }
            case '{': {
                if (_depth == MaxNestingDepth) return false;
                ++_text;
                ++_depth;
                SetType(out, TypeObject);
                skip();
                if (_text < _end && *_text == '}') { ++_text; --_depth; return true; }
                for (;;) {
                    skip();
                    std::string key;
                    if (!string(key)) return false;
                    skip();
                    if (_text >= _end || *_text != ':') return false;
                    ++_text;
                    skip();
                    out.members.emplace_back(std::move(key), HostNode{});
                    if (!value(out.members.back().second)) return false;
                    skip();
                    if (_text < _end && *_text == ',') { ++_text; continue; }
                    if (_text < _end && *_text == '}') { ++_text; --_depth; return true; }
                    return false;
                }
            }
            default:
                return number(out);
        }
    }
};

std::uint64_t argument(Machine& guest, unsigned index) {
    constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
    return guest.Get(registers.at(index));
}

}

struct SceJsonImports::Impl {
    using Handler = std::function<std::uint64_t(Impl&, Machine&)>;
    Machine& guest;
    std::shared_ptr<SceHostTrampolines> trampolines;
    std::map<std::string, std::pair<std::string_view, Handler>> functions;
    std::map<std::string, std::uint64_t> gates;
    std::uint64_t stubPage = 0, stubNext = 0;
    std::uint64_t heapBase = 0, heapMapped = 0, heapNext = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> live;
    std::map<std::uint64_t, std::vector<std::uint64_t>> freeBlocks;
    std::array<std::uint64_t, 8> defaults{};
    std::uint64_t allocator = 0, allocatorUserData = 0;

    // --- guest memory -------------------------------------------------------------
    std::uint64_t u64(std::uint64_t address) const {
        std::uint64_t value = 0;
        guest.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    void put(std::uint64_t address, std::uint64_t value) { guest.Write(address, std::as_bytes(std::span(&value, 1))); }
    std::int32_t i32(std::uint64_t address) const {
        std::int32_t value = 0;
        guest.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    void put32(std::uint64_t address, std::int32_t value) { guest.Write(address, std::as_bytes(std::span(&value, 1))); }
    void put8(std::uint64_t address, std::uint8_t value) { guest.Write(address, std::as_bytes(std::span(&value, 1))); }
    std::string bytes(std::uint64_t address, std::size_t size) const {
        if (size > MaxGuestText) throw std::runtime_error("sce::Json: guest text exceeds the host copy limit");
        std::string text(size, '\0');
        if (size) guest.Read(address, std::as_writable_bytes(std::span(text.data(), size)));
        return text;
    }
    std::string cString(std::uint64_t address, const char* what) const {
        if (!address) throw std::invalid_argument(std::string("sce::Json: null ") + what);
        std::string text;
        for (;;) {
            const std::size_t chunk = 0x1000 - (address & 0xfff);
            const auto piece = bytes(address, chunk);
            const auto end = piece.find('\0');
            text.append(piece, 0, end);
            if (end != std::string::npos) return text;
            if (text.size() > MaxGuestText) throw std::runtime_error("sce::Json: unterminated guest text");
            address += chunk;
        }
    }

    // --- provider heap ------------------------------------------------------------
    std::uint64_t alloc(std::uint64_t size) {
        size = size <= 64 ? (size + 15) & ~std::uint64_t{15} : std::bit_ceil(size);
        std::uint64_t address = 0;
        if (auto found = freeBlocks.find(size); found != freeBlocks.end() && !found->second.empty()) {
            address = found->second.back();
            found->second.pop_back();
        } else {
            if (heapNext + size > heapBase + HeapReserve) throw std::runtime_error("sce::Json provider heap exhausted");
            while (heapNext + size > heapBase + heapMapped) {
                guest.Map(heapBase + heapMapped, HeapChunk, Permission::Read | Permission::Write);
                heapMapped += HeapChunk;
            }
            address = heapNext;
            heapNext += size;
        }
        const std::vector<std::byte> zero(size);
        guest.Write(address, zero);
        live.emplace(address, size);
        return address;
    }
    void release(std::uint64_t address) {
        if (!address) return;
        const auto found = live.find(address);
        if (found == live.end()) throw std::runtime_error("sce::Json: freeing a block the provider heap does not own");
        freeBlocks[found->second].push_back(address);
        live.erase(found);
    }

    // --- String -------------------------------------------------------------------
    std::uint64_t newRep(const std::string& text) {
        const auto rep = alloc(StringText + text.size() + 1);
        put(rep, text.size());
        put(rep + 8, text.size());
        guest.Write(rep + StringText, std::as_bytes(std::span(text.data(), text.size())));
        return rep;
    }
    std::string readString(std::uint64_t slot) const {
        const auto rep = u64(slot);
        if (!rep) throw std::runtime_error("sce::Json::String used before construction");
        return bytes(rep + StringText, u64(rep));
    }
    void constructString(std::uint64_t slot, const std::string& text) { put(slot, newRep(text)); }
    void destroyString(std::uint64_t slot) { release(u64(slot)); put(slot, 0); }
    void assignString(std::uint64_t slot, const std::string& text) {
        const auto old = u64(slot);
        put(slot, newRep(text));
        release(old);
    }

    // --- lists --------------------------------------------------------------------
    std::vector<std::uint64_t> elements(std::uint64_t header) const {
        if (!header) throw std::runtime_error("sce::Json container used before construction");
        std::vector<std::uint64_t> out;
        for (auto element = u64(header + ListHead); element; element = u64(element + ElementNext)) out.push_back(element);
        return out;
    }
    std::uint64_t pushElement(std::uint64_t header, std::uint64_t size) {
        if (!header) throw std::runtime_error("sce::Json container used before construction");
        const auto element = alloc(size);
        const auto tail = u64(header + ListTail);
        put(element + ElementPrev, tail);
        if (tail) put(tail + ElementNext, element);
        else put(header + ListHead, element);
        put(header + ListTail, element);
        put(header + ListSize, u64(header + ListSize) + 1);
        return element;
    }
    void constructList(std::uint64_t slot) { put(slot, alloc(ListHeaderSize)); }
    void clearArrayItems(std::uint64_t header) {
        for (const auto element : elements(header)) {
            destroyValue(element + ArrayElementValue);
            release(element);
        }
        put(header + ListSize, 0); put(header + ListHead, 0); put(header + ListTail, 0);
    }
    void clearObjectItems(std::uint64_t header) {
        for (const auto element : elements(header)) {
            destroyString(element + PairKey);
            destroyValue(element + PairValue);
            release(element);
        }
        put(header + ListSize, 0); put(header + ListHead, 0); put(header + ListTail, 0);
    }
    void destroyArray(std::uint64_t slot) {
        const auto header = u64(slot);
        if (!header) return;
        clearArrayItems(header);
        release(header);
        put(slot, 0);
    }
    void destroyObject(std::uint64_t slot) {
        const auto header = u64(slot);
        if (!header) return;
        clearObjectItems(header);
        release(header);
        put(slot, 0);
    }
    void copyArray(std::uint64_t destination, std::uint64_t source) {
        const auto items = elements(u64(source));
        constructList(destination);
        for (const auto item : items)
            copyInto(pushElement(u64(destination), ArrayElementSize) + ArrayElementValue, item + ArrayElementValue);
    }
    void copyObject(std::uint64_t destination, std::uint64_t source) {
        const auto items = elements(u64(source));
        constructList(destination);
        for (const auto item : items) {
            const auto pair = pushElement(u64(destination), PairElementSize);
            constructString(pair + PairKey, readString(item + PairKey));
            copyInto(pair + PairValue, item + PairValue);
        }
    }

    // --- Value / Node -------------------------------------------------------------
    std::uint64_t nodeOf(std::uint64_t value) const {
        if (!value) throw std::invalid_argument("sce::Json: null Value");
        const auto node = u64(value);
        if (!node) throw std::runtime_error("sce::Json::Value used before construction");
        return node;
    }
    ValueType typeOf(std::uint64_t node) const { return static_cast<ValueType>(i32(node)); }
    void clearNode(std::uint64_t node) {
        const auto type = typeOf(node);
        if (type == TypeString) destroyString(node + NodeString);
        if (type == TypeArray) destroyArray(node + NodeArray);
        if (type == TypeObject) destroyObject(node + NodeObject);
        guest.Write(node, std::vector<std::byte>(NodeSize));
    }
    void construct(std::uint64_t value) { put(value, alloc(NodeSize)); }
    void destroyValue(std::uint64_t value) {
        const auto node = u64(value);
        if (!node) return;
        clearNode(node);
        release(node);
        put(value, 0);
    }
    void setType(std::uint64_t node, ValueType type) {
        if (type < TypeNull || type > TypeObject) throw std::invalid_argument("sce::Json::Value::set: invalid value type");
        clearNode(node);
        put32(node, type);
        if (type == TypeString) constructString(node + NodeString, std::string());
        if (type == TypeArray) constructList(node + NodeArray);
        if (type == TypeObject) constructList(node + NodeObject);
    }
    // Builds a deep copy of source in a fresh node, then swaps it into destination, so
    // assigning a value from one of its own descendants is safe (upstream AssignNode).
    void assignNode(std::uint64_t destination, std::uint64_t source) {
        const auto copy = alloc(NodeSize);
        const auto type = typeOf(source);
        guest.Write(copy, std::as_bytes(std::span(bytes(source, NodeString).data(), NodeString)));
        if (type == TypeString) constructString(copy + NodeString, readString(source + NodeString));
        if (type == TypeArray) copyArray(copy + NodeArray, source + NodeArray);
        if (type == TypeObject) copyObject(copy + NodeObject, source + NodeObject);
        clearNode(destination);
        const auto moved = bytes(copy, NodeSize);
        guest.Write(destination, std::as_bytes(std::span(moved.data(), moved.size())));
        release(copy);
    }
    void copyInto(std::uint64_t destination, std::uint64_t source) {
        const auto node = nodeOf(source);
        construct(destination);
        assignNode(u64(destination), node);
    }

    // Typed defaults returned by mismatched getters and missing keys (upstream Defaults
    // with no null-access callback registered).
    std::uint64_t defaultValue(ValueType type) {
        if (!defaults[0])
            for (int i = 0; i < 8; ++i) {
                const auto value = alloc(8);
                construct(value);
                setType(u64(value), static_cast<ValueType>(i));
                defaults[i] = value;
            }
        return defaults[type];
    }
    std::uint64_t typed(std::uint64_t value, ValueType requested) {
        const auto node = nodeOf(value);
        if (typeOf(node) == requested) return node;
        return u64(defaultValue(requested));
    }

    // --- host tree copies ---------------------------------------------------------
    HostNode readTree(std::uint64_t node) const {
        HostNode out;
        out.type = typeOf(node);
        std::uint8_t boolean = 0;
        guest.Read(node + NodeBoolean, std::as_writable_bytes(std::span(&boolean, 1)));
        out.boolean = boolean != 0;
        out.integer = static_cast<std::int64_t>(u64(node + NodeInteger));
        out.uinteger = u64(node + NodeUInteger);
        out.real = std::bit_cast<double>(u64(node + NodeReal));
        if (out.type == TypeString) out.string = readString(node + NodeString);
        if (out.type == TypeArray)
            for (const auto element : elements(u64(node + NodeArray))) out.items.push_back(readTree(nodeOf(element + ArrayElementValue)));
        if (out.type == TypeObject)
            for (const auto element : elements(u64(node + NodeObject)))
                out.members.emplace_back(readString(element + PairKey), readTree(nodeOf(element + PairValue)));
        return out;
    }
    // node must be cleared.
    void writeTree(std::uint64_t node, const HostNode& tree) {
        setType(node, tree.type);
        put8(node + NodeBoolean, tree.boolean ? 1 : 0);
        put(node + NodeInteger, static_cast<std::uint64_t>(tree.integer));
        put(node + NodeUInteger, tree.uinteger);
        put(node + NodeReal, std::bit_cast<std::uint64_t>(tree.real));
        if (tree.type == TypeString) assignString(node + NodeString, tree.string);
        for (const auto& item : tree.items) {
            const auto value = pushElement(u64(node + NodeArray), ArrayElementSize) + ArrayElementValue;
            construct(value);
            writeTree(u64(value), item);
        }
        for (const auto& [key, item] : tree.members) {
            const auto pair = pushElement(u64(node + NodeObject), PairElementSize);
            constructString(pair + PairKey, key);
            construct(pair + PairValue);
            writeTree(u64(pair + PairValue), item);
        }
    }

    std::uint64_t objectEntry(std::uint64_t object, const std::string& key) {
        const auto header = u64(object);
        for (const auto element : elements(header))
            if (readString(element + PairKey) == key) return element + PairValue;
        const auto pair = pushElement(header, PairElementSize);
        constructString(pair + PairKey, key);
        construct(pair + PairValue);
        return pair + PairValue;
    }
    std::uint64_t findKey(std::uint64_t value, const std::string& key) {
        const auto node = nodeOf(value);
        if (typeOf(node) == TypeObject)
            for (const auto element : elements(u64(node + NodeObject)))
                if (readString(element + PairKey) == key) return element + PairValue;
        return defaultValue(TypeNull);
    }

    Impl(Machine& machine, std::uint64_t base)
        : guest(machine), trampolines(std::make_shared<SceHostTrampolines>(machine, base, 255, "SCE json")) {
        stubPage = base + 0x100000;
        stubNext = stubPage;
        heapBase = heapNext = base + 0x200000;
        guest.Map(stubPage, 0x1000, Permission::Read | Permission::Write);
        guest.Write(stubPage, std::vector<std::byte>(0x1000, std::byte{0xcc}));
        guest.Protect(stubPage, 0x1000, Permission::Read | Permission::Execute);
        registerFunctions();
    }

    void add(std::string_view name, Handler handler) {
        if (!functions.emplace(Nid::ComputeNid(std::string(name), ""), std::pair{name, std::move(handler)}).second)
            throw std::logic_error("Duplicate sce::Json NID for " + std::string(name));
    }

    void registerFunctions() {
        using I = Impl;
        using M = Machine;
        const auto a = [](M& g, unsigned i) { return argument(g, i); };
        // Initializer / InitParameter2 {allocator, userData, fileBufferSize}.
        add("_ZN3sce4Json14InitParameter2C1Ev", [a](I& s, M& g) { const auto p = a(g, 0); s.put(p, 0); s.put(p + 8, 0); s.put(p + 16, 0); return Zero; });
        add("_ZN3sce4Json14InitParameter2C2Ev", [a](I& s, M& g) { const auto p = a(g, 0); s.put(p, 0); s.put(p + 8, 0); s.put(p + 16, 0); return Zero; });
        add("_ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv", [a](I& s, M& g) { s.put(a(g, 0), a(g, 1)); s.put(a(g, 0) + 8, a(g, 2)); return Zero; });
        add("_ZN3sce4Json14InitParameter217setFileBufferSizeEm", [a](I& s, M& g) { s.put(a(g, 0) + 16, a(g, 1)); return Zero; });
        // InitParameterRtti2's layout is unknown and upstream leaves it NotImplemented:
        // accept the call without writing guest memory, like upstream's return 0.
        add("_ZN3sce4Json18InitParameterRtti216setAllocatorRttiEPNS0_14AllocParamRttiEPv", [](I&, M&) { return Zero; });
        add("_ZN3sce4Json11InitializerC1Ev", [](I&, M&) { return Zero; });
        add("_ZN3sce4Json11InitializerD1Ev", [](I&, M&) { return Zero; });
        add("_ZN3sce4Json11Initializer9terminateEv", [](I&, M&) { return Zero; });
        add("_ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E", [a](I& s, M& g) {
            const auto p = a(g, 1);
            if (!p) throw std::invalid_argument("sce::Json::Initializer::initialize: null parameter");
            s.allocator = s.u64(p);
            s.allocatorUserData = s.u64(p + 8);
            return Zero;
        });
        add("_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE", [a](I&, M& g) {
            if (!a(g, 1)) throw std::invalid_argument("sce::Json::Initializer::initialize: null parameter");
            return Zero;
        });

        // String
        add("_ZN3sce4Json6StringC1Ev", [a](I& s, M& g) { s.constructString(a(g, 0), {}); return Zero; });
        add("_ZN3sce4Json6StringC1EPKc", [a](I& s, M& g) { s.constructString(a(g, 0), s.cString(a(g, 1), "String text")); return Zero; });
        add("_ZN3sce4Json6StringC1ERKS1_", [a](I& s, M& g) { s.constructString(a(g, 0), s.readString(a(g, 1))); return Zero; });
        add("_ZN3sce4Json6StringD1Ev", [a](I& s, M& g) { s.destroyString(a(g, 0)); return Zero; });
        add("_ZN3sce4Json6StringaSERKS1_", [a](I& s, M& g) {
            if (a(g, 0) != a(g, 1)) s.assignString(a(g, 0), s.readString(a(g, 1)));
            return a(g, 0);
        });
        add("_ZNK3sce4Json6String5c_strEv", [a](I& s, M& g) { return s.u64(a(g, 0)) + StringText; });
        add("_ZNK3sce4Json6String6lengthEv", [a](I& s, M& g) { return s.u64(s.u64(a(g, 0))); });
        add("_ZNK3sce4Json6String5emptyEv", [a](I& s, M& g) { return static_cast<std::uint64_t>(s.u64(s.u64(a(g, 0))) == 0); });

        // Array
        add("_ZN3sce4Json5ArrayC1Ev", [a](I& s, M& g) { s.constructList(a(g, 0)); return Zero; });
        add("_ZN3sce4Json5ArrayD1Ev", [a](I& s, M& g) { s.destroyArray(a(g, 0)); return Zero; });
        add("_ZN3sce4Json5Array9push_backERKNS0_5ValueE", [a](I& s, M& g) {
            const auto self = a(g, 0), value = a(g, 1);
            s.nodeOf(value);
            s.copyInto(s.pushElement(s.u64(self), ArrayElementSize) + ArrayElementValue, value);
            return Zero;
        });
        add("_ZNK3sce4Json5Array4sizeEv", [a](I& s, M& g) { return s.u64(s.u64(a(g, 0)) + ListSize); });
        add("_ZNK3sce4Json5Array5emptyEv", [a](I& s, M& g) { return static_cast<std::uint64_t>(s.u64(s.u64(a(g, 0)) + ListSize) == 0); });
        add("_ZNK3sce4Json5Array4backEv", [a](I& s, M& g) {
            const auto tail = s.u64(s.u64(a(g, 0)) + ListTail);
            if (!tail) throw std::out_of_range("sce::Json::Array::back: empty array");
            return tail + ArrayElementValue;
        });

        // Object
        add("_ZN3sce4Json6ObjectC1Ev", [a](I& s, M& g) { s.constructList(a(g, 0)); return Zero; });
        add("_ZN3sce4Json6ObjectC1ERKS1_", [a](I& s, M& g) { s.copyObject(a(g, 0), a(g, 1)); return Zero; });
        add("_ZN3sce4Json6ObjectD1Ev", [a](I& s, M& g) { s.destroyObject(a(g, 0)); return Zero; });
        add("_ZN3sce4Json6ObjectaSERKS1_", [a](I& s, M& g) {
            const auto self = a(g, 0), other = a(g, 1);
            if (self == other) return self;
            const auto copy = s.alloc(8);
            s.copyObject(copy, other);
            s.destroyObject(self);
            s.put(self, s.u64(copy));
            s.release(copy);
            return self;
        });
        add("_ZN3sce4Json6Object5clearEv", [a](I& s, M& g) { s.clearObjectItems(s.u64(a(g, 0))); return Zero; });
        add("_ZN3sce4Json6ObjectixERKNS0_6StringE", [a](I& s, M& g) { return s.objectEntry(a(g, 0), s.readString(a(g, 1))); });
        add("_ZNK3sce4Json6Object4sizeEv", [a](I& s, M& g) { return s.u64(s.u64(a(g, 0)) + ListSize); });
        add("_ZNK3sce4Json6Object5emptyEv", [a](I& s, M& g) { return static_cast<std::uint64_t>(s.u64(s.u64(a(g, 0)) + ListSize) == 0); });

        // Value constructors
        const auto scalar = [](I& s, std::uint64_t value, ValueType type, std::uint64_t field, std::uint64_t bits, bool byte) {
            s.construct(value);
            const auto node = s.u64(value);
            s.put32(node, type);
            if (byte) s.put8(node + field, static_cast<std::uint8_t>(bits != 0));
            else s.put(node + field, bits);
        };
        add("_ZN3sce4Json5ValueC1Ev", [a](I& s, M& g) { s.construct(a(g, 0)); return Zero; });
        add("_ZN3sce4Json5ValueC1Eb", [a, scalar](I& s, M& g) { scalar(s, a(g, 0), TypeBoolean, NodeBoolean, a(g, 1) & 0xff, true); return Zero; });
        add("_ZN3sce4Json5ValueC1El", [a, scalar](I& s, M& g) { scalar(s, a(g, 0), TypeInteger, NodeInteger, a(g, 1), false); return Zero; });
        add("_ZN3sce4Json5ValueC1Em", [a, scalar](I& s, M& g) { scalar(s, a(g, 0), TypeUInteger, NodeUInteger, a(g, 1), false); return Zero; });
        // The double arrives in XMM0; the guest stub in front of this gate copies it to RSI.
        add("_ZN3sce4Json5ValueC1Ed", [a, scalar](I& s, M& g) { scalar(s, a(g, 0), TypeReal, NodeReal, a(g, 1), false); return Zero; });
        add("_ZN3sce4Json5ValueC1EPKc", [a](I& s, M& g) {
            const auto text = s.cString(a(g, 1), "Value text");
            s.construct(a(g, 0));
            s.setType(s.u64(a(g, 0)), TypeString);
            s.assignString(s.u64(a(g, 0)) + NodeString, text);
            return Zero;
        });
        add("_ZN3sce4Json5ValueC1ERKNS0_6StringE", [a](I& s, M& g) {
            const auto text = s.readString(a(g, 1));
            s.construct(a(g, 0));
            s.setType(s.u64(a(g, 0)), TypeString);
            s.assignString(s.u64(a(g, 0)) + NodeString, text);
            return Zero;
        });
        add("_ZN3sce4Json5ValueC1ERKNS0_5ArrayE", [a](I& s, M& g) {
            s.construct(a(g, 0));
            const auto node = s.u64(a(g, 0));
            s.put32(node, TypeArray);
            s.copyArray(node + NodeArray, a(g, 1));
            return Zero;
        });
        add("_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", [a](I& s, M& g) {
            s.construct(a(g, 0));
            const auto node = s.u64(a(g, 0));
            s.put32(node, TypeObject);
            s.copyObject(node + NodeObject, a(g, 1));
            return Zero;
        });
        add("_ZN3sce4Json5ValueC1ENS0_9ValueTypeE", [a](I& s, M& g) {
            s.construct(a(g, 0));
            s.setType(s.u64(a(g, 0)), static_cast<ValueType>(static_cast<std::int32_t>(a(g, 1))));
            return Zero;
        });
        add("_ZN3sce4Json5ValueC1ERKS1_", [a](I& s, M& g) { s.copyInto(a(g, 0), a(g, 1)); return Zero; });
        add("_ZN3sce4Json5ValueD1Ev", [a](I& s, M& g) { s.destroyValue(a(g, 0)); return Zero; });
        add("_ZN3sce4Json5ValueaSERKS1_", [a](I& s, M& g) {
            if (a(g, 0) != a(g, 1)) s.assignNode(s.nodeOf(a(g, 0)), s.nodeOf(a(g, 1)));
            return a(g, 0);
        });
        add("_ZN3sce4Json5Value3setENS0_9ValueTypeE", [a](I& s, M& g) {
            s.setType(s.nodeOf(a(g, 0)), static_cast<ValueType>(static_cast<std::int32_t>(a(g, 1))));
            return Zero;
        });
        add("_ZN3sce4Json5Value3setERKS1_", [a](I& s, M& g) {
            if (a(g, 0) != a(g, 1)) s.assignNode(s.nodeOf(a(g, 0)), s.nodeOf(a(g, 1)));
            return Zero;
        });
        add("_ZN3sce4Json5Value5clearEv", [a](I& s, M& g) { if (a(g, 0)) s.clearNode(s.nodeOf(a(g, 0))); return Zero; });

        // Getters
        add("_ZNK3sce4Json5Value7getTypeEv", [a](I& s, M& g) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.typeOf(s.nodeOf(a(g, 0))))); });
        add("_ZNK3sce4Json5Value10getBooleanEv", [a](I& s, M& g) { return s.typed(a(g, 0), TypeBoolean) + NodeBoolean; });
        add("_ZNK3sce4Json5Value10getIntegerEv", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            if (s.typeOf(node) == TypeUInteger && s.u64(node + NodeUInteger) <= static_cast<std::uint64_t>(INT64_MAX)) {
                s.put(node + NodeInteger, s.u64(node + NodeUInteger));
                return node + NodeInteger;
            }
            return s.typed(a(g, 0), TypeInteger) + NodeInteger;
        });
        add("_ZNK3sce4Json5Value11getUIntegerEv", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            if (s.typeOf(node) == TypeInteger && static_cast<std::int64_t>(s.u64(node + NodeInteger)) >= 0) {
                s.put(node + NodeUInteger, s.u64(node + NodeInteger));
                return node + NodeUInteger;
            }
            return s.typed(a(g, 0), TypeUInteger) + NodeUInteger;
        });
        add("_ZNK3sce4Json5Value7getRealEv", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            const auto type = s.typeOf(node);
            if (type == TypeInteger || type == TypeUInteger) {
                const double real = type == TypeInteger ? static_cast<double>(static_cast<std::int64_t>(s.u64(node + NodeInteger)))
                                                        : static_cast<double>(s.u64(node + NodeUInteger));
                s.put(node + NodeReal, std::bit_cast<std::uint64_t>(real));
                return node + NodeReal;
            }
            return s.typed(a(g, 0), TypeReal) + NodeReal;
        });
        add("_ZNK3sce4Json5Value9getStringEv", [a](I& s, M& g) { return s.typed(a(g, 0), TypeString) + NodeString; });
        add("_ZNK3sce4Json5Value8getArrayEv", [a](I& s, M& g) { return s.typed(a(g, 0), TypeArray) + NodeArray; });
        add("_ZNK3sce4Json5Value9getObjectEv", [a](I& s, M& g) { return s.typed(a(g, 0), TypeObject) + NodeObject; });
        add("_ZNK3sce4Json5Value5countEv", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            const auto type = s.typeOf(node);
            if (type == TypeArray) return s.u64(s.u64(node + NodeArray) + ListSize);
            if (type == TypeObject) return s.u64(s.u64(node + NodeObject) + ListSize);
            return Zero;
        });
        add("_ZNK3sce4Json5ValueixEPKc", [a](I& s, M& g) {
            if (!a(g, 1)) return s.defaultValue(TypeNull);
            return s.findKey(a(g, 0), s.cString(a(g, 1), "key"));
        });
        add("_ZNK3sce4Json5ValueixEm", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            if (s.typeOf(node) == TypeArray) {
                const auto items = s.elements(s.u64(node + NodeArray));
                if (a(g, 1) < items.size()) return items[a(g, 1)] + ArrayElementValue;
            }
            return s.defaultValue(TypeNull);
        });
        add("_ZN3sce4Json5Value10referArrayEv", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            if (s.typeOf(node) == TypeNull) s.setType(node, TypeArray);
            if (s.typeOf(node) != TypeArray) throw std::runtime_error("sce::Json::Value::referArray: value is not an array");
            return node + NodeArray;
        });
        add("_ZN3sce4Json5Value11referObjectEv", [a](I& s, M& g) {
            const auto node = s.nodeOf(a(g, 0));
            if (s.typeOf(node) == TypeNull) s.setType(node, TypeObject);
            if (s.typeOf(node) != TypeObject) throw std::runtime_error("sce::Json::Value::referObject: value is not an object");
            return node + NodeObject;
        });

        // Serialise / parse on host copies.
        const auto serialize = [a](I& s, M& g, bool stringAsIs) {
            const auto node = s.nodeOf(a(g, 0));
            const auto out = a(g, 1);
            if (!out) throw std::invalid_argument("sce::Json::Value::serialize: null String");
            std::string text;
            const auto tree = s.readTree(node);
            if (stringAsIs && tree.type == TypeString) text = tree.string;
            else Serialize(text, tree);
            if (s.u64(out)) s.assignString(out, text);
            else s.constructString(out, text);
            return Zero;
        };
        add("_ZN3sce4Json5Value9serializeERNS0_6StringE", [serialize](I& s, M& g) { return serialize(s, g, false); });
        add("_ZNK3sce4Json5Value8toStringERNS0_6StringE", [serialize](I& s, M& g) { return serialize(s, g, true); });
        add("_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", [a](I& s, M& g) {
            const auto out = a(g, 0), text = a(g, 1), size = a(g, 2);
            if (!text) throw std::invalid_argument("sce::Json::Parser::parse: null text");
            const auto node = s.nodeOf(out);
            const auto copy = s.bytes(text, size);
            HostNode parsed;
            Parser parser(copy.data(), copy.size());
            if (!parser.Parse(parsed)) return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(JsonErrorParse)));
            s.clearNode(node);
            s.writeTree(node, parsed);
            return Zero;
        });
    }

    // movq rsi, xmm0; jmp [rip+0]; .quad gate
    std::uint64_t xmm0ToRsi(std::uint64_t gate) {
        std::array<std::uint8_t, 24> code{0x66, 0x48, 0x0f, 0x7e, 0xc6, 0xff, 0x25, 0, 0, 0, 0};
        std::memcpy(code.data() + 11, &gate, sizeof(gate));
        if (stubNext + code.size() > stubPage + 0x1000) throw std::runtime_error("sce::Json stub page exhausted");
        const auto stub = stubNext;
        guest.Protect(stubPage, 0x1000, Permission::Read | Permission::Write);
        guest.Write(stub, std::as_bytes(std::span(code)));
        guest.Protect(stubPage, 0x1000, Permission::Read | Permission::Execute);
        stubNext += code.size();
        return stub;
    }
};

SceJsonImports::SceJsonImports(Machine& machine, std::uint64_t gateBase) : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceJsonImports::~SceJsonImports() = default;

std::uint64_t SceJsonImports::HeapBase() const { return impl->heapBase; }
std::uint64_t SceJsonImports::HeapEnd() const { return impl->heapBase + impl->heapMapped; }
std::size_t SceJsonImports::LiveAllocations() const { return impl->live.size(); }
std::uint64_t SceJsonImports::RegisteredAllocator() const { return impl->allocator; }
std::uint64_t SceJsonImports::RegisteredAllocatorUserData() const { return impl->allocatorUserData; }

std::optional<std::uint64_t> SceJsonImports::Resolve(const SceImport& import) {
    if ((import.LibraryName != "libSceJson2" && import.LibraryName != "libSceJson") ||
        (import.ModuleName != "libSceJson" && import.ModuleName != "libSceJson2"))
        return std::nullopt;
    const auto found = impl->functions.find(import.Nid);
    if (found == impl->functions.end()) return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    auto gate = impl->trampolines->Add([state = std::weak_ptr<Impl>(impl), run = found->second.second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("sce::Json import runtime has expired");
        guest.Set(Register::Rax, run(*context, guest));
    });
    if (found->second.first == "_ZN3sce4Json5ValueC1Ed") gate = impl->xmm0ToRsi(gate);
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
