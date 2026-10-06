#include <cpu/ElfLoader.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void put(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) bytes.at(offset + i) = static_cast<std::byte>(value >> (i * 8));
}

std::vector<std::byte> fixture() {
    std::vector<std::byte> bytes(0x1010);
    put(bytes, 0, 0x00010102464c457f, 8);
    put(bytes, 16, 2, 2);
    put(bytes, 18, 62, 2);
    put(bytes, 20, 1, 4);
    put(bytes, 24, 0x400180, 8);
    put(bytes, 32, 64, 8);
    put(bytes, 52, 64, 2);
    put(bytes, 54, 56, 2);
    put(bytes, 56, 2, 2);
    put(bytes, 64, 1, 4);
    put(bytes, 68, 5, 4);
    put(bytes, 80, 0x400000, 8);
    put(bytes, 96, 0x200, 8);
    put(bytes, 104, 0x200, 8);
    put(bytes, 112, 4096, 8);
    put(bytes, 120, 1, 4);
    put(bytes, 124, 6, 4);
    put(bytes, 128, 0x1000, 8);
    put(bytes, 136, 0x401000, 8);
    put(bytes, 152, 16, 8);
    put(bytes, 160, 48, 8);
    put(bytes, 168, 4096, 8);
    const std::array<unsigned char, 6> code{0xb8, 0x11, 0x22, 0x33, 0x44, 0xc3};
    for (std::size_t i = 0; i < code.size(); ++i) bytes[0x180 + i] = static_cast<std::byte>(code[i]);
    for (unsigned i = 0; i < 16; ++i) bytes[0x1000 + i] = static_cast<std::byte>(0xa0 + i);
    return bytes;
}

struct File {
    std::filesystem::path Path;
    explicit File(const std::vector<std::byte>& bytes) {
        static unsigned serial = 0;
        Path = std::filesystem::temp_directory_path() / ("anyps5-loader-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++) + ".elf");
        std::ofstream output(Path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!output) throw std::runtime_error("Cannot write loader fixture");
    }
    ~File() { std::error_code error; std::filesystem::remove(Path, error); }
};

void failure(const std::function<void()>& operation, const std::string& diagnostic) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(diagnostic) != std::string::npos,
                "Wrong failure for " + diagnostic + ": " + error.what());
        return;
    }
    throw std::runtime_error("Expected failure: " + diagnostic);
}

std::uint64_t word(Cpu::Machine& machine, std::uint64_t address) {
    std::array<std::byte, 8> bytes;
    machine.Read(address, bytes);
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
    return value;
}

std::string string(Cpu::Machine& machine, std::uint64_t address) {
    std::string result;
    for (unsigned i = 0; i < 4096; ++i) {
        std::byte byte;
        machine.Read(address + i, std::span(&byte, 1));
        if (byte == std::byte{0}) return result;
        result.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("Guest initial stack string is not terminated");
}

void loadAndExecute() {
    const auto bytes = fixture();
    const File file(bytes);
    Cpu::Machine machine;
    const auto image = Cpu::Load(machine, file.Path);
    require(image.Entry == 0x400180, "Loader changed the ELF entry point");
    require(image.ProgramHeaderAddress == 0x400040, "AT_PHDR does not address the mapped ELF headers");
    require(image.Segments.size() == 2, "Loader omitted a PT_LOAD segment");
    std::array<std::byte, 48> data;
    machine.Read(0x401000, data);
    for (unsigned i = 0; i < 16; ++i) require(data[i] == static_cast<std::byte>(0xa0 + i), "PT_LOAD data bytes changed");
    require(std::all_of(data.begin() + 16, data.end(), [](auto byte) { return byte == std::byte{0}; }), "BSS was not zero-filled");
    require(machine.Run(image.Entry, image.Entry + 5, 10) == Cpu::StopReason::Address, "Loaded x86 MOV did not reach the next instruction");
    require(machine.Get(Cpu::Register::Rax) == 0x44332211, "Loaded x86 machine code produced the wrong register value");
    failure([&] { machine.Run(0x401000, 0x401001, 10); }, "protected instruction fetch");
    const std::array<std::byte, 8> writeCode{std::byte{0xc6}, std::byte{0x05}, std::byte{0xf9}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x90}, std::byte{0xc3}};
    machine.Write(image.Entry, writeCode);
    failure([&] { machine.Run(image.Entry, image.Entry + 7, 10); }, "protected write");
}

