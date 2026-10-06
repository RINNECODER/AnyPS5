#include <cpu/SceNetAddressImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr const char* htonl = "9T2pDF2Ryqg";
constexpr const char* htons = "iWQWrwiSt8A";
constexpr const char* ntop = "9vA2aW+CHuA";
constexpr const char* pton = "8Kcp5d-q1Uo";

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function> void rejects(Function&& function, std::string_view expected = {}) {
    try { function(); }
    catch (const std::exception& error) {
        require(expected.empty() || std::string_view(error.what()).find(expected) != std::string_view::npos,
                error.what());
        return;
    }
    throw std::runtime_error("Expected network-address gate rejection");
}

Cpu::SceImport qualified(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libSceNet";
    value.ModuleName = "libSceNet";
    value.LibraryId = 21;
    value.ModuleId = 24;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceNetAddressImports> imports =
        std::make_unique<Cpu::SceNetAddressImports>(machine, 0x7ffdf3000000ULL);

    Session() {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "Network-address fixture requires the native modern TCG backend");
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x6000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0,
            0x48, 0x89, 0x05, 0x03, 0x10, 0, 0,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }

    std::uint64_t callGate(std::uint64_t gate, std::uint64_t first = 0, std::uint64_t second = 0,
                           std::uint64_t third = 0, std::uint64_t fourth = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        machine.Set(Register::Rdi, first);
        machine.Set(Register::Rsi, second);
        machine.Set(Register::Rdx, third);
        machine.Set(Register::Rcx, fourth);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        constexpr std::array saved{Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (auto reg : saved) machine.Set(reg, 0x9876543210123456);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address,
                "Network gate did not return through the actual x86 CALL/RET caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "Network gate corrupted the guest stack or skipped its continuation");
        for (auto reg : saved)
            require(machine.Get(reg) == 0x9876543210123456, "Network gate corrupted a callee-saved register");
        std::uint64_t result = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&result, 1)));
        require(result == machine.Get(Register::Rax), "Guest did not store the actual network gate RAX result");
        return result;
    }

    std::uint64_t call(const char* nid, std::uint64_t first = 0, std::uint64_t second = 0,
                       std::uint64_t third = 0, std::uint64_t fourth = 0) {
        return callGate(imports->Resolve(qualified(nid)).value(), first, second, third, fourth);
    }

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t count) {
        std::vector<std::uint8_t> result(count);
        machine.Read(address, std::as_writable_bytes(std::span(result)));
        return result;
    }

    void fill(std::uint64_t address, std::size_t count) {
        const std::vector<std::uint8_t> sentinel(count, 0xa7);
        machine.Write(address, std::as_bytes(std::span(sentinel)));
    }

    void text(std::uint64_t address, const char* value) {
        const std::string_view input(value);
        machine.Write(address, std::as_bytes(std::span(value, input.size() + 1)));
    }
};

void scalarWidthsAndByteOrder() {
    Session session;
    for (const auto [input, expected] : std::array<std::array<std::uint64_t, 2>, 4>{{
             {0, 0}, {0x01234567, 0x67452301}, {0xaabbccdd80000001ULL, 0x01000080},
             {0xffffffffffffffffULL, 0xffffffff}}})
        require(session.call(htonl, input) == expected, "Htonl did not swap and zero-extend exactly its low 32 bits");
    for (const auto [input, expected] : std::array<std::array<std::uint64_t, 2>, 4>{{
             {0, 0}, {0x1234, 0x3412}, {0xaabbccdd11228001ULL, 0x0180},
             {0xffffffffffffffffULL, 0xffff}}})
        require(session.call(htons, input) == expected, "Htons did not swap and zero-extend exactly its low 16 bits");
}

