#include <cpu/SceElf.hpp>
#include <cpu/SceImports.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t Bias = 0x1000000;
constexpr std::uint64_t Dyn = 0x5000;
constexpr std::uint64_t Strings = 0x6040;
constexpr std::uint64_t Symbols = 0x6200;
constexpr std::uint64_t Relocations = 0x6300;
constexpr std::uint64_t Plt = 0x6380;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void put(Bytes& bytes, std::uint64_t offset, std::uint64_t value, unsigned size = 8) {
    for (unsigned index = 0; index < size; ++index) bytes.at(offset + index) = static_cast<std::byte>(value >> (8 * index));
}

std::uint64_t word(Cpu::Machine& machine, std::uint64_t address) {
    std::array<std::byte, 8> bytes{};
    machine.Read(address, bytes);
    std::uint64_t result = 0;
    for (unsigned index = 0; index < bytes.size(); ++index)
        result |= std::uint64_t(std::to_integer<unsigned>(bytes[index])) << (index * 8);
    return result;
}

struct Fixture {
    Bytes bytes = Bytes(0x6800);
    std::size_t tags = 0;
    std::uint32_t libcOffset = 0;
    std::uint32_t nidOffset = 0;
    std::uint32_t exitOffset = 0;

    Fixture(bool modern = true) {
        std::memcpy(bytes.data(), "\x7f" "ELF", 4);
        put(bytes, 4, 2, 1);
        put(bytes, 5, 1, 1);
        put(bytes, 6, 1, 1);
        put(bytes, 7, 9, 1);
        put(bytes, 8, 2, 1);
        put(bytes, 16, 0xfe10, 2);
        put(bytes, 18, 62, 2);
        put(bytes, 20, 1, 4);
        put(bytes, 24, 0x1000);
        put(bytes, 32, 64);
        put(bytes, 52, 64, 2);
        put(bytes, 54, 56, 2);
        put(bytes, 56, 6, 2);
        Bytes code;
        const auto emit = [&](std::initializer_list<unsigned> values) {
            for (const auto value : values) code.push_back(static_cast<std::byte>(value));
        };
        const auto rip = [&](std::initializer_list<unsigned> opcode, std::uint64_t target, unsigned trailing = 0) {
            emit(opcode);
            const auto displacement = target - (0x1000 + code.size() + 4 + trailing);
            for (unsigned index = 0; index < 4; ++index) code.push_back(static_cast<std::byte>(displacement >> (8 * index)));
        };
        emit({0x48, 0x8b, 0x07});
        rip({0x48, 0x89, 0x05}, 0x3000);
        emit({0x48, 0x8b, 0x47, 8});
        rip({0x48, 0x89, 0x05}, 0x3008);
        emit({0x48, 0x89, 0xe0});
        rip({0x48, 0x89, 0x05}, 0x3010);
        emit({0x48, 0x8b, 0x04, 0x24});
        rip({0x48, 0x89, 0x05}, 0x3028);
        emit({0x48, 0x89, 0xf0});
        rip({0x48, 0x89, 0x05}, 0x3018);
        emit({0x48, 0x83, 0xec, 8});
        rip({0x48, 0x8b, 0x3d}, 0x4010);
        emit({0xbe, 0xab, 0, 0, 0, 0xba, 4, 0, 0, 0});
        rip({0xff, 0x15}, 0x4000);
        rip({0x80, 0x3d}, 0x3200, 1);
        emit({0xab, 0x9c, 0x5a});
        rip({0x48, 0x89, 0x15}, 0x3020);
        emit({0x75, 12, 0x31, 0xff, 0x48, 0x83, 0xc4, 8});
        rip({0xff, 0x25}, 0x4020);
        emit({0xbf, 77, 0, 0, 0, 0x48, 0x83, 0xc4, 8});
        rip({0xff, 0x25}, 0x4020);
        std::copy(code.begin(), code.end(), bytes.begin() + 0x1000);
        header(0, 1, 5, 0x1000, 0x1000, code.size(), 4096, 4096);
        header(1, 1, 6, 0x3000, 0x3000, 0x180, 4096, 4096);
        header(2, 0x61000010, 4, 0x4000, 0x4000, 0x100, 4096, 4096);
        header(4, 0x61000000, 4, 0x6000, 0, 0x800, 0, 8);
        header(5, 0x61000001, 4, 0x3100, 0x3100, 0x40, 0x40, 8);
        std::string strings(1, '\0');
        libcOffset = strings.size();
        strings += "libc";
        strings.push_back('\0');
        nidOffset = strings.size();
        strings += "8zTFvBIAIN8#B#B";
        strings.push_back('\0');
        exitOffset = strings.size();
        strings += "uMei1W9uyNo#B#B";
        strings.push_back('\0');
        std::memcpy(bytes.data() + Strings, strings.data(), strings.size());
        const std::uint64_t library = (1ull << 48) | (1ull << 32) | libcOffset;
        const std::uint64_t module = (1ull << 48) | (1ull << 40) | (1ull << 32) | libcOffset;
        tag(modern ? 0x61000049 : 0x61000015, library);
        tag(modern ? 0x61000049 : 0x61000015, library);
        tag(modern ? 0x61000045 : 0x6100000f, module);
        tag(0x61000035, 0x40);
        tag(0x61000037, strings.size());
        tag(0x61000039, 0x200);
        tag(0x6100003b, 24);
        tag(0x6100003f, 96);
        tag(0x6100002f, 0x300);
        tag(0x61000031, 72);
        tag(0x61000033, 24);
        tag(0x61000029, 0x380);
        tag(0x6100002d, 48);
        tag(0x6100002b, 7);
        finish();
        put(bytes, Symbols + 24, nidOffset, 4);
        put(bytes, Symbols + 28, 0x12, 1);
        put(bytes, Symbols + 52, 0x11, 1);
        put(bytes, Symbols + 54, 1, 2);
        put(bytes, Symbols + 56, 0x3000);
        put(bytes, Symbols + 64, 4096);
        put(bytes, Symbols + 72, exitOffset, 4);
        put(bytes, Symbols + 76, 0x12, 1);
        relocation(Relocations, 0x4008, 0, 8, 0x1000);
        relocation(Relocations + 24, 0x4010, 2, 1, 0x200);
        relocation(Relocations + 48, 0x4018, 2, 6, 0);
        relocation(Plt, 0x4000, 1, 7, 0);
        relocation(Plt + 24, 0x4020, 3, 7, 0);
    }

