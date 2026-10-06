#include <cpu/SceKernelImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceTls.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using Cpu::Register;
using Cpu::Permission;
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
    throw std::runtime_error(std::string("Missing kernel import rejection: ") + expected);
}

Cpu::SceImport import(const char* nid) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = "libkernel";
    value.LibraryId = 7;
    value.ModuleName = "libkernel";
    value.ModuleId = 11;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Resources {
    std::filesystem::path directory;
    Resources() {
        std::array<char, 64> pattern{};
        const std::string value = "/tmp/anyps5-kernel-imports-XXXXXX";
        std::copy(value.begin(), value.end(), pattern.begin());
        const auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create kernel import resource fixture");
        directory = created;
        constexpr std::array<unsigned char, 8> bytes{0x00, 0x80, 0xff, 0x19, 0xa3, 0x67, 0x2d, 0x11};
        std::ofstream stream(directory / "data.bin", std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(stream.good(), "Cannot write independent kernel import resource bytes");
    }
    ~Resources() { std::error_code error; std::filesystem::remove_all(directory, error); }
};

struct Session {
    Resources resources;
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceKernelImports> imports = std::make_unique<Cpu::SceKernelImports>(machine, resources.directory);

    Session() {
        machine.Map(0x1000, 4096, rx);
        machine.Map(0x2000, 4096, rw);
        machine.Map(0x3000, 4096, rw);
        machine.Map(0x4000, 4096, rw);
        machine.Map(0x5000, 4096, rw);
        constexpr std::array<std::uint8_t, 17> program{
            0xff, 0x15, 0xfa, 0x0f, 0x00, 0x00,
            0x48, 0x89, 0x05, 0x03, 0x10, 0x00, 0x00,
            0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(program)));
        constexpr std::array<std::uint8_t, 3> dereference{0x48, 0x8b, 0x00};
        machine.Write(0x1020, std::as_bytes(std::span(dereference)));
        const std::string path = "/app0/data.bin";
        machine.Write(0x3000, std::as_bytes(std::span(path.c_str(), path.size() + 1)));
    }

    std::uint64_t callGate(std::uint64_t gate, std::uint64_t first, std::uint64_t second = 0,
                           std::uint64_t third = 0, std::uint64_t fourth = 0) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        machine.Set(Register::Rdi, first);
        machine.Set(Register::Rsi, second);
        machine.Set(Register::Rdx, third);
        machine.Set(Register::Rcx, fourth);
        machine.Set(Register::Rsp, 0x4ff0);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "Kernel gate did not return to actual x86 GOT caller");
        require(machine.Get(Register::Rsp) == 0x4ff0 && machine.Get(Register::Rbx) == 0x123456789abcdf00,
                "Kernel gate corrupted x86 CALL/RET stack or callee-saved continuation");
        std::uint64_t result = 0;
        machine.Read(0x2010, std::as_writable_bytes(std::span(&result, 1)));
        require(result == machine.Get(Register::Rax), "Guest did not store kernel gate return register");
        return result;
    }

    std::uint64_t call(const char* nid, std::uint64_t first, std::uint64_t second = 0,
                       std::uint64_t third = 0, std::uint64_t fourth = 0) {
        return callGate(imports->Resolve(import(nid)), first, second, third, fourth);
    }

    std::vector<std::uint8_t> bytes(std::uint64_t address, std::size_t size) {
        std::vector<std::uint8_t> result(size);
        machine.Read(address, std::as_writable_bytes(std::span(result)));
        return result;
    }
};

void fileGates() {
    Session session;
    const auto descriptor = session.call("1G3lF1Gg1k8", 0x3000, 0, 0666);
    require(descriptor == 3, "Kernel open did not return a private guest descriptor");
    require(session.call("Cg4srZ6TKbU", descriptor, 0x5000, 3) == 3 &&
            session.bytes(0x5000, 3) == std::vector<std::uint8_t>{0, 0x80, 0xff}, "Kernel read failed typed guest buffer marshalling");
    session.machine.Set(Register::R10, 0);
    require(session.call("+r3rMFwItV4", descriptor, 0x5010, 2, 5) == 2 &&
            session.bytes(0x5010, 2) == std::vector<std::uint8_t>{0x67, 0x2d}, "Kernel pread did not marshal fourth SysV argument from RCX");
    require(session.call("oib76F-12fk", descriptor, 0, 1) == 3, "Kernel pread changed sequential descriptor position");
    require(session.call("oib76F-12fk", descriptor, static_cast<std::uint64_t>(-1), 2) == 7, "Kernel lseek lost signed offset");
    session.machine.Protect(0x5000, 4096, Permission::Read);
    require(session.call("Cg4srZ6TKbU", descriptor, 0x5000, 1) == 0xffffffff8002000eULL,
            "Kernel read did not preserve full signed EFAULT in RAX");
    require(session.call("oib76F-12fk", descriptor, 0, 1) == 7, "Rejected kernel read consumed file bytes");
    session.machine.Protect(0x5000, 4096, rw);
    require(session.call("Cg4srZ6TKbU", descriptor, 0x5000, 8) == 1 && session.bytes(0x5000, 1) == std::vector<std::uint8_t>{0x11},
            "Kernel read fabricated bytes beyond resource EOF");
    require(session.call("UK2Tl2DWUns", descriptor) == 0, "Kernel close gate failed");
    require(session.call("UK2Tl2DWUns", descriptor) == 0xffffffff80020009ULL,
            "Kernel close did not sign-extend EBADF into RAX");
    rejects([&] { session.call("1G3lF1Gg1k8", 0x3000, 2); }, "Unsupported guest /app0 open flags");
}

