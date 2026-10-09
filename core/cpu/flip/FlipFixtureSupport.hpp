#pragma once
#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <fstream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace FlipFixture {
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto RX = Cpu::Permission::Read | Cpu::Permission::Execute;
constexpr std::uint64_t Control = 0x100002000ULL, Builder = Control + 256,
    Commands = Control + 4096;
constexpr const char* FlipNid = "YUeqkyT7mEQ";
constexpr const char* WaitNid = "MWiElSNE8j8";
constexpr const char* SubmitNid = "UglJIZjGssM";
inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F&& action, const char* reason) {
    try { action(); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(reason) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing rejection: ") + reason);
}
template<class T> void store(Cpu::Machine& machine, std::uint64_t address, const T& value) {
    machine.Write(address, std::as_bytes(std::span(&value, 1)));
}
template<class T> T load(Cpu::Machine& machine, std::uint64_t address) {
    T value{};
    machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}
inline Cpu::SceImport identity(const char* nid, bool driver = false) {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = value.ModuleName = driver ? "libSceAgcDriver" : "libSceAgc";
    value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
    value.LibraryId = driver ? 41 : 37;
    value.ModuleId = driver ? 42 : 38;
    return value;
}
inline Cpu::SceImport videoIdentity(const char* nid) {
    auto value = identity(nid);
    value.LibraryName = value.ModuleName = "libSceVideoOut";
    value.LibraryId = 39; value.ModuleId = 40;
    return value;
}
inline void builder(Cpu::Machine& machine, std::uint64_t cursor = Commands,
                    std::uint64_t end = Commands + 256, std::uint32_t reserve = 2) {
    // Independent byte-offset layout; no production CommandBuffer struct used.
    std::array<std::byte, 72> guarded; guarded.fill(std::byte{0x7b});
    machine.Write(Builder - 8, guarded);
    store(machine, Builder, Commands); store(machine, Builder + 8, end);
    store(machine, Builder + 16, cursor); store(machine, Builder + 24, end);
    store(machine, Builder + 32, std::uint64_t{0x9897969594939291ULL});
    store(machine, Builder + 40, std::uint64_t{0x8182838485868788ULL});
    store(machine, Builder + 48, reserve);
    std::array<std::byte, 128> poison; poison.fill(std::byte{0xd3});
    machine.Write(Commands, poison);
}
class Caller {
    Cpu::Machine& machine;
    std::array<std::vector<char>, 4> code;
public:
    enum Section : unsigned { Emit, Wait, Sequence, Submit };
    Caller(Cpu::Machine& value, const char* const* paths,
           std::span<std::byte> commands = {}, std::span<std::byte> fullBacking = {}) : machine(value) {
        machine.Map(0x1000, 4096, RX); machine.Map(0x4000, 4096, RX);
        machine.Map(0x5000, 4096, RW); machine.Map(Control, 4096, RW);
        if (commands.empty()) machine.Map(Commands, 4096, RW);
        else machine.MapBorrowed(Commands, commands, RW, fullBacking);
        for (unsigned i = 0; i < code.size(); ++i) {
            std::ifstream input(paths[i], std::ios::binary);
            require(input.good(), "Missing compiler-produced flip caller");
            code[i] = std::vector<char>((std::istreambuf_iterator<char>(input)), {});
            require(!code[i].empty() && code[i].size() < 4096, "Guest flip caller exceeded code-page bound");
        }
    }
    std::uint64_t call(Section section, std::uint64_t gate, const std::array<std::uint64_t, 8>& args) {
        machine.Write(0x4000, std::as_bytes(std::span(code[section])));
        store(machine, Control, args);
        machine.Set(Cpu::Register::Rdi, gate); machine.Set(Cpu::Register::Rsi, Control);
        machine.Set(Cpu::Register::Rsp, 0x5fc8); store(machine, 0x5fc8, std::uint64_t{0x1000});
        machine.Set(Cpu::Register::Rbx, 0x123456789abcdef0ULL);
        store(machine, 0x5f00, std::uint64_t{0xf1f2f3f4f5f6f7f8ULL});
        require(machine.Run(0x4000, 0x1000, 2000) == Cpu::StopReason::Address,
            "Compiler-produced flip caller did not return");
        require(machine.Get(Cpu::Register::Rsp) == 0x5fd0 &&
            machine.Get(Cpu::Register::Rbx) == 0x123456789abcdef0ULL &&
            load<std::uint64_t>(machine, 0x5f00) == 0xf1f2f3f4f5f6f7f8ULL,
            "Flip import corrupted guest continuation/stack");
        return machine.Get(Cpu::Register::Rax);
    }
};
}
