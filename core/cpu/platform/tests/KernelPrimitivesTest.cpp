#include "KernelPrimitives.hpp"
#include <cpu/SceElf.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using Cpu::Register;
using Cpu::Permission;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
template<class F> void rejects(F f, const char* expected) {
    try { f(); }
    catch (const std::exception& e) {
        require(std::string(e.what()).find(expected) != std::string::npos, e.what()); return;
    }
    throw std::runtime_error("Expected contract rejection");
}
Cpu::SceImport qualified(std::string_view nid) {
    Cpu::SceImport i; i.Nid = nid; i.LibraryName = "libkernel"; i.ModuleName = "libkernel";
    i.LibraryVersion = 1; i.ModuleMajor = 1; i.ModuleMinor = 1; i.LibraryId = 7; i.ModuleId = 11; return i;
}
struct Session {
    Cpu::Machine machine;
    std::uint64_t active = 1;
    std::unique_ptr<Cpu::Platform::KernelPrimitives> provider;
    std::array<std::uint64_t, 9> gates;
    explicit Session(const std::vector<std::byte>& code) {
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
                "Kernel fixture needs native modern QEMU TCG");
        machine.Map(0x10000, 4096, Permission::Read | Permission::Execute); machine.Write(0x10000, code);
        machine.Map(0x1000, 4096, Permission::Read | Permission::Execute);
        machine.Map(0x2000, 4096, Permission::Read | Permission::Write);
        machine.Map(0x4000, 4096, Permission::Read | Permission::Write);
        provider = std::make_unique<Cpu::Platform::KernelPrimitives>(machine, [&] { return active; });
        const auto inventory = Cpu::Platform::KernelPrimitiveInventory();
        for (unsigned i = 0; i < gates.size(); ++i) gates[i] = *provider->Resolve(qualified(inventory[i].Nid), 2);
        machine.Write(0x2100, std::as_bytes(std::span(gates)));
        const std::array name{'m','u','t','e','x','\0'};
        machine.Write(0x2250, std::as_bytes(std::span(name)));
    }
    void put(std::uint64_t p, std::uint64_t value) { machine.Write(p, std::as_bytes(std::span(&value, 1))); }
    std::uint64_t get(std::uint64_t p) {
        std::uint64_t value; machine.Read(p, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
    void run(unsigned mode) {
        put(0x4ff8, 0x1800);
        machine.Set(Register::Rsp, 0x4ff8); machine.Set(Register::Rdi, 0x2100);
        machine.Set(Register::Rsi, 0x2300); machine.Set(Register::Rdx, mode);
        constexpr std::array saved{Register::Rbx, Register::Rbp, Register::R12, Register::R13, Register::R14, Register::R15};
        for (auto r : saved) machine.Set(r, 0x1234567890abcdef);
        require(machine.Run(0x10000, 0x1800, 10000) == Cpu::StopReason::Address, "Compiled kernel caller did not return");
        require(machine.Get(Register::Rsp) == 0x5000, "Kernel probe stack imbalance");
        for (auto r : saved) require(machine.Get(r) == 0x1234567890abcdef, "Kernel probe corrupted callee saved register");
    }
    std::uint64_t call(unsigned op, std::uint64_t slot, std::uint64_t arg = 0, std::uint64_t label = 0) {
        put(0x2400, slot); put(0x2408, arg); put(0x2410, label); run(op + 1); return get(0x2300);
    }
};
void check(const std::vector<std::byte>& code) {
    Session s(code); s.run(0);
    constexpr std::array<std::uint64_t, 24> expected{
        0,0,0,0,0,0x80020010,0,0x80020010,0,0,2,0x80020016,0,0,
        0,0,0x80020010,0x8002000b,0,0,0,0,0,2};
    for (unsigned i = 0; i < expected.size(); ++i)
        require(s.get(0x2300 + 8*i) == expected[i], "Independent recursive/default/static lifecycle oracle differs");
    require(s.call(0, 0x2200) == 0, "Reinitialize destroyed mutex failed");
    const auto first = s.get(0x2200);
    // Init always overwrites (PLAT-04); it never reports EBUSY for a used slot.
    require(s.call(0, 0x2200) == 0 && s.get(0x2200) != first, "Mutex re-init did not publish a fresh handle");
    const auto token = s.get(0x2200);
    require(first > 2 && token > 2 && token != 0x2200, "Mutex slot lacks opaque lifecycle token");
    require(s.call(1, 0x2200) == 0, "Guest owner did not acquire mutex");
    s.active = 2;
    require(s.call(2, 0x2200) == 0x80020001, "Foreign guest thread unlocked mutex");
    require(s.call(1, 0x2200) == 0x80020010, "Foreign guest thread acquired busy mutex");
    rejects([&] { s.call(8, 0x2200); }, "wait/wake scheduler");
    s.active = 1;
    require(s.call(2, 0x2200) == 0 && s.call(3, 0x2200) == 0, "Foreign failure altered ownership");
    s.put(0x2220, token);
    require(s.call(1, 0x2220) == 0x80020016, "Copy of a destroyed mutex handle was admitted");
    require(s.call(4, 0x2210) == 0, "Attribute reinit failed");
    require(s.call(5, 0x2210, 0) == 0x80020016, "Invalid mutex type admitted");
    rejects([&] { s.call(7, 0x2210, 1); }, "priority protocol");
    require(s.call(7, 0x2210, 3) == 0x80020016, "Invalid protocol admitted");
    require(s.call(6, 0x2210) == 0, "Attribute cleanup failed");
    s.put(0x2220, 0xabcdef);
    rejects([&] { s.call(0, 0x2220, 0, 0x9000); }, "Guest access denied");
    require(s.get(0x2220) == 0xabcdef, "Rejected name published a mutex");
    rejects([&] { s.call(0, 0); }, "Invalid kernel primitive guest slot");
    rejects([&] { s.call(0, 0xfffffffffffffffcuLL); }, "Invalid kernel primitive guest slot");
    rejects([&] { s.call(0, 0x2ffc); }, "Guest access denied");
    s.put(0x2230, 0); s.active = 0;
    rejects([&] { s.call(1, 0x2230); }, "active guest thread");
    require(s.get(0x2230) == 0, "Missing guest thread published a static mutex token");
    s.active = 1;
    s.machine.Map(0x6000, 4096, Permission::Read);
    rejects([&] { s.call(0, 0x6000); }, "permission");
    require(s.call(0, 0x2220) == 0, "Negative pointer controls left orphan mutex state");
    require(s.call(3, 0x2220) == 0, "Cleanup after pointer controls failed");
    auto exact = qualified("upoVrzMHFeE");
    for (unsigned field = 0; field < 5; ++field) {
        auto wrong = exact;
        if (field == 0) wrong.LibraryName = "libc";
        if (field == 1) wrong.ModuleName = "libc";
        if (field == 2) wrong.LibraryVersion = 2;
        if (field == 3) wrong.ModuleMajor = 2;
        if (field == 4) wrong.ModuleMinor = 2;
        rejects([&] { s.provider->Resolve(wrong, 2); }, "scope/version/type");
    }
    rejects([&] { s.provider->Resolve(exact, 1); }, "scope/version/type");
    auto unknown = exact; unknown.Nid = "AAAAAAAAAAA";
    require(!s.provider->Resolve(unknown, 2), "Unknown kernel NID fabricated a gate");
    require(s.call(0, 0x2200) == 0, "Cross-provider control could not create live mutex");
    const auto liveToken = s.get(0x2200);
    {
        Cpu::Platform::KernelPrimitives other(s.machine, [&] { return s.active; }, 0x7ffdc1010000);
        const auto foreignGate = *other.Resolve(exact, 2);
        s.put(0x2108, foreignGate);
        require(s.call(1, 0x2200) == 0x80020016, "Provider accepted another provider's live token");
        require(s.get(0x2200) == liveToken, "Foreign provider changed the original mutex token");
        s.put(0x2108, s.gates[1]);
    }
    require(s.call(1, 0x2200) == 0 && s.call(2, 0x2200) == 0 && s.call(3, 0x2200) == 0,
            "Cross-provider rejection altered original mutex lifecycle");
    require(s.call(0, 0x2200) == 0, "Provider replacement control could not create live mutex");
    const auto withdrawnToken = s.get(0x2200);
    const auto withdrawnGate = s.gates[1];
    s.provider.reset();
    rejects([&] { s.machine.CheckAccess(withdrawnGate, 1, Permission::Execute); }, "Guest access denied");
    s.provider = std::make_unique<Cpu::Platform::KernelPrimitives>(s.machine, [&] { return s.active; });
    const auto inventory = Cpu::Platform::KernelPrimitiveInventory();
    for (unsigned i = 0; i < s.gates.size(); ++i)
        s.gates[i] = *s.provider->Resolve(qualified(inventory[i].Nid), 2);
    s.machine.Write(0x2100, std::as_bytes(std::span(s.gates)));
    require(s.gates[1] == withdrawnGate, "Replacement did not reuse the released gate page");
    require(s.call(1, 0x2200) == 0x80020016, "Replacement retained withdrawn provider token or callback");
    require(s.get(0x2200) == withdrawnToken, "Replacement stale-token rejection changed guest slot");
    require(s.call(0, 0x2200) == 0 && s.get(0x2200) != withdrawnToken,
            "Replacement initialization did not publish a fresh token");
    require(s.call(1, 0x2200) == 0 && s.call(2, 0x2200) == 0 && s.call(3, 0x2200) == 0,
            "Replacement provider real guest lifecycle failed");
    s.provider.reset();
    rejects([&] { s.machine.CheckAccess(withdrawnGate, 1, Permission::Execute); }, "Guest access denied");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: KernelPrimitivesTest flat-guest-text.bin");
        std::ifstream stream(argv[1], std::ios::binary);
        require(bool(stream), "Cannot open compiled kernel probe");
        const std::vector<char> raw((std::istreambuf_iterator<char>(stream)), {});
        require(!raw.empty() && raw.size() <= 4096, "Kernel probe must fit one code page");
        std::vector<std::byte> code(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) code[i] = std::byte(static_cast<unsigned char>(raw[i]));
        check(code);
        std::cout << "PASS compiled x86-64 kernel mutex lifecycle, recursion, guest ownership, pointer guards and qualification\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
