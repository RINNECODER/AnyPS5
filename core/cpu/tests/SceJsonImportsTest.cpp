#include <cpu/SceJsonImports.hpp>
#include <cpu/SceElf.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t Scratch = 0x10000;
constexpr std::int64_t ParseError = static_cast<std::int32_t>(0x80920101);

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Cpu::SceImport qualified(std::string_view name, const char* library = "libSceJson2") {
    Cpu::SceImport value;
    value.Nid = Nid::ComputeNid(std::string(name), "");
    value.LibraryName = library;
    value.ModuleName = "libSceJson";
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceJsonImports> imports = std::make_unique<Cpu::SceJsonImports>(machine);
    std::uint64_t next = Scratch;

    Session() {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "sce::Json fixture requires the native modern TCG backend");
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(Scratch, 0x10000, rw);
        // 0x1000: call [0x2000]; mov [0x2010], rax; nop
        constexpr std::array<std::uint8_t, 14> caller{0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
        // 0x1100: movq xmm0, rdx; call [0x2000]; nop - the double argument travels in XMM0 only.
        constexpr std::array<std::uint8_t, 12> doubleCaller{0x66, 0x48, 0x0f, 0x6e, 0xc2, 0xff, 0x15, 0xf5, 0x0e, 0, 0, 0x90};
        machine.Write(0x1100, std::as_bytes(std::span(doubleCaller)));
    }

    std::uint64_t gate(std::string_view name) {
        const auto found = imports->Resolve(qualified(name));
        require(found.has_value(), "sce::Json did not resolve " + std::string(name));
        return *found;
    }

    std::uint64_t call(std::string_view name, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
        const auto address = gate(name);
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, a);
        machine.Set(Register::Rsi, b);
        machine.Set(Register::Rdx, c);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x1234);
        require(machine.Run(0x1000, 0x100d, 100) == Cpu::StopReason::Address, std::string(name) + " did not return to its caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x1234, std::string(name) + " corrupted the caller frame");
        return machine.Get(Register::Rax);
    }

    void callDouble(std::string_view name, std::uint64_t self, double value) {
        const auto address = gate(name);
        machine.Write(0x2000, std::as_bytes(std::span(&address, 1)));
        machine.Set(Register::Rdi, self);
        machine.Set(Register::Rsi, 0xdeadbeefdeadbeef);
        machine.Set(Register::Rdx, std::bit_cast<std::uint64_t>(value));
        machine.Set(Register::Rsp, 0x4ff0);
        require(machine.Run(0x1100, 0x110b, 100) == Cpu::StopReason::Address, std::string(name) + " did not return to its caller");
    }

    std::uint64_t alloc(std::size_t size) {
        const auto address = next;
        next += (size + 15) & ~std::size_t{15};
        require(next <= Scratch + 0x10000, "test scratch exhausted");
        return address;
    }
    std::uint64_t text(std::string_view value) {
        const auto address = alloc(value.size() + 1);
        std::string copy(value);
        machine.Write(address, std::as_bytes(std::span(copy.c_str(), copy.size() + 1)));
        return address;
    }
    template <typename T> T read(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    void write64(std::uint64_t address, std::uint64_t value) { machine.Write(address, std::as_bytes(std::span(&value, 1))); }
    std::string cString(std::uint64_t address) {
        std::string out;
        for (char c; (c = read<char>(address)) != '\0'; ++address) out += c;
        return out;
    }
    bool inHeap(std::uint64_t address) const { return address >= imports->HeapBase() && address < imports->HeapEnd(); }

    // Convenience wrappers named after the sce::Json member they call.
    std::uint64_t value() { const auto v = alloc(8); call("_ZN3sce4Json5ValueC1Ev", v); return v; }
    std::uint64_t string(std::string_view s) { const auto v = alloc(8); call("_ZN3sce4Json6StringC1EPKc", v, text(s)); return v; }
    std::string cStr(std::uint64_t string) { return cString(call("_ZNK3sce4Json6String5c_strEv", string)); }
    std::int32_t type(std::uint64_t value) { return static_cast<std::int32_t>(call("_ZNK3sce4Json5Value7getTypeEv", value)); }
    std::uint64_t key(std::uint64_t value, std::string_view k) { return call("_ZNK3sce4Json5ValueixEPKc", value, text(k)); }
    std::uint64_t index(std::uint64_t value, std::uint64_t i) { return call("_ZNK3sce4Json5ValueixEm", value, i); }
    std::uint64_t parse(std::uint64_t value, std::string_view json) { return call("_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", value, text(json), json.size()); }
    std::string serialize(std::uint64_t value) {
        const auto out = alloc(8);
        call("_ZN3sce4Json6StringC1Ev", out);
        require(call("_ZN3sce4Json5Value9serializeERNS0_6StringE", value, out) == 0, "serialize failed");
        auto result = cStr(out);
        call("_ZN3sce4Json6StringD1Ev", out);
        return result;
    }
};

