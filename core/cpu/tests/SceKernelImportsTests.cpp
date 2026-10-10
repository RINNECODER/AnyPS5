#include <cpu/SceKernelImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceTls.hpp>
#include <SceTypes.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
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
        constexpr std::array<std::uint8_t, 3> store{0x48, 0x89, 0x10};
        machine.Write(0x1030, std::as_bytes(std::span(store)));
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
    require(session.call("1G3lF1Gg1k8", std::numeric_limits<std::uint64_t>::max(), 0) == 0xffffffff8002000eULL,
            "Overflowing kernel open pathname did not return signed EFAULT to actual x86 caller");
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

#if ANYPS5_CPU_MODERN_TCG
void tscGates() {
    Session session;
    constexpr std::array<std::uint8_t, 9> rdtsc{0x0f, 0x31, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0};
    session.machine.Write(0x1040, std::as_bytes(std::span(rdtsc)));
    const auto guestTsc = [&] {
        require(session.machine.Run(0x1040, 0x1049, 10) == Cpu::StopReason::Address, "Guest RDTSC did not execute");
        return session.machine.Get(Register::Rax);
    };
    const auto frequency = session.call("1j3S3n-tTW4", 0);
    require(frequency >= 1000000000ULL && frequency <= 4000000000ULL,
            "sceKernelGetTscFrequency did not report a plausible PS5 TSC frequency");
    const auto before = session.call("-2IRUCO--PM", 0);
    const auto guest = guestTsc();
    const auto after = session.call("-2IRUCO--PM", 0);
    require(before < guest && guest < after, "sceKernelReadTsc and guest RDTSC are not one monotonic clock");
    const auto start = std::chrono::steady_clock::now();
    const auto first = guestTsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto second = guestTsc();
    const auto elapsed = std::chrono::duration<long double>(std::chrono::steady_clock::now() - start).count();
    const auto ticks = static_cast<long double>(second - first);
    require(second > first && ticks >= 0.019L * frequency && ticks <= elapsed * frequency * 1.01L,
            "Guest RDTSC does not advance at the sceKernelGetTscFrequency rate");
}
#endif
}

void sanitizerGates() {
    // The CPU runtime's tables must carry the size the prx ABI structure actually has.
    static_assert(sizeof(void*) == 8);
    static_assert(sizeof(MallocReplace) == 112 && sizeof(NewReplace) == 104);
    static_assert(offsetof(MallocReplace, malloc) == 24 && offsetof(NewReplace, new_p) == 8);
    Session session;
    // libc.prx of the reference title imports sceKernelGetSanitizerNewReplaceExternal while its
    // dependencies initialise, so an unregistered libkernel sanitizer NID stops the whole title in
    // dependency init. These NIDs are the SHA1 form of the names (tools/nid_names.py).
    require(session.call("jh+8XiK4LeE", 0) == 0, "sceKernelIsAddressSanitizerEnabled claimed an active sanitizer");
    const auto mallocTable = session.call("py6L8jiVAN8", 0);
    const auto newTable = session.call("bnZxYgAFeA0", 0);
    require(mallocTable && newTable && mallocTable != newTable,
            "Sanitizer replacement getters did not return distinct guest tables");
    require(newTable - mallocTable == sizeof(MallocReplace),
            "Sanitizer replacement tables are not laid out at the mirrored structure boundaries");
    require(mallocTable == session.call("py6L8jiVAN8", 0) && newTable == session.call("bnZxYgAFeA0", 0),
            "Sanitizer replacement tables changed identity between calls");
    const auto words = [&](std::uint64_t address, std::size_t count) {
        std::vector<std::uint64_t> values(count);
        session.machine.Read(address, std::as_writable_bytes(std::span(values)));
        return values;
    };
    // A real guest store through the returned pointer, not a host-side write: the getter hands
    // out the caller's registration table, so the page must be guest-writable (review finding).
    const auto storeWord = [&](std::uint64_t address, std::uint64_t value) {
        session.machine.Set(Register::Rsp, 0x4ff0);
        session.machine.Set(Register::Rax, address);
        session.machine.Set(Register::Rdx, value);
        require(session.machine.Run(0x1030, 0x1033, 10) == Cpu::StopReason::Address,
                "Actual x86 guest store through the sanitizer replacement table did not reach its stop");
    };
    // MallocReplace and NewReplace start with their own size, then only null entry points, which
    // is how the prx side reports "no sanitizer installed" so libc keeps its allocator.
    const auto malloc = words(mallocTable, sizeof(MallocReplace) / 8);
    const auto replacement = words(newTable, sizeof(NewReplace) / 8);
    require(malloc.front() == sizeof(MallocReplace) && replacement.front() == sizeof(NewReplace),
            "Sanitizer replacement tables lost their own size field");
    require(std::all_of(malloc.begin() + 1, malloc.end(), [](std::uint64_t value) { return value == 0; }) &&
            std::all_of(replacement.begin() + 1, replacement.end(), [](std::uint64_t value) { return value == 0; }),
            "Sanitizer replacement tables published non-null hook entry points");
    // The size field must be readable by real guest x86 through the returned pointer.
    session.callGate(session.imports->Resolve(import("py6L8jiVAN8")), 0);
    require(session.machine.Run(0x1020, 0x1023, 10) == Cpu::StopReason::Address &&
            session.machine.Get(Register::Rax) == sizeof(MallocReplace),
            "Actual x86 caller could not dereference the returned malloc replacement size");
    // Registration data is writable guest storage, never executable gate storage.
    session.machine.CheckAccess(mallocTable, sizeof(MallocReplace), rw);
    session.machine.CheckAccess(newTable, sizeof(NewReplace), rw);
    rejects([&] { session.machine.CheckAccess(mallocTable, 1, Permission::Execute); }, "permission");
    rejects([&] { session.machine.CheckAccess(newTable, 1, Permission::Execute); }, "permission");
    const auto hookSlot = mallocTable + offsetof(MallocReplace, malloc);
    storeWord(hookSlot, 0x1000);
    require(words(mallocTable, sizeof(MallocReplace) / 8)[offsetof(MallocReplace, malloc) / 8] == 0x1000,
            "Sanitizer replacement table did not accept an actual guest registration store");
    storeWord(hookSlot, 0);
    require(words(mallocTable, sizeof(MallocReplace) / 8)[offsetof(MallocReplace, malloc) / 8] == 0,
            "Sanitizer replacement registration could not be cleared by the guest");
    require(session.call("jh+8XiK4LeE", 0) == 0, "Sanitizer state changed after replacement registration");
}

int main() {
    try {
        fileGates();
        scopedGates();
        tlsGate();
        sanitizerGates();
#if ANYPS5_CPU_MODERN_TCG
        tscGates();
#endif
        std::cout << "PASS scoped kernel NID gates, actual x86 SysV file calls, signed errors, TLS and sanitizer replacement tables\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