void ipv4TextAndGuestPointer() {
    Session session;
    struct Case { std::array<std::uint8_t, 4> input; const char* output; std::uint32_t capacity; };
    for (const auto& value : std::array<Case, 3>{{
             {{192, 0, 2, 129}, "192.0.2.129", 12}, {{0, 0, 0, 0}, "0.0.0.0", 8},
             {{255, 255, 255, 255}, "255.255.255.255", 16}}}) {
        session.machine.Write(0x2200, std::as_bytes(std::span(value.input)));
        session.fill(0x3000, 34);
        require(session.call(ntop, 0xffff000000000002ULL, 0x2200, 0x3001,
                             0xffff000000000000ULL | value.capacity) == 0x3001,
                "InetNtop did not return the original guest destination pointer");
        auto expected = std::vector<std::uint8_t>(34, 0xa7);
        const std::string_view literal(value.output);
        for (std::size_t i = 0; i < literal.size(); ++i) expected[i + 1] = literal[i];
        expected[literal.size() + 1] = 0;
        require(session.bytes(0x3000, 34) == expected, "InetNtop literal, NUL, capacity tail or guards differ");
    }
    constexpr std::array<std::uint8_t, 4> input{0, 0, 0, 0};
    session.fill(0x3000, 10);
    session.machine.Write(0x3001, std::as_bytes(std::span(input)));
    require(session.call(ntop, 2, 0x3001, 0x3001, 8) == 0x3001,
            "InetNtop failed when source and exact-size destination alias");
    require(session.bytes(0x3000, 10) == std::vector<std::uint8_t>{0xa7,'0','.','0','.','0','.','0',0,0xa7},
            "InetNtop did not preserve guards or capture aliased source before writing");
}

void ipv4ParsingAndAliasing() {
    Session session;
    struct Case { const char* input; std::array<std::uint8_t, 4> output; };
    for (const auto& value : std::array<Case, 3>{{
             {"192.0.2.129", {192, 0, 2, 129}}, {"0.0.0.0", {0, 0, 0, 0}},
             {"255.255.255.255", {255, 255, 255, 255}}}}) {
        session.text(0x6000, value.input);
        session.fill(0x3000, 6);
        require(session.call(pton, 2, 0x6000, 0x3001) == 1, "Valid IPv4 InetPton did not return one");
        require(session.bytes(0x3000, 6) == std::vector<std::uint8_t>{0xa7, value.output[0], value.output[1],
                value.output[2], value.output[3], 0xa7}, "InetPton did not write exactly four network-order bytes");
    }
    for (const auto* text : {"", "256.0.2.129", "192.0.2.129garbage", "255.255.255.255x"}) {
        session.text(0x6000, text);
        session.fill(0x3000, 6);
        require(session.call(pton, 2, 0x6000, 0x3001) == 0,
                "Malformed readable IPv4 text did not return zero");
        require(session.bytes(0x3000, 6) == std::vector<std::uint8_t>(6, 0xa7),
                "Malformed IPv4 text changed output or its guards");
        require(session.call(pton, 2, 0x6000, 0xa000) == 0,
                "Malformed IPv4 text accessed an unused output pointer");
    }
    session.text(0x6fff, "");
    rejects([&] { session.call(pton, 2, 0x6fff, 0); },
            "Unsupported SCE network address null destination: guest errno is unavailable");
    require(session.call(pton, 2, 0x6fff, 0x3001) == 0,
            "Empty IPv4 text read past its last-mapped-byte terminator");
    constexpr std::array<std::uint8_t, 16> overlong{
        '2','5','5','.','2','5','5','.','2','5','5','.','2','5','5','x'};
    session.machine.Write(0x6ff0, std::as_bytes(std::span(overlong)));
    require(session.call(pton, 2, 0x6ff0, 0x3001) == 0,
            "Overlong readable IPv4 text read beyond its bounded guest span");
    require(session.bytes(0x3000, 6) == std::vector<std::uint8_t>(6, 0xa7),
            "Boundary malformed IPv4 text changed output or its guards");
    session.text(0x3000, "192.0.2.129");
    require(session.call(pton, 2, 0x3000, 0x3000) == 1 &&
            session.bytes(0x3000, 4) == std::vector<std::uint8_t>{192, 0, 2, 129},
            "InetPton did not capture aliased text before writing");
}

