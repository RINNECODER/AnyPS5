#include "../NetworkServices.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace {
using Cpu::Register;
using Cpu::Permission;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class F> void rejects(F&& f, std::string_view reason) {
    try { f(); }
    catch (const std::exception& error) {
        require(std::string_view(error.what()).find(reason) != std::string_view::npos, error.what());
        return;
    }
    throw std::runtime_error("Expected HTTP component rejection");
}
Cpu::SceImport identity() {
    Cpu::SceImport i;
    i.Nid = Cpu::Platform::NetworkServices::UriEscapeNid;
    i.LibraryName = i.ModuleName = "libSceHttp";
    i.LibraryId = 3; i.ModuleId = 4;
    i.LibraryVersion = i.ModuleMajor = i.ModuleMinor = 1;
    return i;
}
struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::Platform::NetworkServices> imports;
    std::uint64_t gate;
    Session() {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "HTTP fixture requires modern native TCG");
        imports = std::make_unique<Cpu::Platform::NetworkServices>(machine);
        gate = imports->Resolve(identity(), 2).value();
        machine.Map(0x100000, 0x10000, rx);
        machine.Map(0x110000, 0x10000, rx);
        machine.Map(0x200000, 0x10000, rw);
        machine.Map(0x300000, 0x10000, rw);
        const std::array<std::byte, 3> call{std::byte{0xff}, std::byte{0xd0}, std::byte{0x90}};
        machine.Write(0x100000, call); // call rax; guest returns to byte 2
        const std::array<unsigned char, 11> input{'A','z','0','-','_','.','~',' ','/',255,0};
        machine.Write(0x301000, std::as_bytes(std::span(input)));
        fill(0x302000, 64);
        fill(0x303000, 64);
    }
    void fill(std::uint64_t p, std::size_t n) {
        const std::vector<std::byte> v(n, std::byte{0xa7}); machine.Write(p, v);
    }
    std::vector<std::byte> read(std::uint64_t p, std::size_t n) {
        std::vector<std::byte> v(n); machine.Read(p, v); return v;
    }
    std::uint64_t call(std::uint64_t out, std::uint64_t needed, std::uint64_t size, std::uint64_t in) {
        machine.Set(Register::Rax, gate);
        machine.Set(Register::Rdi, out); machine.Set(Register::Rsi, needed);
        machine.Set(Register::Rdx, size); machine.Set(Register::Rcx, in);
        machine.Set(Register::Rsp, 0x20fff0);
        require(machine.Run(0x100000, 0x100002, 100) == Cpu::StopReason::Address, "URI call did not return");
        require(machine.Get(Register::Rsp) == 0x20fff0, "URI call corrupted guest stack");
        return static_cast<std::uint32_t>(machine.Get(Register::Rax));
    }
};
void scopes() {
    Session s;
    require(s.imports->Resolve(identity(), 2).value() == s.gate, "Repeated import changed gate");
    auto i = identity(); i.Nid = "unknown";
    require(!s.imports->Resolve(i, 2), "Unknown NID admitted");
    i = identity(); i.LibraryName = i.ModuleName = "foreign";
    require(!s.imports->Resolve(i, 2), "Foreign import admitted");
    for (int field = 0; field != 5; ++field) {
        i = identity();
        if (field == 0) i.LibraryName = "foreign";
        if (field == 1) i.ModuleName = "foreign";
        if (field == 2) ++i.LibraryVersion;
        if (field == 3) ++i.ModuleMajor;
        if (field == 4) ++i.ModuleMinor;
        rejects([&] { s.imports->Resolve(i, 2); }, "scope/version");
    }
    for (auto type : {0, 1, 3}) rejects([&] { s.imports->Resolve(identity(), type); }, "symbol type");
}
void guestFixture(const char* path) {
    Session s;
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "Cannot open flat guest .text");
    std::vector<char> code{std::istreambuf_iterator<char>(stream), {}};
    require(!code.empty() && code.size() <= 0x10000, "Invalid guest .text size");
    s.machine.Write(0x110000, std::as_bytes(std::span(code)));
    const std::array<std::uint64_t, 5> args{s.gate, 0x302000, 0x301000, 0x303000, 0x304000};
    s.machine.Write(0x300000, std::as_bytes(std::span(args)));
    constexpr std::uint64_t stop = 0x10fff0;
    s.machine.Write(0x20ffe8, std::as_bytes(std::span(&stop, 1)));
    s.machine.Set(Register::Rdi, 0x300000); s.machine.Set(Register::Rsp, 0x20ffe8);
    require(s.machine.Run(0x110000, stop, 100000) == Cpu::StopReason::Address, "Compiled guest did not return");
    require(s.machine.Get(Register::Rax) == 0, "Compiled guest output/error oracle failed");
    std::array<std::uint64_t, 6> receipt{};
    s.machine.Read(0x304000, std::as_writable_bytes(std::span(receipt)));
    require(receipt == std::array<std::uint64_t, 6>{0x4e45545552494f4bULL,0,17,0x80431022,0,0x804311fe},
            "Independent guest receipt mismatch");
    auto expected = std::vector<std::byte>(64, std::byte{0xa7});
    constexpr char literal[] = "Az0-_.~%20%2F%FF";
    std::memcpy(expected.data(), literal, sizeof(literal));
    require(s.read(0x302000,64) == expected, "HTTP exact bytes, NUL or untouched tail mismatch");
}
void pointerControls() {
    Session s;
    require(s.call(0xdead000, 0x303000, 1, 0x301000) == 0x80431022, "Small capacity accessed unused output");
    std::uint64_t required = 0;
    s.machine.Read(0x303000, std::as_writable_bytes(std::span(&required,1)));
    require(required == 17, "Small capacity did not publish required size");
    require(s.call(0,0,~0ULL,0x301000) == 0, "Size query without size destination failed");
    require(s.call(0xdead000,0xdead000,0,0) == 0x804311fe, "Null input accessed unused destinations");
    rejects([&] { s.call(0x302000,0x303000,64,0x500000); }, "Guest access denied");
    rejects([&] { s.call(0x302000,0x303000,64,0xffffffffffffffffULL); }, "invalid guest span");
    s.machine.Protect(0x300000,0x10000,Permission::Read);
    rejects([&] { s.call(0x302000,0,64,0x301000); }, "Guest access denied");
    rejects([&] { s.call(0,0x303000,0,0x301000); }, "Guest access denied");
    s.machine.Protect(0x300000,0x10000,rw);
    s.fill(0x303000,64);
    rejects([&] { s.call(0x30fff8,0x303000,64,0x301000); }, "Guest access denied");
    require(s.read(0x303000,64)==std::vector<std::byte>(64,std::byte{0xa7}), "Invalid output modified required size");
    s.fill(0x30fff8,8);
    rejects([&] { s.call(0x302000,0x303000,64,0x30fff8); }, "Guest access denied");
    require(s.read(0x302000,64)==std::vector<std::byte>(64,std::byte{0xa7}), "Unterminated input modified output");
    s.fill(0x308000,0x4000);
    rejects([&] { s.call(0x302000,0x303000,64,0x308000); }, "beyond 16383");
    constexpr std::array<unsigned char,3> input{'/',255,0};
    s.machine.Write(0x302000,std::as_bytes(std::span(input)));
    require(s.call(0x302000,0x303000,7,0x302000)==0,"Aliased input/output escape failed");
    constexpr char alias[]="%2F%FF";
    require(s.read(0x302000,7)==std::vector<std::byte>(reinterpret_cast<const std::byte*>(alias),
            reinterpret_cast<const std::byte*>(alias)+sizeof(alias)),"Aliased input was overwritten before capture");
    s.imports.reset();
    rejects([&] { s.machine.CheckAccess(s.gate, 1, Permission::Execute); }, "Guest access denied");
    rejects([&] { s.call(0,0,0,0x301000); }, "Guest unmapped instruction fetch");
    s.imports = std::make_unique<Cpu::Platform::NetworkServices>(s.machine);
    s.gate = s.imports->Resolve(identity(), 2).value();
    s.fill(0x302000, 64);
    require(s.call(0x302000, 0x303000, 17, 0x301000) == 0, "Replacement provider did not execute helper");
    constexpr char replacement[] = "Az0-_.~%20%2F%FF";
    auto expected = std::vector<std::byte>(64, std::byte{0xa7});
    std::memcpy(expected.data(), replacement, sizeof(replacement));
    require(s.read(0x302000, 64) == expected, "Replacement retained stale gate callback or wrong URI output");
}
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"Usage: NetworkServicesTest flat-guest-text");
        scopes(); guestFixture(argv[1]); pointerControls();
        std::cout << "NetworkServicesTest: PASS (compiled x86 guest URI output/error and memory/scope/lifetime controls)\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