void listResolves() {
    Session s;
    constexpr std::array<std::string_view, 44> names{
        "_ZN3sce4Json14InitParameter2C2Ev", "_ZN3sce4Json18InitParameterRtti216setAllocatorRttiEPNS0_14AllocParamRttiEPv",
        "_ZN3sce4Json14InitParameter217setFileBufferSizeEm", "_ZN3sce4Json11InitializerC1Ev",
        "_ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E", "_ZN3sce4Json11InitializerD1Ev",
        "_ZN3sce4Json6ObjectC1Ev", "_ZN3sce4Json6StringC1EPKc", "_ZN3sce4Json5ValueC1ERKNS0_6StringE",
        "_ZN3sce4Json6ObjectixERKNS0_6StringE", "_ZN3sce4Json5ValueaSERKS1_", "_ZN3sce4Json6StringD1Ev",
        "_ZN3sce4Json5ValueD1Ev", "_ZN3sce4Json6ObjectC1ERKS1_", "_ZN3sce4Json6ObjectD1Ev",
        "_ZN3sce4Json5Value10referArrayEv", "_ZN3sce4Json5Value3setENS0_9ValueTypeE",
        "_ZN3sce4Json5Array9push_backERKNS0_5ValueE", "_ZN3sce4Json5ValueC1El", "_ZN3sce4Json5Value11referObjectEv",
        "_ZN3sce4Json6ObjectaSERKS1_", "_ZNK3sce4Json5Value10getBooleanEv", "_ZN3sce4Json5Value9serializeERNS0_6StringE",
        "_ZN3sce4Json6Object5clearEv", "_ZNK3sce4Json5Array4backEv", "_ZN3sce4Json5ValueC1Eb",
        "_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", "_ZN3sce4Json5ValueC1Ev", "_ZN3sce4Json6StringaSERKS1_",
        "_ZN3sce4Json5ValueC1EPKc", "_ZNK3sce4Json6String6lengthEv", "_ZNK3sce4Json5Value9getStringEv",
        "_ZNK3sce4Json5ValueixEm", "_ZNK3sce4Json5Value9getObjectEv", "_ZNK3sce4Json5Value7getTypeEv",
        "_ZNK3sce4Json5Value10getIntegerEv", "_ZN3sce4Json6StringC1Ev", "_ZNK3sce4Json5ValueixEPKc",
        "_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", "_ZNK3sce4Json6String5c_strEv", "_ZNK3sce4Json5Value8getArrayEv",
        "_ZNK3sce4Json5Value7getRealEv", "_ZN3sce4Json5ValueC1Ed", "_ZNK3sce4Json5Array4sizeEv"};
    for (const auto name : names) {
        require(s.imports->Resolve(qualified(name)).has_value(), "unresolved " + std::string(name));
        require(s.imports->Resolve(qualified(name, "libSceJson")).has_value(), "libSceJson alias unresolved " + std::string(name));
    }
    // NIDs from the import table, so the name hashing is pinned too.
    auto import = qualified("_ZN3sce4Json5ValueC1Ev");
    import.Nid = "S5JxQnoGF3E";
    require(s.imports->Resolve(import).has_value(), "Parser::parse NID did not resolve");
    require(!s.imports->Resolve(qualified("_ZN3sce4Json5ValueC1Ev", "libSceNet")).has_value(), "foreign library resolved");
    import.Nid = "AAAAAAAAAAA";
    require(!s.imports->Resolve(import).has_value(), "unknown NID resolved");
}