void checkedMemoryAndFailureAtomicity() {
    Session session;
    constexpr std::array<std::uint8_t, 4> address{192, 0, 2, 129};
    session.machine.Write(0x2200, std::as_bytes(std::span(address)));
    session.text(0x6000, "192.0.2.129");
    session.text(0x6ff4, "192.0.2.129");
    require(session.call(pton, 2, 0x6ff4, 0x3000) == 1 &&
            session.bytes(0x3000, 4) == std::vector<std::uint8_t>{192, 0, 2, 129},
            "InetPton read beyond a NUL at the last mapped source byte");
    session.fill(0x3000, 32);
    constexpr std::array<std::uint8_t, 7> edgePrefix{'1','9','2','.','0','.','2'};
    session.machine.Write(0x6ff9, std::as_bytes(std::span(edgePrefix)));
    rejects([&] { session.call(pton, 2, 0x6ff9, 0x3000); }, "Guest access denied");
    require(session.bytes(0x3000, 32) == std::vector<std::uint8_t>(32, 0xa7),
            "InetPton published output before finding its source terminator");
    session.fill(0x6ff0, 16);
    rejects([&] { session.call(ntop, 2, 0x2200, 0x6ff0, 32); });
    require(session.bytes(0x6ff0, 16) == std::vector<std::uint8_t>(16, 0xa7),
            "InetNtop wrote a fitting text prefix before checking its full capacity");
    session.fill(0x6ffe, 2);
    session.fill(0x3000, 32);
    rejects([&] { session.call(ntop, 2, 0x6ffe, 0x3000, 16); });
    require(session.bytes(0x3000, 32) == std::vector<std::uint8_t>(32, 0xa7),
            "InetNtop changed output before checking all four source bytes");
    rejects([&] { session.call(pton, 2, 0x6000, 0x6ffe); });
    require(session.bytes(0x6ffe, 2) == std::vector<std::uint8_t>(2, 0xa7),
            "InetPton wrote part of its four-byte output before rejecting an unmapped suffix");
    session.machine.Map(0x7000, 4096, Permission::Read);
    session.fill(0x6ff0, 32);
    rejects([&] { session.call(ntop, 2, 0x2200, 0x6ff0, 32); }, "permission");
    rejects([&] { session.call(pton, 2, 0x6000, 0x6ffe); }, "permission");
    require(session.bytes(0x6ff0, 32) == std::vector<std::uint8_t>(32, 0xa7),
            "Read-only output suffix allowed a partial network-address write");
    session.machine.Protect(0x7000, 4096, rw);
    require(session.call(ntop, 2, 0x2200, 0x6ff0, 32) == 0x6ff0,
            "InetNtop rejected a fully writable adjacent-mapping capacity");
    require(session.call(pton, 2, 0x6000, 0x6ffe) == 1 &&
            session.bytes(0x6ffe, 4) == std::vector<std::uint8_t>{192, 0, 2, 129},
            "InetPton rejected adjacent writable output mappings");
    session.fill(0x3000, 32);
    session.machine.Protect(0x6000, 4096, Permission::Write);
    rejects([&] { session.call(ntop, 2, 0x6000, 0x3000, 16); }, "permission");
    rejects([&] { session.call(pton, 2, 0x6000, 0x3000); }, "permission");
    require(session.bytes(0x3000, 32) == std::vector<std::uint8_t>(32, 0xa7),
            "Unreadable source changed network output");
    session.machine.Protect(0x6000, 4096, Permission::Read);
    require(session.call(pton, 2, 0x6000, 0x3000) == 1, "Read-only valid text source was rejected");
    for (const auto pointer : {std::uint64_t(0), std::uint64_t(0xa000),
                              std::numeric_limits<std::uint64_t>::max() - 1}) {
        session.fill(0x3000, 32);
        rejects([&] { session.call(ntop, 2, pointer, 0x3000, 16); });
        rejects([&] { session.call(pton, 2, pointer, 0x3000); });
        require(session.bytes(0x3000, 32) == std::vector<std::uint8_t>(32, 0xa7),
                "Invalid source pointer changed network output");
    }
    for (auto family : {0u, 28u}) {
        rejects([&] { session.call(pton, family, 0xa000, 0xa000); }, "Unsupported SCE network address family");
        rejects([&] { session.call(ntop, family, 0xa000, 0xa000, 16); }, "Unsupported SCE network address family");
    }
    for (auto capacity : {0u, 7u, 16u * 1024 * 1024 + 1})
        rejects([&] { session.call(ntop, 2, 0xa000, 0xa000, capacity); },
                "Unsupported SCE network address buffer capacity");
    constexpr std::array<std::uint8_t, 4> longest{255, 255, 255, 255};
    session.machine.Write(0x2200, std::as_bytes(std::span(longest)));
    rejects([&] { session.call(ntop, 2, 0x2200, 0x3000, 15); },
            "IPv4 text does not fit; guest errno is unavailable");
    session.machine.Write(0x2200, std::as_bytes(std::span(address)));
    require(session.bytes(0x3000, 32) == std::vector<std::uint8_t>(32, 0xa7),
            "Rejected network-address operation changed output");
    session.machine.Map(0x9000, 4096, Permission::Write);
    session.fill(0x9000, 32);
    require(session.call(ntop, 2, 0x2200, 0x9000, 32) == 0x9000,
            "InetNtop incorrectly required read permission on its output");
    session.machine.Protect(0x9000, 4096, Permission::Read);
    require(session.bytes(0x9000, 12) == std::vector<std::uint8_t>{'1','9','2','.','0','.','2','.','1','2','9',0},
            "Write-only destination did not receive the IPv4 literal");
    session.machine.Protect(0x9000, 4096, Permission::Write);
    session.text(0x6000, "192.0.2.129");
    require(session.call(pton, 2, 0x6000, 0x9000) == 1,
            "InetPton incorrectly required read permission on its output");
    session.machine.Protect(0x9000, 4096, Permission::Read);
    require(session.bytes(0x9000, 4) == std::vector<std::uint8_t>{192, 0, 2, 129},
            "Write-only destination did not receive network-order bytes");
}

