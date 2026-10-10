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
    // A transfer whose tail is unmapped is refused before any byte changes, however large.
    session.machine.Protect(0x3000, 4096, rw);
    session.machine.Write(0x3000, sentinel);
    session.setup("8zTFvBIAIN8", 0x3000, 0x55, 64 * 1024 * 1024);
    rejects([&] { session.machine.Run(0x1000, 0x1011, 100); }, "Guest access denied");
    require(session.read(0x3000, 4) == std::vector<std::byte>(sentinel.begin(), sentinel.end()),
            "Large memset with an unmapped tail partially changed guest memory");
}

// Transfers over 16 MiB are carried out in bounded chunks with no total cap (PLAT-22).
void largeTransfers() {
    Session session;
    constexpr std::uint64_t Base = 0x10000000;
    constexpr std::uint64_t Other = 0x12000000;
    constexpr std::size_t Size = 16 * 1024 * 1024 + 4097;
    session.machine.Map(Base, 24 * 1024 * 1024, rw);
    session.machine.Map(Other, 24 * 1024 * 1024, rw);
    std::vector<std::byte> pattern(Size);
    for (std::size_t index = 0; index < Size; ++index)
        pattern[index] = static_cast<std::byte>((index * 167 + (index >> 13) * 11 + (index >> 24)) & 255);
    const std::array tail{std::byte{0xe1}, std::byte{0xe2}};
    session.machine.Write(Base + Size, tail);
    require(session.call("8zTFvBIAIN8", Base, 0x15a, Size) == Base, "Large memset returned wrong guest pointer");
    const auto set = session.read(Base, Size + 2);
    require(std::all_of(set.begin(), set.begin() + Size, [](std::byte value) { return value == std::byte{0x5a}; }) &&
            set[Size] == tail[0] && set[Size + 1] == tail[1], "Large memset wrote wrong bytes or past its length");
    session.machine.Write(Base, pattern);
    require(session.call("Q3VBxCXhUHs", Other, Base, Size) == Other && session.read(Other, Size) == pattern,
            "Large memcpy copied wrong bytes");
    // Overlapping memmove in both directions across chunk boundaries.
    require(session.call("+P6FRGH4LfA", Base + 3, Base, Size) == Base + 3 && session.read(Base + 3, Size) == pattern,
            "Large forward-overlapping memmove did not preserve the source");
    require(session.call("+P6FRGH4LfA", Base, Base + 3, Size) == Base && session.read(Base, Size) == pattern,
            "Large backward-overlapping memmove did not preserve the source");
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
        memoryFailures();
        largeTransfers();
        exits();
        std::cout << "PASS scoped SCE NID imports, actual x86 GOT calls, native libc, memory permissions, and exit\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