void initializerRecordsAllocatorWithoutCallingIt() {
    Session s;
    const auto parameter = s.alloc(40);
    s.write64(parameter, ~0ULL);
    s.write64(parameter + 8, ~0ULL);
    s.write64(parameter + 16, ~0ULL);
    s.call("_ZN3sce4Json14InitParameter2C2Ev", parameter);
    require(s.read<std::uint64_t>(parameter) == 0 && s.read<std::uint64_t>(parameter + 8) == 0 &&
            s.read<std::uint64_t>(parameter + 16) == 0, "InitParameter2 constructor did not clear its fields");
    s.call("_ZN3sce4Json14InitParameter217setFileBufferSizeEm", parameter, 0x2000);
    require(s.read<std::uint64_t>(parameter + 16) == 0x2000, "setFileBufferSize did not store the size");
    // A MemAllocator whose vtable points at unmapped memory: calling it would fault.
    const auto allocator = s.alloc(8);
    s.write64(allocator, 0xdead0000);
    s.write64(parameter, allocator);
    s.write64(parameter + 8, 0x77);
    const auto rtti = s.alloc(16);
    require(s.call("_ZN3sce4Json18InitParameterRtti216setAllocatorRttiEPNS0_14AllocParamRttiEPv", rtti, allocator, 0x55) == 0,
            "setAllocatorRtti failed");
    const auto initializer = s.alloc(16);
    require(s.call("_ZN3sce4Json11InitializerC1Ev", initializer) == 0, "Initializer constructor failed");
    require(s.call("_ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E", initializer, parameter) == 0, "initialize failed");
    require(s.imports->RegisteredAllocator() == allocator && s.imports->RegisteredAllocatorUserData() == 0x77,
            "initialize did not record the guest allocator");
    // Allocation routing: payloads come from the provider heap, never the guest allocator.
    const auto v = s.value();
    require(s.parse(v, R"({"name":"text","list":[1,2,3]})") == 0, "parse after initialize failed");
    const auto node = s.read<std::uint64_t>(v);
    require(s.inHeap(node), "Value node is outside the provider heap");
    const auto name = s.call("_ZNK3sce4Json5Value9getStringEv", s.key(v, "name"));
    require(s.inHeap(name) && s.inHeap(s.call("_ZNK3sce4Json6String5c_strEv", name)), "string payload is outside the provider heap");
    require(s.inHeap(s.index(s.key(v, "list"), 2)), "array element is outside the provider heap");
    s.call("_ZN3sce4Json5ValueD1Ev", v);
    require(s.read<std::uint64_t>(v) == 0, "Value destructor left its node pointer");
    require(s.call("_ZN3sce4Json11InitializerD1Ev", initializer) == 0, "Initializer destructor failed");
}

void stringsLiveInGuestMemory() {
    Session s;
    const auto empty = s.alloc(8);
    s.call("_ZN3sce4Json6StringC1Ev", empty);
    require(s.cStr(empty).empty() && s.call("_ZNK3sce4Json6String6lengthEv", empty) == 0, "empty String is not empty");
    const auto baseline = s.imports->LiveAllocations();
    const auto hello = s.string("hello, json");
    require(s.inHeap(s.read<std::uint64_t>(hello)), "String rep is outside the provider heap");
    require(s.cStr(hello) == "hello, json", "String c_str mismatch");
    require(s.call("_ZNK3sce4Json6String6lengthEv", hello) == 11, "String length mismatch");
    require(s.call("_ZN3sce4Json6StringaSERKS1_", empty, hello) == empty, "String assignment did not return this");
    require(s.cStr(empty) == "hello, json" && s.cStr(hello) == "hello, json", "String assignment did not copy");
    s.call("_ZN3sce4Json6StringaSERKS1_", hello, hello);
    require(s.cStr(hello) == "hello, json", "String self-assignment changed the text");
    s.call("_ZN3sce4Json6StringD1Ev", hello);
    require(s.read<std::uint64_t>(hello) == 0, "String destructor left its rep");
    require(s.imports->LiveAllocations() == baseline, "String assignment/destructor leaked or double freed");
    s.call("_ZN3sce4Json6StringD1Ev", empty);
}