void stackAbi() {
    const File file(fixture());
    Cpu::Machine machine;
    auto image = Cpu::Load(machine, file.Path);
    machine.Set(Cpu::Register::Rflags, 0x402);
    Cpu::SetupStack(machine, image, {"fixture", "two words"}, {"CPU_MODE=arm64"});
    const auto stack = machine.Get(Cpu::Register::Rsp);
    require(stack == image.StackPointer && stack % 16 == 0, "Initial x86-64 stack is not 16-byte aligned");
    require(stack >= image.StackBase && stack < image.StackBase + image.StackSize, "Initial stack pointer is outside its mapping");
    require(word(machine, stack) == 2, "Initial argc is incorrect");
    require(string(machine, word(machine, stack + 8)) == "fixture", "argv[0] was corrupted");
    require(string(machine, word(machine, stack + 16)) == "two words", "argv[1] was corrupted");
    require(word(machine, stack + 24) == 0, "argv lacks its null terminator");
    require(string(machine, word(machine, stack + 32)) == "CPU_MODE=arm64", "envp was corrupted");
    require(word(machine, stack + 40) == 0, "envp lacks its null terminator");
    std::map<std::uint64_t, std::uint64_t> auxv;
    auto cursor = stack + 48;
    bool terminated = false;
    for (unsigned i = 0; i < 32; ++i) {
        const auto type = word(machine, cursor);
        const auto value = word(machine, cursor + 8);
        cursor += 16;
        if (type == 0) { require(value == 0, "AT_NULL value is not zero"); terminated = true; break; }
        require(auxv.emplace(type, value).second, "Auxiliary vector contains duplicate entries");
    }
    require(terminated, "Auxiliary vector lacks AT_NULL");
    require(auxv.at(3) == 0x400040 && auxv.at(4) == 56 && auxv.at(5) == 2, "Program header auxiliary vector is incorrect");
    require(auxv.at(6) == 4096 && auxv.at(7) == 0 && auxv.at(8) == 0 && auxv.at(9) == 0x400180, "Page/base/entry auxiliary vector is incorrect");
    require(string(machine, auxv.at(31)) == file.Path.string(), "AT_EXECFN does not name the loaded executable");
    require(auxv.at(25) >= cursor && auxv.at(25) + 16 <= image.StackBase + image.StackSize, "AT_RANDOM does not address 16 bytes of stack memory");
    require(machine.Get(Cpu::Register::Rdx) == 0, "Initial RDX incorrectly supplies a dynamic linker finalizer");
    require(machine.Get(Cpu::Register::Rflags) == 2, "Initial RFLAGS retains a stale direction flag");
    failure([&] { Cpu::SetupStack(machine, image, {}); }, "already been configured");
    std::byte guard;
    failure([&] { machine.Read(image.StackBase - 1, std::span(&guard, 1)); }, "Guest access denied");
}

void detachedProgramHeaders() {
    auto bytes = fixture();
    put(bytes, 72, 0x180, 8);
    put(bytes, 80, 0x400180, 8);
    put(bytes, 96, 0x80, 8);
    put(bytes, 104, 0x80, 8);
    put(bytes, 112, 1, 8);
    const File file(bytes);
    Cpu::Machine machine;
    const auto image = Cpu::Load(machine, file.Path);
    std::array<std::byte, 112> headers;
    machine.Read(image.ProgramHeaderAddress, headers);
    require(std::equal(headers.begin(), headers.end(), bytes.begin() + 64), "Detached AT_PHDR bytes do not match the executable table");
}