void scopedGates() {
    Session session;
    const auto original = import("1G3lF1Gg1k8");
    const auto gate = session.imports->Resolve(original);
    require(session.imports->Resolve(original) == gate, "Repeated full kernel import identity changed guest gate");
    session.machine.CheckAccess(gate, 1, Permission::Execute);
    rejects([&] { session.machine.CheckAccess(gate, 1, Permission::Write); }, "permission");
    auto other = original;
    other.LibraryId = 12;
    other.ModuleId = 29;
    const auto localGate = session.imports->Resolve(other);
    require(localGate != gate && session.callGate(localGate, 0x3000) == 3,
            "Kernel importer-local IDs were discarded or incorrectly used as global identities");
    for (unsigned mismatch = 0; mismatch < 5; ++mismatch) {
        auto wrong = original;
        switch (mismatch) {
        case 0: wrong.LibraryName = "libScePosix"; break;
        case 1: wrong.ModuleName = "other"; break;
        case 2: wrong.LibraryVersion = 2; break;
        case 3: wrong.ModuleMajor = 2; break;
        case 4: wrong.ModuleMinor = 2; break;
        }
        rejects([&] { session.imports->Resolve(wrong); }, "scope/version");
    }
    auto unknown = original;
    unknown.Nid = "AAAAAAAAAAA";
    rejects([&] { session.imports->Resolve(unknown); }, "Unsupported SCE kernel import service");
    rejects([&] { Cpu::SceKernelImports duplicate(session.machine, session.resources.directory); }, "Map guest memory");
    session.imports.reset();
    rejects([&] { session.callGate(gate, 0x3000); }, "runtime has expired");
}

void tlsGate() {
    Session session;
    const auto gate = session.imports->Resolve(import("vNe1w4diLCs"));
    rejects([&] { session.callGate(gate, 0x3010); }, "before main-module TLS is configured");
    constexpr std::array<std::uint8_t, 16> initial{0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                                  0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87};
    auto tls = std::make_shared<Cpu::SceTls>(session.machine, std::as_bytes(std::span(initial)), 64, 16, 0x8000);
    session.imports->SetTls(tls);
    std::array<std::uint64_t, 2> index{1, 8};
    session.machine.Write(0x3010, std::as_bytes(std::span(index)));
    require(session.callGate(gate, 0x3010) == 0x8008, "Kernel TLS gate returned host pointer or wrong main-module offset");
    require(session.machine.Run(0x1020, 0x1023, 10) == Cpu::StopReason::Address &&
            session.machine.Get(Register::Rax) == 0x8786858483828180ULL,
            "Actual x86 caller could not dereference returned initialized TLS address");
    require(session.machine.Get(Register::FsBase) == 0x8040, "Kernel TLS gate changed guest TCB base");
    index[1] = 24;
    session.machine.Write(0x3010, std::as_bytes(std::span(index)));
    require(session.callGate(gate, 0x3010) == 0x8018, "Kernel TLS gate mislocated uninitialized storage");
    require(session.machine.Run(0x1020, 0x1023, 10) == Cpu::StopReason::Address && session.machine.Get(Register::Rax) == 0,
            "TLS BSS was not zero for actual guest dereference");
    index = {2, 0};
    session.machine.Write(0x3010, std::as_bytes(std::span(index)));
    rejects([&] { session.callGate(gate, 0x3010); }, "Unsupported SCE TLS module ID 2");
    index = {1, 64};
    session.machine.Write(0x3010, std::as_bytes(std::span(index)));
    rejects([&] { session.callGate(gate, 0x3010); }, "outside the main module");
    rejects([&] { session.callGate(gate, 0x9ff8); }, "Guest access denied");
    session.imports->SetTls(nullptr);
    rejects([&] { session.callGate(gate, 0x3010); }, "before main-module TLS is configured");
}
}

int main() {
    try {
        fileGates();
        scopedGates();
        tlsGate();
        std::cout << "PASS scoped kernel NID gates, actual x86 SysV file calls, signed errors and TLS pointer dereferences\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