void qualificationAndWeakLifetime() {
    Session session;
    const auto identity = qualified(htonl);
    const auto gate = session.imports->Resolve(identity).value();
    require(session.imports->Resolve(identity).value() == gate, "Repeated exact identity changed its gate");
    session.machine.CheckAccess(gate, 1, Permission::Execute);
    rejects([&] { session.machine.CheckAccess(gate, 1, Permission::Write); }, "permission");
    auto unrelated = identity;
    unrelated.LibraryName = "libc";
    unrelated.ModuleName = "libc";
    require(!session.imports->Resolve(unrelated), "Network resolver captured an unrelated library/module pair");
    auto unknown = identity;
    unknown.Nid = "AAAAAAAAAAA";
    require(!session.imports->Resolve(unknown), "Network resolver fabricated a gate for an unknown NID");
    for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
        auto wrong = identity;
        switch (mismatch) {
        case 0: wrong.LibraryName = "libc"; break;
        case 1: wrong.ModuleName = "libc"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 2; break;
        }
        rejects([&] { session.imports->Resolve(wrong); });
    }
    auto otherImporter = identity;
    otherImporter.LibraryId = 29;
    otherImporter.ModuleId = 31;
    const auto otherGate = session.imports->Resolve(otherImporter).value();
    require(otherGate != gate && session.callGate(otherGate, 0x01234567) == 0x67452301,
            "Importer-local identity did not retain a correctly bound independent gate");
    session.imports.reset();
    rejects([&] { session.callGate(gate, 0x01234567); }, "SCE network address import runtime has expired");
}
}

int main() {
    try {
        scalarWidthsAndByteOrder();
        ipv4TextAndGuestPointer();
        ipv4ParsingAndAliasing();
        checkedMemoryAndFailureAtomicity();
        qualificationAndWeakLifetime();
        std::cout << "PASS native x86 network-address gates, byte widths, IPv4 literals, checked memory and lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