    void header(unsigned index, std::uint32_t type, std::uint32_t flags, std::uint64_t offset,
                std::uint64_t address, std::uint64_t fileSize, std::uint64_t memorySize, std::uint64_t alignment) {
        const auto location = 64 + index * 56;
        put(bytes, location, type, 4);
        put(bytes, location + 4, flags, 4);
        put(bytes, location + 8, offset);
        put(bytes, location + 16, address);
        put(bytes, location + 32, fileSize);
        put(bytes, location + 40, memorySize);
        put(bytes, location + 48, alignment);
    }

    void tag(std::uint64_t type, std::uint64_t value) {
        put(bytes, Dyn + tags * 16, type);
        put(bytes, Dyn + tags++ * 16 + 8, value);
    }

    void finish() {
        put(bytes, Dyn + tags * 16, 0);
        put(bytes, Dyn + tags * 16 + 8, 0);
        header(3, 2, 4, Dyn, 0, (tags + 1) * 16, (tags + 1) * 16, 8);
    }

    void relocation(std::uint64_t offset, std::uint64_t target, unsigned symbol, unsigned type, std::uint64_t addend) {
        put(bytes, offset, target);
        put(bytes, offset + 8, (std::uint64_t(symbol) << 32) | type);
        put(bytes, offset + 16, addend);
    }
};

struct Input {
    std::filesystem::path directory;
    std::filesystem::path path;
    explicit Input(const Bytes& bytes) {
        directory = std::filesystem::temp_directory_path() / ("anyps5-sce-" + std::to_string(::getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "Cannot create SCE fixture directory");
        path = directory / "application.elf";
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        require(bool(stream), "Cannot write SCE fixture");
    }
    ~Input() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) == std::string::npos)
            throw std::runtime_error(std::string("Wrong SCE rejection: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing SCE rejection: ") + expected);
}