void malformedImages() {
    struct Case { const char* Diagnostic; std::function<void(std::vector<std::byte>&)> Mutate; };
    const std::vector<Case> cases{
        {"file size", [](auto& b) { b.resize(63); }},
        {"not an ELF", [](auto& b) { b[0] = std::byte{0}; }},
        {"little-endian ELF64", [](auto& b) { b[5] = std::byte{2}; }},
        {"OS ABI", [](auto& b) { b[7] = std::byte{9}; }},
        {"static ET_EXEC", [](auto& b) { put(b, 16, 3, 2); }},
        {"x86-64 guest", [](auto& b) { put(b, 18, 183, 2); }},
        {"header version or size", [](auto& b) { put(b, 52, 0, 2); }},
        {"program header table", [](auto& b) { put(b, 32, 0xfffffffffffffff0, 8); }},
        {"program header table", [](auto& b) { put(b, 54, 55, 2); }},
        {"PT_DYNAMIC", [](auto& b) { put(b, 120, 2, 4); }},
        {"PT_INTERP", [](auto& b) { put(b, 120, 3, 4); }},
        {"PT_TLS", [](auto& b) { put(b, 120, 7, 4); }},
        {"unsupported program header type", [](auto& b) { put(b, 120, 0x60000001, 4); }},
        {"executable guest stacks", [](auto& b) { put(b, 120, 0x6474e551, 4); put(b, 124, 7, 4); }},
        {"permission flags", [](auto& b) { put(b, 124, 0x80, 4); }},
        {"PT_LOAD file range", [](auto& b) { put(b, 152, 49, 8); }},
        {"PT_LOAD file range", [](auto& b) { put(b, 128, 0x2000, 8); }},
        {"low canonical guest memory", [](auto& b) { put(b, 136, 0xfffffffffffff000, 8); }},
        {"page congruent", [](auto& b) { put(b, 136, 0x401001, 8); }},
        {"PT_LOAD alignment", [](auto& b) { put(b, 168, 3, 8); }},
        {"PT_LOAD addresses are not sorted", [](auto& b) { put(b, 136, 0x3ff000, 8); }},
        {"overlapping PT_LOAD pages", [](auto& b) { put(b, 136, 0x400000, 8); }},
        {"256-MiB image limit", [](auto& b) { put(b, 160, 256 * 1024 * 1024, 8); }},
        {"entry point", [](auto& b) { put(b, 24, 0x401000, 8); }},
        {"entry point", [](auto& b) { put(b, 104, 0x400, 8); put(b, 24, 0x400300, 8); }},
    };
    for (const auto& test : cases) {
        auto bytes = fixture();
        test.Mutate(bytes);
        const File file(bytes);
        Cpu::Machine machine;
        failure([&] { Cpu::Load(machine, file.Path); }, test.Diagnostic);
    }
    const File file(fixture());
    Cpu::Machine machine;
    auto image = Cpu::Load(machine, file.Path);
    failure([&] { Cpu::SetupStack(machine, image, {std::string("x\0y", 3)}); }, "embedded NUL");
    failure([&] { Cpu::SetupStack(machine, image, {std::string(1024 * 1024, 'a')}); }, "guest stack size");
    Cpu::SetupStack(machine, image, {});
    require(word(machine, image.StackPointer) == 0 && word(machine, image.StackPointer + 8) == 0 &&
            word(machine, image.StackPointer + 16) == 0, "Empty argc/argv/envp startup layout is incorrect");
}

}

int main() {
    try {
        loadAndExecute();
        stackAbi();
        detachedProgramHeaders();
        malformedImages();
        std::cout << "ELF loader tests passed: PT_LOAD bytes/BSS/permissions, entry execution, SysV stack, malformed/unsupported rejection\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