void constructorsAndGetters() {
    Session s;
    // Instantiate the typed defaults first so the leak baseline excludes them.
    const auto probe = s.value();
    s.call("_ZNK3sce4Json5Value9getStringEv", probe);
    s.call("_ZNK3sce4Json5Value10getBooleanEv", probe);
    s.call("_ZN3sce4Json5ValueD1Ev", probe);
    const auto baseline = s.imports->LiveAllocations();

    const auto null = s.value();
    require(s.type(null) == 0, "Value() is not null");
    const auto boolean = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1Eb", boolean, 1);
    require(s.type(boolean) == 1 && s.read<std::uint8_t>(s.call("_ZNK3sce4Json5Value10getBooleanEv", boolean)) == 1,
            "Value(bool) mismatch");
    const auto integer = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1El", integer, static_cast<std::uint64_t>(-42));
    require(s.type(integer) == 2 && s.read<std::int64_t>(s.call("_ZNK3sce4Json5Value10getIntegerEv", integer)) == -42,
            "Value(int64) mismatch");
    require(s.read<double>(s.call("_ZNK3sce4Json5Value7getRealEv", integer)) == -42.0, "getReal did not widen an integer");
    const auto real = s.alloc(8);
    s.callDouble("_ZN3sce4Json5ValueC1Ed", real, 2.5);
    require(s.type(real) == 4 && s.read<double>(s.call("_ZNK3sce4Json5Value7getRealEv", real)) == 2.5,
            "Value(double) did not take its argument from XMM0");
    const auto text = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1EPKc", text, s.text("abc"));
    require(s.type(text) == 5 && s.cStr(s.call("_ZNK3sce4Json5Value9getStringEv", text)) == "abc", "Value(const char*) mismatch");
    const auto key = s.string("from-string");
    const auto fromString = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1ERKNS0_6StringE", fromString, key);
    require(s.cStr(s.call("_ZNK3sce4Json5Value9getStringEv", fromString)) == "from-string", "Value(String) mismatch");
    // Mismatched getters return the typed default, like upstream with no null-access callback.
    require(s.cStr(s.call("_ZNK3sce4Json5Value9getStringEv", integer)).empty(), "getString on an integer is not empty");
    require(s.read<std::uint8_t>(s.call("_ZNK3sce4Json5Value10getBooleanEv", text)) == 0, "getBoolean default is not false");
    require(s.call("_ZNK3sce4Json5Array4sizeEv", s.call("_ZNK3sce4Json5Value8getArrayEv", text)) == 0, "getArray default not empty");
    require(s.type(s.index(integer, 0)) == 0 && s.type(s.key(integer, "x")) == 0, "index on a scalar is not null");
    // set(ValueType) and assignment.
    require(s.call("_ZN3sce4Json5Value3setENS0_9ValueTypeE", null, 6) == 0 && s.type(null) == 6, "set(Array) mismatch");
    require(s.call("_ZN3sce4Json5ValueaSERKS1_", null, text) == null && s.type(null) == 5 &&
            s.cStr(s.call("_ZNK3sce4Json5Value9getStringEv", null)) == "abc", "Value assignment mismatch");
    s.call("_ZN3sce4Json5ValueaSERKS1_", text, text);
    require(s.cStr(s.call("_ZNK3sce4Json5Value9getStringEv", text)) == "abc", "Value self-assignment changed it");
    for (const auto v : {null, boolean, integer, real, text, fromString}) s.call("_ZN3sce4Json5ValueD1Ev", v);
    s.call("_ZN3sce4Json6StringD1Ev", key);
    require(s.imports->LiveAllocations() == baseline, "Value destructors leaked: " + std::to_string(s.imports->LiveAllocations()) +
            " live, expected " + std::to_string(baseline));
}

void parseIndexAndArrays() {
    Session s;
    const auto v = s.value();
    require(s.parse(v, R"( {"a":1,"b":[true,"x\né",2.5,18446744073709551615],"c":{"d":null}} )") == 0, "parse failed");
    require(s.type(v) == 7, "root is not an object");
    require(s.read<std::int64_t>(s.call("_ZNK3sce4Json5Value10getIntegerEv", s.key(v, "a"))) == 1, "a != 1");
    const auto b = s.key(v, "b");
    require(s.type(b) == 6, "b is not an array");
    const auto array = s.call("_ZNK3sce4Json5Value8getArrayEv", b);
    require(s.call("_ZNK3sce4Json5Array4sizeEv", array) == 4, "array size mismatch");
    require(s.read<std::uint8_t>(s.call("_ZNK3sce4Json5Value10getBooleanEv", s.index(b, 0))) == 1, "b[0] != true");
    const auto x = s.call("_ZNK3sce4Json5Value9getStringEv", s.index(b, 1));
    require(s.cStr(x) == "x\n\xc3\xa9" && s.call("_ZNK3sce4Json6String6lengthEv", x) == 4, "b[1] text mismatch");
    require(s.read<double>(s.call("_ZNK3sce4Json5Value7getRealEv", s.index(b, 2))) == 2.5, "b[2] != 2.5");
    const auto back = s.call("_ZNK3sce4Json5Array4backEv", array);
    require(back == s.index(b, 3) && s.type(back) == 3, "back() is not the last element");
    require(s.type(s.index(b, 4)) == 0, "out-of-range index is not null");
    require(s.type(s.key(v, "missing")) == 0, "missing key is not null");
    const auto c = s.key(v, "c");
    require(s.type(c) == 7 && s.type(s.key(c, "d")) == 0, "nested object mismatch");
    require(s.call("_ZNK3sce4Json5Value9getObjectEv", c) == s.read<std::uint64_t>(c) + 0x30, "getObject is not the node's Object");
    require(s.serialize(v) == R"({"a":1,"b":[true,"x\n)" "\xc3\xa9" R"(",2.5,18446744073709551615],"c":{"d":null}})",
            "round trip mismatch: " + s.serialize(v));
    // A failed parse returns the upstream error and leaves the value untouched.
    require(static_cast<std::int64_t>(static_cast<std::int32_t>(s.parse(v, "[1,"))) == ParseError, "bad JSON did not fail");
    require(s.type(v) == 7, "failed parse changed the value");
    require(s.parse(v, "[]") == 0 && s.type(v) == 6, "reparse failed");
    s.call("_ZN3sce4Json5ValueD1Ev", v);
}