void translatedEntry(bool modern) {
    Fixture fixture(modern);
    Input input(fixture.bytes);
    const auto parsed = Cpu::ParseSce(input.path);
    require(parsed.Entry == 0x1000 && parsed.Type == 0xfe10 && parsed.RelocationCount == 5,
            "SCE inspection lost executable entry or relocation metadata");
    require(parsed.Imports.size() == 2 && parsed.Imports[0].Nid == "8zTFvBIAIN8" && parsed.Imports[1].Nid == "uMei1W9uyNo" &&
            parsed.Imports[0].LibraryName == "libc" && parsed.Imports[0].ModuleName == "libc" &&
            parsed.Imports[0].LibraryId == 1 && parsed.Imports[0].ModuleId == 1 &&
            parsed.Imports[0].LibraryVersion == 1 && parsed.Imports[0].ModuleMajor == 1 &&
            parsed.Imports[0].ModuleMinor == 1, "SCE inspection lost qualified import identity or packed versions");
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    auto image = Cpu::LoadSce(machine, input.path, Bias, [&](const auto& import) { return imports.Resolve(import); });
    require(image.Entry == 0x1001000 && image.LoadBias == Bias, "SCE entry did not include the runtime load bias");
    require(word(machine, 0x1004008) == 0x1001000 && word(machine, 0x1004010) == 0x1003200 &&
            word(machine, 0x1004018) == 0x1003000, "RELATIVE, absolute-symbol, or GLOB_DAT relocation is incorrect");
    machine.CheckAccess(word(machine, 0x1004000), 1, Cpu::Permission::Execute);
    rejects([&] { machine.CheckAccess(0x1004000, 8, Cpu::Permission::Write); }, "permission");
    require(word(machine, 0x1003200) == 0, "SCE BSS was not zero initialized");
    require(image.ProcParam && image.ProcParam->Address == 0x1003100, "Process parameters did not retain their guest address");
    Cpu::SetupSceEntry(machine, image, {"fixture", "beta"}, imports.ExitGate());
    require(image.StackPointer % 16 == 8 && image.ArgumentBlock == image.StackPointer &&
            word(machine, image.StackPointer) == 2,
            "Orbis entry stack must place argc/argv at the initial RSP with RSP modulo 16 equal to 8");
    require(machine.Run(image.Entry, 0, 200) == Cpu::StopReason::Exit && machine.ExitCode() == 0,
            "Translated SCE entry/import/control-flow path did not exit successfully");
    require(word(machine, 0x1003000) == 2 && word(machine, 0x1003028) == 2 && word(machine, 0x1003010) == image.StackPointer &&
            word(machine, 0x1003018) == imports.ExitGate(), "Actual x86 entry observed wrong argc, stack, or RSI exit gate");
    std::array<char, 8> argument{};
    machine.Read(word(machine, 0x1003008), std::as_writable_bytes(std::span(argument)));
    require(std::string(argument.data()) == "fixture", "Actual x86 entry observed a host pointer or wrong argv");
    require(word(machine, 0x1003200) == 0x00000000abababab &&
            (word(machine, 0x1003020) & 0x8d5) == 0x44 && machine.Get(Cpu::Register::Rax) == 0x1003200,
            "Translated imported memset, compare flags, memory, or return register is incorrect");
    require(machine.Get(Cpu::Register::Rsp) == image.StackPointer, "SCE imported exit did not preserve the actual guest entry stack");
}

void invalidInputs() {
    struct Case { const char* Expected; std::function<void(Fixture&)> Corrupt; };
    const std::array cases{
        Case{"SCE table exceeds DYNLIBDATA", [](auto& f) { put(f.bytes, Dyn + 3 * 16 + 8, 0x900); }},
        Case{"conflicting imported identity", [](auto& f) { put(f.bytes, Dyn + 16 + 8, (1ull << 48) | (2ull << 32) | f.libcOffset); }},
        Case{"conflicting imported identity", [](auto& f) { f.tag(0x61000045, (1ull << 48) | (1ull << 40) | (2ull << 32) | f.libcOffset); f.finish(); }},
        Case{"qualifier has no matching", [](auto& f) { f.bytes[Strings + f.nidOffset + 14] = std::byte{'C'}; }},
        Case{"full NID#library#module", [](auto& f) { f.bytes[Strings + f.nidOffset + 13] = std::byte{0}; }},
        Case{"overlapping relocation writes", [](auto& f) { put(f.bytes, Relocations + 24, 0x400c); }},
        Case{"unmapped or inaccessible", [](auto& f) { put(f.bytes, 24, 0x3000); }},
        Case{"program segment exceeds", [](auto& f) { put(f.bytes, 64 + 4 * 56 + 32, 0x10000); }},
        Case{"unterminated dynamic segment", [](auto& f) { put(f.bytes, Dyn + f.tags * 16, 21); }},
        Case{"overlapping PT_LOAD pages", [](auto& f) { put(f.bytes, 64 + 2 * 56 + 16, 0x3000); }},
        Case{"RELATIVE relocation must", [](auto& f) { put(f.bytes, Relocations + 8, (1ull << 32) | 8); }},
        Case{"addend must be zero", [](auto& f) { put(f.bytes, Plt + 16, 1); }},
    };
    for (const auto& test : cases) {
        Fixture fixture;
        test.Corrupt(fixture);
        Input input(fixture.bytes);
        rejects([&] { Cpu::ParseSce(input.path); }, test.Expected);
    }
}

