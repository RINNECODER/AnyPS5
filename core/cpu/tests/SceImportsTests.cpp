#include <cpu/Cpu.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceImports.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Cpu::Machine;
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing SCE import rejection: ") + expected);
}

Cpu::SceImport import(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libc";
    value.LibraryId = 1;
    value.ModuleName = "libc";
    value.ModuleId = 1;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Machine machine;
    Cpu::SceImports imports{machine};

    Session() {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> program{
            0xff, 0x15, 0xfa, 0x0f, 0x00, 0x00,
            0x48, 0x89, 0x05, 0x03, 0x10, 0x00, 0x00,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(program)));
    }

    void setup(const char* nid, std::uint64_t first, std::uint64_t second = 0, std::uint64_t length = 0) {
        const auto gate = imports.Resolve(import(nid));
        machine.CheckAccess(gate, 1, Permission::Execute);
        rejects([&] { machine.CheckAccess(gate, 1, Permission::Write); }, "permission");
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        machine.Set(Register::Rdi, first);
        machine.Set(Register::Rsi, second);
        machine.Set(Register::Rdx, length);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x76543210fedeabcd);
    }

    std::uint64_t call(const char* nid, std::uint64_t first, std::uint64_t second = 0, std::uint64_t length = 0) {
        setup(nid, first, second, length);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "SCE import did not return to actual x86 caller");
        require(machine.Get(Register::Rsp) == 0x4ff0, "SCE import corrupted CALL/RET guest stack");
        require(machine.Get(Register::Rbx) == 0x76543210fedeabce, "SCE import corrupted callee-saved RBX or skipped continuation");
        std::uint64_t stored;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&stored, 1)));
        require(stored == machine.Get(Register::Rax), "x86 caller did not store the actual SCE return register");
        return stored;
    }

    std::vector<std::byte> read(std::uint64_t address, std::size_t count) {
        std::vector<std::byte> bytes(count);
        machine.Read(address, bytes);
        return bytes;
    }
};

void memoryCalls() {
    Session session;
    constexpr std::array<std::uint8_t, 8> source{0x80, 0x00, 0xff, 0x7f, 0x13, 0x42, 0x65, 0xa9};
    session.machine.Write(0x3010, std::as_bytes(std::span(source)));
    require(session.call("Q3VBxCXhUHs", 0x3040, 0x3010, source.size()) == 0x3040, "memcpy returned a host pointer instead of guest destination");
    const auto copied = session.read(0x3040, source.size());
    require(std::equal(copied.begin(), copied.end(), std::as_bytes(std::span(source)).begin()), "memcpy changed binary guest bytes");
    require(session.call("+P6FRGH4LfA", 0x3042, 0x3040, 6) == 0x3042, "memmove returned wrong guest pointer");
    constexpr std::array<std::uint8_t, 8> moved{0x80, 0x00, 0x80, 0x00, 0xff, 0x7f, 0x13, 0x42};
    const auto actual = session.read(0x3040, moved.size());
    require(std::equal(actual.begin(), actual.end(), std::as_bytes(std::span(moved)).begin()), "overlapping memmove did not preserve original source bytes");
    require(session.call("8zTFvBIAIN8", 0x3041, 0x1234ab, 3) == 0x3041, "memset returned wrong guest pointer");
    constexpr std::array<std::uint8_t, 8> filled{0x80, 0xab, 0xab, 0xab, 0xff, 0x7f, 0x13, 0x42};
    const auto set = session.read(0x3040, filled.size());
    require(std::equal(set.begin(), set.end(), std::as_bytes(std::span(filled)).begin()), "memset failed unsigned byte conversion or changed adjacent bytes");
    require(session.call("Q3VBxCXhUHs", 0xdead0000, 0xbad0000, 0) == 0xdead0000, "zero-length memcpy incorrectly accessed guest pointers");
}

void stringCalls() {
    Session session;
    constexpr std::array<std::uint8_t, 9> strings{0x80, 'b', 0, 'z', 0, 0x7f, 'b', 0, 0};
    session.machine.Write(0x3010, std::as_bytes(std::span(strings)));
    require(session.call("j4ViWNHEgww", 0x3010) == 2, "strlen did not stop at first guest NUL");
    require(session.call("j4ViWNHEgww", 0x3018) == 0, "strlen misread an empty guest string");
    const auto greater = session.call("Ovb2dSJOAuE", 0x3010, 0x3015);
    require(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(greater)) > 0, "strcmp did not compare unsigned bytes");
    const auto lesser = session.call("Ovb2dSJOAuE", 0x3015, 0x3010);
    require(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(lesser)) < 0, "strcmp negative int return is incorrect");
    require(session.call("Ovb2dSJOAuE", 0x3010, 0x3010) == 0, "strcmp failed equal guest strings");
    constexpr std::array end{std::byte{'x'}, std::byte{0}};
    session.machine.Map(0x6000, 4096, rw);
    session.machine.Write(0x6ffe, end);
    require(session.call("j4ViWNHEgww", 0x6ffe) == 1, "strlen read beyond a mapped page after its final-byte terminator");
}