void buildObjectsAndSerialize() {
    Session s;
    const auto probe = s.value();
    s.call("_ZNK3sce4Json5Value9getObjectEv", probe);
    s.call("_ZN3sce4Json5ValueD1Ev", probe);
    const auto baseline = s.imports->LiveAllocations();

    const auto root = s.value();
    const auto object = s.call("_ZN3sce4Json5Value11referObjectEv", root);
    require(object == s.read<std::uint64_t>(root) + 0x30 && s.type(root) == 7, "referObject did not make an object");
    const auto k = s.string("k");
    const auto slot = s.call("_ZN3sce4Json6ObjectixERKNS0_6StringE", object, k);
    require(s.inHeap(slot) && s.type(slot) == 0, "Object[] did not insert a null");
    require(s.call("_ZN3sce4Json6ObjectixERKNS0_6StringE", object, k) == slot, "Object[] inserted a duplicate key");
    const auto array = s.call("_ZN3sce4Json5Value10referArrayEv", slot);
    const auto one = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1El", one, 1);
    const auto yes = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1Eb", yes, 1);
    s.call("_ZN3sce4Json5Array9push_backERKNS0_5ValueE", array, one);
    s.call("_ZN3sce4Json5Array9push_backERKNS0_5ValueE", array, yes);
    require(s.call("_ZNK3sce4Json5Array4sizeEv", array) == 2, "push_back size mismatch");
    const auto quoted = s.string("q\"");
    const auto other = s.call("_ZN3sce4Json6ObjectixERKNS0_6StringE", object, quoted);
    s.call("_ZN3sce4Json5ValueaSERKS1_", other, yes);
    require(s.serialize(root) == R"({"k":[1,true],"q\"":true})", "serialize mismatch: " + s.serialize(root));

    // Object copy constructor, assignment, Value(Object), clear and destructor.
    const auto copy = s.alloc(8);
    s.call("_ZN3sce4Json6ObjectC1ERKS1_", copy, object);
    const auto fromObject = s.alloc(8);
    s.call("_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", fromObject, copy);
    require(s.serialize(fromObject) == R"({"k":[1,true],"q\"":true})", "Value(Object) mismatch");
    s.call("_ZN3sce4Json6Object5clearEv", object);
    require(s.serialize(root) == "{}" && s.serialize(fromObject) != "{}", "clear() reached a copy");
    const auto assigned = s.alloc(8);
    s.call("_ZN3sce4Json6ObjectC1Ev", assigned);
    require(s.call("_ZN3sce4Json6ObjectaSERKS1_", assigned, copy) == assigned, "Object assignment did not return this");
    s.call("_ZN3sce4Json6ObjectaSERKS1_", assigned, assigned);
    s.call("_ZN3sce4Json5Value3setENS0_9ValueTypeE", root, 0);
    const auto rootObject = s.call("_ZN3sce4Json5Value11referObjectEv", root);
    s.call("_ZN3sce4Json6ObjectaSERKS1_", rootObject, assigned);
    require(s.serialize(root) == R"({"k":[1,true],"q\"":true})", "Object assignment mismatch");

    for (const auto o : {copy, assigned}) s.call("_ZN3sce4Json6ObjectD1Ev", o);
    for (const auto v : {root, one, yes, fromObject}) s.call("_ZN3sce4Json5ValueD1Ev", v);
    for (const auto t : {k, quoted}) s.call("_ZN3sce4Json6StringD1Ev", t);
    require(s.imports->LiveAllocations() == baseline, "object graph leaked: " + std::to_string(s.imports->LiveAllocations()) +
            " live, expected " + std::to_string(baseline));
}

}

int main() {
    try {
        listResolves();
        initializerRecordsAllocatorWithoutCallingIt();
        stringsLiveInGuestMemory();
        constructorsAndGetters();
        parseIndexAndArrays();
        buildObjectsAndSerialize();
    } catch (const std::exception& error) {
        std::cerr << "sce::Json imports test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "sce::Json imports test passed\n";
    return 0;
}