void distinctScopes() {
    Fixture fixture;
    std::string strings(1, '\0');
    const auto add = [&](const char* value) {
        const auto offset = strings.size();
        strings += value;
        strings.push_back('\0');
        return offset;
    };
    add("libc");
    add("8zTFvBIAIN8#B#B");
    add("uMei1W9uyNo#B#B");
    const auto otherLibrary = add("otherlib");
    const auto otherModule = add("othermodule");
    const auto otherImport = add("8zTFvBIAIN8#C#C");
    std::memcpy(fixture.bytes.data() + Strings, strings.data(), strings.size());
    put(fixture.bytes, Dyn + 4 * 16 + 8, strings.size());
    put(fixture.bytes, Dyn + 7 * 16 + 8, 120);
    put(fixture.bytes, Dyn + 9 * 16 + 8, 96);
    fixture.tag(0x61000049, (2ull << 48) | (7ull << 32) | otherLibrary);
    fixture.tag(0x61000045, (2ull << 48) | (3ull << 40) | (4ull << 32) | otherModule);
    fixture.finish();
    put(fixture.bytes, Symbols + 96, otherImport, 4);
    put(fixture.bytes, Symbols + 100, 0x12, 1);
    fixture.relocation(Relocations + 72, 0x4028, 4, 6, 0);
    Input input(fixture.bytes);
    Cpu::Machine machine;
    constexpr std::uint64_t firstGate = 0x70000000;
    constexpr std::uint64_t secondGate = 0x70001000;
    machine.Map(firstGate, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);
    machine.Map(secondGate, 4096, Cpu::Permission::Read | Cpu::Permission::Execute);
    std::vector<Cpu::SceImport> observed;
    const auto image = Cpu::LoadSce(machine, input.path, Bias, [&](const Cpu::SceImport& import) {
        observed.push_back(import);
        return import.LibraryId == 1 ? firstGate : secondGate;
    });
    require(observed.size() == 3 && image.Imports.size() == 3 && observed[0].Nid == observed[2].Nid,
            "Distinct scopes for the same NID were collapsed");
    require(observed[2].LibraryName == "otherlib" && observed[2].LibraryId == 2 && observed[2].LibraryVersion == 7 &&
            observed[2].ModuleName == "othermodule" && observed[2].ModuleId == 2 && observed[2].ModuleMajor == 3 &&
            observed[2].ModuleMinor == 4, "Resolver received the wrong full library/module/version identity");
    require(word(machine, 0x1004000) == firstGate && word(machine, 0x1004028) == secondGate,
            "Same-NID imports in distinct scopes bound to the wrong guest gate");
}

void unsupportedStartup() {
    struct Case { const char* Expected; std::function<void(Fixture&)> Change; };
    const std::array cases{
        Case{"guest initializer", [](auto& f) { f.tag(12, 0x1000); f.finish(); }},
        Case{"guest initializer", [](auto& f) { f.tag(25, 0x3010); f.tag(27, 8); f.finish(); }},
        Case{"DT_NEEDED guest module", [](auto& f) { f.tag(1, f.libcOffset); f.finish(); }},
        Case{"guest module initialization/loading", [](auto& f) { f.tag(0x61000045, (2ull << 48) | (2ull << 40) | (1ull << 32) | f.libcOffset); f.finish(); }},
        Case{"data or TLS imports", [](auto& f) { put(f.bytes, Symbols + 28, 0x11, 1); }},
        Case{"untyped imports", [](auto& f) { put(f.bytes, Symbols + 28, 0x10, 1); }},
        Case{"guest TLS initialization", [](auto& f) { f.header(5, 7, 4, 0x3100, 0x3100, 8, 16, 8); }},
        Case{"unsupported relocation type 18", [](auto& f) { put(f.bytes, Relocations + 8, (2ull << 32) | 18); }},
        Case{"guest shared module", [](auto& f) { put(f.bytes, 16, 0xfe18, 2); }},
    };
    for (const auto& test : cases) {
        Fixture fixture;
        test.Change(fixture);
        Input input(fixture.bytes);
        Cpu::Machine machine;
        unsigned resolutions = 0;
        rejects([&] { Cpu::LoadSce(machine, input.path, Bias, [&](const auto&) { ++resolutions; return 0; }); }, test.Expected);
        require(resolutions == 0, "Unsupported startup mutated import resolution before failing");
        rejects([&] { machine.CheckAccess(0x1001000, 1, Cpu::Permission::Execute); }, "permission");
    }
    Fixture fixture;
    Input input(fixture.bytes);
    Cpu::Machine machine;
    rejects([&] { Cpu::LoadSce(machine, input.path, Bias, [](const auto&) { return 0; }); }, "unresolved import");
    rejects([&] { machine.CheckAccess(0x1001000, 1, Cpu::Permission::Execute); }, "permission");
}
}

int main() {
    try {
        translatedEntry(true);
        translatedEntry(false);
        invalidInputs();
        distinctScopes();
        unsupportedStartup();
        std::cout << "PASS translated SCE entry, scoped host imports, relocations, RELRO, BSS, Orbis ABI, malformed metadata, and unsupported startup\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