void scopedBinding() {
    Session session;
    const auto original = import("Q3VBxCXhUHs");
    const auto first = session.imports.Resolve(original);
    require(session.imports.Resolve(original) == first, "Repeated identical scoped import changed guest function address");
    auto otherIds = original;
    otherIds.LibraryId = 2;
    otherIds.ModuleId = 3;
    const auto otherGate = session.imports.Resolve(otherIds);
    session.setup("Q3VBxCXhUHs", 0xdead0000, 0, 0);
    session.machine.Write(0x2000, std::as_bytes(std::span(&otherGate, 1)));
    require(session.machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address &&
            session.machine.Get(Register::Rax) == 0xdead0000,
            "Importer-local IDs incorrectly prevented binding the declared libc scope");
    auto wrong = original;
    wrong.ModuleName = "unrelated";
    rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
    wrong = original; wrong.LibraryName = "libkernel";
    rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
    wrong = original; wrong.LibraryVersion = 2;
    rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
    wrong = original; wrong.ModuleMajor = 2;
    rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
    wrong = original; wrong.ModuleMinor = 2;
    rejects([&] { session.imports.Resolve(wrong); }, "scope/version");
    wrong = original; wrong.Nid = "AAAAAAAAAAA";
    rejects([&] { session.imports.Resolve(wrong); }, "Unsupported SCE import service");
}

// Every importer numbers its own libraries, so one libc service gets a gate per scope.
// More scopes than one page of gates, and than the TCG engine's 256 host gates, still bind.
void manyScopedGates() {
    Session session;
    std::vector<std::uint64_t> gates;
    for (std::uint16_t id = 1; id <= 1200; ++id) {
        auto scoped = import("j4ViWNHEgww");
        scoped.LibraryId = id;
        gates.push_back(session.imports.Resolve(scoped));
    }
    std::vector<std::uint64_t> unique = gates;
    std::sort(unique.begin(), unique.end());
    require(std::adjacent_find(unique.begin(), unique.end()) == unique.end(), "Distinct import scopes shared one gate");
    auto repeated = import("j4ViWNHEgww");
    repeated.LibraryId = 1000;
    require(session.imports.Resolve(repeated) == gates[999], "Repeated scope on a grown gate page changed its address");
    constexpr std::array text{std::byte{'g'}, std::byte{'a'}, std::byte{'t'}, std::byte{'e'}, std::byte{0}};
    session.machine.Write(0x3000, text);
    for (const auto index : {0, 255, 256, 1199}) {
        session.setup("j4ViWNHEgww", 0x3000);
        session.machine.Write(0x2000, std::as_bytes(std::span(&gates[index], 1)));
        require(session.machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address &&
                session.machine.Get(Register::Rax) == 4, "A gate beyond the first page did not reach its service");
    }
}

void memoryFailures() {
    Session session;
    constexpr std::array sentinel{std::byte{0x17}, std::byte{0xa4}, std::byte{0x2f}, std::byte{0x8e}};
    session.machine.Write(0x3000, sentinel);
    session.setup("Q3VBxCXhUHs", 0x3000, 0x9000, 4);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "Guest access denied at 0x9000");
    require(session.read(0x3000, 4) == std::vector<std::byte>(sentinel.begin(), sentinel.end()), "Rejected memcpy partially changed destination");
    session.machine.Write(0x4ffe, std::span(sentinel).first(2));
    session.setup("8zTFvBIAIN8", 0x4ffe, 0x55, 4);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "Guest access denied at 0x5000");
    require(session.read(0x4ffe, 2) == std::vector<std::byte>(sentinel.begin(), sentinel.begin() + 2),
            "Out-of-range memset partially changed valid destination bytes");
    session.machine.Protect(0x3000, 4096, Permission::Read);
    session.setup("8zTFvBIAIN8", 0x3000, 0x55, 4);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "permission");
    require(session.read(0x3000, 4) == std::vector<std::byte>(sentinel.begin(), sentinel.end()), "memset bypassed guest write permission");
    session.machine.Protect(0x3000, 4096, Permission::Write);
    session.setup("j4ViWNHEgww", 0x3000);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "permission");
    session.setup("Q3VBxCXhUHs", std::numeric_limits<std::uint64_t>::max() - 1, 0x2000, 4);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "overflows");
    session.setup("8zTFvBIAIN8", 0x2000, 0, 16 * 1024 * 1024 + 1);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "16 MiB");
}

void exits() {
    for (const auto [argument, expected] : std::array<std::pair<std::uint64_t, int>, 2>{{
             {0x123, 35}, {0xffffffffffffffff, 255}}}) {
        Session session;
        session.setup("uMei1W9uyNo", argument);
        require(session.machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Exit, "SCE exit returned to guest caller");
        require(session.machine.ExitCode() == expected, "SCE exit did not preserve low eight-bit process status");
    }
    Session callback;
    callback.setup("uMei1W9uyNo", 0x123);
    const auto callbackGate = callback.imports.ExitGate();
    callback.machine.Write(0x2000, std::as_bytes(std::span(&callbackGate, 1)));
    rejects([&] { callback.machine.Run(0x1000, 0x1011, 100); }, "Unsupported SCE entry termination callback");
}
}

int main() {
    try {
        memoryCalls();
        stringCalls();
        scopedBinding();
        manyScopedGates();
        memoryFailures();
        exits();
        std::cout << "PASS scoped SCE NID imports, actual x86 GOT calls, native libc, memory permissions, and exit\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
