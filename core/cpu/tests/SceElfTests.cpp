#include <cpu/SceElf.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
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
        Case{"overlapping logical PT_LOAD ranges", [](auto& f) { put(f.bytes, 64 + 2 * 56 + 16, 0x3000); }},
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

void sharedPageInspection() {
    Fixture fixture;
    put(fixture.bytes, 56, 7, 2);
    put(fixture.bytes, 64 + 40, 0x180);
    fixture.header(6, 1, 0, 0x2180, 0x1180, 0x80, 0x80, 16);
    Input input(fixture.bytes);
    const auto parsed = Cpu::ParseSce(input.path);
    require(parsed.Imports.size() == 2 && std::any_of(parsed.UnsupportedReasons.begin(), parsed.UnsupportedReasons.end(), [](const auto& reason) {
        return reason.find("differing permissions on a shared guest page") != std::string::npos;
    }), "Nonconflicting logical segments sharing a page must remain inspectable with an explicit execution blocker");
    Cpu::Machine machine;
    unsigned resolutions = 0;
    rejects([&] { Cpu::LoadSce(machine, input.path, Bias, [&](const auto&) { ++resolutions; return 0; }); },
            "differing permissions on a shared guest page");
    require(resolutions == 0, "Unsafe shared-page permissions reached import resolution");
    rejects([&] { machine.CheckAccess(Bias + 0x1000, 1, Cpu::Permission::Execute); }, "permission");
    fixture.header(6, 1, 0, 0x2170, 0x1170, 0x80, 0x80, 16);
    Input conflicting(fixture.bytes);
    rejects([&] { Cpu::ParseSce(conflicting.path); }, "overlapping logical PT_LOAD ranges");
}

void repeatedLibraryAttributes() {
    Fixture fixture;
    put(fixture.bytes, Dyn + 8, (1ull << 32) | fixture.libcOffset);
    put(fixture.bytes, Dyn + 16 + 8, (1ull << 32) | fixture.libcOffset);
    fixture.bytes[Strings + fixture.nidOffset + 12] = std::byte{'A'};
    fixture.bytes[Strings + fixture.exitOffset + 12] = std::byte{'A'};
    fixture.tag(0x61000019, 9);
    fixture.tag(0x61000019, 9);
    fixture.finish();
    Input input(fixture.bytes);
    const auto parsed = Cpu::ParseSce(input.path);
    require(parsed.Imports.size() == 2 && parsed.Imports[0].LibraryId == 0 && parsed.Imports[1].LibraryId == 0 &&
            parsed.Imports[0].LibraryName == "libc" && parsed.Imports[0].ModuleId == 1,
            "Repeated imported-library attributes or importer-local library ID zero lost qualified identity");
    require(parsed.ImportLibraryAttributes.size() == 2 && parsed.ImportLibraryAttributes[0].LibraryId == 0 &&
            parsed.ImportLibraryAttributes[0].Attributes == 9 && parsed.ImportLibraryAttributes[1].Attributes == 9,
            "Inspection discarded repeated scoped imported-library attribute records");
    require(std::any_of(parsed.UnsupportedReasons.begin(), parsed.UnsupportedReasons.end(), [](const auto& reason) {
        return reason.find("imported library attributes") != std::string::npos;
    }), "Unimplemented imported-library attribute semantics must remain an explicit execution blocker");
    Cpu::Machine machine;
    unsigned resolutions = 0;
    rejects([&] { Cpu::LoadSce(machine, input.path, Bias, [&](const auto&) { ++resolutions; return 0; }); },
            "imported library attributes");
    require(resolutions == 0, "Unsupported imported-library attributes reached host resolution");
    put(fixture.bytes, Dyn + (fixture.tags - 1) * 16 + 8, 8);
    Input conflicting(fixture.bytes);
    rejects([&] { Cpu::ParseSce(conflicting.path); }, "conflicting imported library attributes");
    put(fixture.bytes, Dyn + (fixture.tags - 1) * 16 + 8, (2ull << 48) | 9);
    Input unbound(fixture.bytes);
    rejects([&] { Cpu::ParseSce(unbound.path); }, "attribute has no matching imported library id");
}

struct TlsFixture : Fixture {
    explicit TlsFixture(bool bssOnly = false) {
        header(5, 7, 4, bssOnly ? 0x3200 : 0x3040, bssOnly ? 0x3200 : 0x3040,
               bssOnly ? 0 : 16, 32, 16);
        put(bytes, Dyn + 7 * 16 + 8, 144);
        put(bytes, Dyn + 9 * 16 + 8, bssOnly ? 120 : 168);
        put(bytes, Dyn + 11 * 16 + 8, 0x500);
        std::copy_n(bytes.begin() + Plt, 48, bytes.begin() + 0x6500);
        put(bytes, Symbols + 100, 0x16, 1);
        put(bytes, Symbols + 102, 1, 2);
        put(bytes, Symbols + 104, 0);
        put(bytes, Symbols + 112, 16);
        put(bytes, Symbols + 124, 0x16, 1);
        put(bytes, Symbols + 126, 1, 2);
        put(bytes, Symbols + 128, 24);
        put(bytes, Symbols + 136, 8);
        relocation(Relocations, 0x4008, 0, 16, 0);
        relocation(Relocations + 24, 0x4010, 4, 17, 8);
        relocation(Relocations + 48, 0x4018, 4, 18, 8);
        relocation(Relocations + 72, 0x4028, 4, 16, 0);
        relocation(Relocations + 96, 0x4030, 5, 18, 0);
        relocation(Relocations + 120, 0x3040, 0, 8, 0x1000);
        relocation(Relocations + 144, 0x3048, 5, 17, 0);
        Bytes code;
        const auto emit = [&](std::initializer_list<unsigned> values) {
            for (const auto value : values) code.push_back(static_cast<std::byte>(value));
        };
        const auto rip = [&](std::initializer_list<unsigned> opcode, std::uint64_t target) {
            emit(opcode);
            const auto displacement = target - (0x1000 + code.size() + 4);
            for (unsigned index = 0; index < 4; ++index) code.push_back(static_cast<std::byte>(displacement >> (8 * index)));
        };
        rip({0x48, 0x8b, 0x05}, 0x4018);
        emit({0x64, 0x48, 0x8b, 0x18});
        rip({0x48, 0x89, 0x1d}, 0x3000);
        emit({0x64, 0x48, 0x8b, 0x04, 0x25, 0xe0, 0xff, 0xff, 0xff});
        rip({0x48, 0x89, 0x05}, 0x3008);
        emit({0x64, 0x48, 0x8b, 0x04, 0x25, 0xf8, 0xff, 0xff, 0xff});
        rip({0x48, 0x89, 0x05}, 0x3010);
        emit({0x64, 0xc7, 0x04, 0x25, 0xf8, 0xff, 0xff, 0xff, 0x78, 0x56, 0x34, 0x12});
        emit({0x64, 0x8b, 0x04, 0x25, 0xf8, 0xff, 0xff, 0xff});
        rip({0x48, 0x89, 0x05}, 0x3018);
        emit({0x31, 0xff, 0x3d, 0x78, 0x56, 0x34, 0x12, 0x40, 0x0f, 0x95, 0xc7, 0x48, 0x83, 0xec, 8});
        rip({0xff, 0x15}, 0x4020);
        emit({0x0f, 0x0b});
        std::copy(code.begin(), code.end(), bytes.begin() + 0x1000);
        header(0, 1, 5, 0x1000, 0x1000, code.size(), 4096, 4096);
    }
};

void translatedTlsEntry(bool bssOnly = false) {
    TlsFixture fixture(bssOnly);
    Input input(fixture.bytes);
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    auto image = Cpu::LoadSce(machine, input.path, Bias, [&](const auto& import) { return imports.Resolve(import); });
    require(image.Tls && image.Tls->ModuleId() == 1 && image.Tls->MemorySize() == 32,
            "Loaded SCE main module did not expose its initialized TLS service");
    require(word(machine, Bias + 0x4008) == 1 && word(machine, Bias + 0x4028) == 1 &&
            word(machine, Bias + 0x4010) == 8 && word(machine, Bias + 0x4018) == 0xffffffffffffffe8ull &&
            word(machine, Bias + 0x4030) == 0xfffffffffffffff8ull,
            "DTPMOD64, DTPOFF64, or TPOFF64 disagrees with the independent main-module TLS ABI offsets");
    const auto tp = machine.Get(Cpu::Register::FsBase);
    require(tp && word(machine, tp - 32) == (bssOnly ? 0 : Bias + 0x1000) &&
            word(machine, tp - 24) == (bssOnly ? 0 : 24) && word(machine, tp - 8) == 0,
            "TLS initial template must copy relocated image values and keep its BSS zero");
    Cpu::SetupSceEntry(machine, image, {"tls-fixture"}, imports.ExitGate());
    require(machine.Run(image.Entry, 0, 200) == Cpu::StopReason::Exit && machine.ExitCode() == 0,
            "Actual x86 SCE TLS program failed its FS-based BSS write/control-flow check");
    require(word(machine, Bias + 0x3000) == (bssOnly ? 0 : 24) &&
            word(machine, Bias + 0x3008) == (bssOnly ? 0 : Bias + 0x1000) &&
            word(machine, Bias + 0x3010) == 0 && word(machine, Bias + 0x3018) == 0x12345678 &&
            word(machine, tp - 8) == 0x12345678 && machine.Get(Cpu::Register::Rbx) == (bssOnly ? 0 : 24),
            "Actual x86 FS accesses observed incorrect initialized TLS bytes, relocated pointer, BSS, or register result");
}


void translatedEmptyTlsEntry() {
    for (const auto alignment : {std::uint64_t{0}, std::uint64_t{16}}) {
        Fixture fixture;
        fixture.header(5, 7, 4, alignment ? 1 : 0, alignment ? 1 : 0, 0, 0, alignment);
        put(fixture.bytes, Dyn + 7 * 16 + 8, 120);
        put(fixture.bytes, Dyn + 9 * 16 + 8, 120);
        put(fixture.bytes, Symbols + 100, 0x16, 1);
        put(fixture.bytes, Symbols + 102, 1, 2);
        fixture.relocation(Relocations + 72, 0x4030, 0, 16, 0);
        fixture.relocation(Relocations + 96, 0x4038, 4, 16, 0);
        Input input(fixture.bytes);
        const auto parsed = Cpu::ParseSce(input.path);
        require(parsed.Tls && parsed.Tls->MemorySize == 0 && parsed.Tls->FileSize == 0 &&
                parsed.UnsupportedReasons.empty(), "Empty PT_TLS was lost or incorrectly blocked");
        Cpu::Machine machine;
        Cpu::SceImports imports(machine);
        auto image = Cpu::LoadSce(machine, input.path, Bias, [&](const auto& import) { return imports.Resolve(import); });
        require(image.Tls && image.Tls->ModuleId() == 1 && image.Tls->MemorySize() == 0 &&
                image.Tls->TlsBase() == 0 && word(machine, Bias + 0x4030) == 1 && word(machine, Bias + 0x4038) == 1,
                "Empty TLS identity-only null/typed DTPMOD64 produced fabricated storage or incorrect module ID");
        rejects([&] { image.Tls->Resolve(1, 0); }, "outside the main module");
        Cpu::SetupSceEntry(machine, image, {"empty-tls"}, imports.ExitGate());
        require(machine.Run(image.Entry, 0, 200) == Cpu::StopReason::Exit && machine.ExitCode() == 0 &&
                word(machine, Bias + 0x4030) == 1 && word(machine, Bias + 0x4038) == 1,
                "Actual x86 entry failed with empty TLS identity or rewrote identity-only relocation outputs");
    }
    struct Case { const char* Expected; std::function<void(Fixture&)> Change; };
    const std::array cases{
        Case{"invalid or duplicate TLS segment", [](auto& f) { f.header(5, 7, 4, 0, 0, 0, 0, 3); }},
        Case{"invalid or duplicate TLS segment", [](auto& f) { f.header(6, 7, 4, 0, 0, 0, 0, 0); put(f.bytes, 56, 7, 2); }},
        Case{"DTPMOD64 addend", [](auto& f) { f.relocation(Relocations + 72, 0x4030, 0, 16, 1); }},
        Case{"requires a nonempty", [](auto& f) { f.relocation(Relocations + 72, 0x4030, 0, 17, 0); }},
        Case{"requires a nonempty", [](auto& f) { f.relocation(Relocations + 72, 0x4030, 0, 18, 0); }},
        Case{"defined ordinary-section TLS symbol", [](auto& f) { f.relocation(Relocations + 72, 0x4030, 2, 16, 0); }},
        Case{"TLS symbol exceeds", [](auto& f) {
            put(f.bytes, Dyn + 7 * 16 + 8, 120);
            put(f.bytes, Symbols + 100, 0x16, 1); put(f.bytes, Symbols + 102, 1, 2); put(f.bytes, Symbols + 112, 1);
        }},
    };
    for (const auto& test : cases) {
        Fixture fixture;
        fixture.header(5, 7, 4, 0, 0, 0, 0, 0);
        put(fixture.bytes, Dyn + 9 * 16 + 8, 96);
        fixture.relocation(Relocations + 72, 0x4030, 0, 16, 0);
        test.Change(fixture);
        Input input(fixture.bytes);
        Cpu::Machine machine;
        unsigned resolutions = 0;
        rejects([&] { Cpu::LoadSce(machine, input.path, Bias, [&](const auto&) { ++resolutions; return 0; }); }, test.Expected);
        require(resolutions == 0 && machine.Mappings().empty() && machine.Get(Cpu::Register::FsBase) == 0,
                "Invalid empty-TLS metadata reached import resolution or guest mutation");
    }
}

void emptyTlsModuleGraph() {
    Fixture main;
    main.header(5, 7, 4, 0x3040, 0x3040, 8, 32, 16);
    put(main.bytes, 0x3040, 0x1122334455667788);
    const auto appendNeeded = [&](std::string_view filename) {
        const auto length = [&] {
            std::uint64_t result = 0;
            for (unsigned index = 0; index < 8; ++index)
                result |= std::uint64_t(std::to_integer<unsigned>(main.bytes.at(Dyn + 4 * 16 + 8 + index))) << (index * 8);
            return result;
        }();
        std::memcpy(main.bytes.data() + Strings + length, filename.data(), filename.size());
        put(main.bytes, Strings + length + filename.size(), 0, 1);
        put(main.bytes, Dyn + 4 * 16 + 8, length + filename.size() + 1);
        main.tag(1, length);
    };
    appendNeeded("EmptyTls.prx");
    appendNeeded("StoredTls.prx");
    main.tag(16, 0);
    main.finish();
    const auto dependency = [](bool empty) {
        Fixture fixture;
        put(fixture.bytes, 16, 0xfe18, 2);
        put(fixture.bytes, 24, 0);
        fixture.header(5, 7, 4, empty ? 0 : 0x3040, empty ? 0 : 0x3040, empty ? 0 : 8, empty ? 0 : 16, empty ? 0 : 16);
        put(fixture.bytes, Dyn + 7 * 16 + 8, 24);
        put(fixture.bytes, Dyn + 9 * 16 + 8, empty ? 24 : 72);
        put(fixture.bytes, Dyn + 12 * 16 + 8, 0);
        fixture.relocation(Relocations, 0x4008, 0, 16, 0);
        if (!empty) {
            put(fixture.bytes, 0x3040, 0x8877665544332211);
            fixture.relocation(Relocations + 24, 0x4010, 0, 18, 0);
            fixture.relocation(Relocations + 48, 0x4018, 0, 17, 0);
        }
        return fixture;
    };
    auto empty = dependency(true), stored = dependency(false);
    Input mainInput(main.bytes), emptyInput(empty.bytes), storedInput(stored.bytes);
    const auto emptyPath = mainInput.directory / "EmptyTls.prx";
    const auto storedPath = mainInput.directory / "StoredTls.prx";
    std::filesystem::copy_file(emptyInput.path, emptyPath);
    std::filesystem::copy_file(storedInput.path, storedPath);
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    const std::array dependencies{Cpu::SceModuleFile{emptyPath, 0x2000000}, Cpu::SceModuleFile{storedPath, 0x3000000}};
    const std::array hosts{Cpu::SceHostModule{"libc.prx", {"libc", 0, 1, 1}, {{"libc", 0, 1}}}};
    Cpu::SceModules graph(machine, {mainInput.path, Bias}, dependencies, hosts,
        [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
            require(type == 2, "Independent empty-TLS fixture unexpectedly needs host data or TLS");
            return Cpu::SceResolvedImport{imports.Resolve(import), type};
        });
    require(graph.Modules().size() == 3 && graph.Tls() && graph.Tls()->ModuleCount() == 3 &&
            graph.Modules()[0].TlsModuleId == 1 && graph.Modules()[1].TlsModuleId == 2 && graph.Modules()[2].TlsModuleId == 3,
            "Empty PT_TLS module was removed or renumbered a future module");
    const auto tls = graph.Tls();
    require(tls->MemorySize(2) == 0 && tls->TlsBase(2) == 0 && word(machine, 0x2004008) == 2 &&
            word(machine, 0x3004008) == 3 && word(machine, 0x3004010) == 0xffffffffffffffd0ull &&
            word(machine, 0x3004018) == 0 && word(machine, tls->Resolve(3, 0)) == 0x8877665544332211,
            "Empty provider's identity relocation or later provider's initialized storage/offsets are incorrect");
    const auto dtv = word(machine, tls->FsBase() + 8);
    require(word(machine, dtv + 24) == 0 && word(machine, dtv + 32) == tls->TlsBase(3),
            "Graph DTV fabricated empty storage or lost later module index");
    rejects([&] { tls->Resolve(2, 0); }, "outside module 2");
    Cpu::SetupSceEntry(machine, graph.Main(), {"empty-tls-graph"}, imports.ExitGate());
    graph.InitializeDependencies();
    require(machine.Run(graph.Main().Entry, 0, 200) == Cpu::StopReason::Exit && machine.ExitCode() == 0,
            "Actual x86 main entry failed with an identity-only TLS dependency");
    graph.FinalizeDependencies();
}

struct ExportFixture : Fixture {
    std::size_t moduleTag = 0;
    std::size_t libraryTag = 0;
    std::size_t attributeTag = 0;
    std::size_t filenameTag = 0;
    std::uint32_t stringsSize = 0;
    std::uint32_t functionOffset = 0;
    std::uint32_t ownerOffset = 0;
    std::uint32_t alternateOwnerOffset = 0;

    explicit ExportFixture(bool modern = true) : Fixture(modern) {
        put(bytes, 16, 0xfe18, 2);
        put(bytes, 24, 0);
        put(bytes, 64 + 16, 0);
        header(5, 7, 4, 0x3040, 0x3040, 8, 16, 16);
        stringsSize = static_cast<std::uint32_t>(libcOffset + 5 + 16 + 16);
        const auto append = [&](const std::string& value) {
            const auto offset = stringsSize;
            for (const auto character : value) bytes.at(Strings + stringsSize++) = static_cast<std::byte>(character);
            bytes.at(Strings + stringsSize++) = std::byte{};
            return offset;
        };
        ownerOffset = append("guest-libc");
        alternateOwnerOffset = append("alternate-owner");
        const auto otherLibrary = append("guest-objects");
        functionOffset = append("z8GPiQwaAEY#B#A");
        const auto object = append("qG50MWOiS-Q#C#A");
        const auto tls = append("eul2MC3gaYs#B#A");
        const auto filename = append("W:/Build/J03341591/sys/internal/usermode/src/libc/Prospero_Release/libc.prx");
        put(bytes, Dyn + 4 * 16 + 8, stringsSize);
        put(bytes, Dyn + 7 * 16 + 8, 168);
        moduleTag = tags;
        tag(modern ? 0x61000043 : 0x6100000d, (2ull << 40) | (3ull << 32) | ownerOffset);
        tag(modern ? 0x61000043 : 0x6100000d, (1ull << 48) | (4ull << 40) | (5ull << 32) | alternateOwnerOffset);
        libraryTag = tags;
        tag(modern ? 0x61000047 : 0x61000013, (1ull << 48) | (7ull << 32) | ownerOffset);
        tag(modern ? 0x61000047 : 0x61000013, (2ull << 48) | (9ull << 32) | otherLibrary);
        attributeTag = tags;
        tag(0x61000017, (1ull << 48) | 1);
        tag(0x61000017, (2ull << 48) | 2);
        filenameTag = tags;
        tag(modern ? 0x61000041 : 0x61000009, filename);
        finish();
        const auto symbol = [&](unsigned index, std::uint32_t name, unsigned info, std::uint64_t value, std::uint64_t size) {
            const auto offset = Symbols + index * 24;
            put(bytes, offset, name, 4);
            put(bytes, offset + 4, info, 1);
            put(bytes, offset + 5, 3, 1);
            put(bytes, offset + 6, 1, 2);
            put(bytes, offset + 8, value);
            put(bytes, offset + 16, size);
        };
        symbol(4, functionOffset, 0x12, 0, 4);
        symbol(5, object, 0x21, 0x3000, 8);
        symbol(6, tls, 0x16, 0, 8);
    }
};

void exportedModuleMetadata() {
    for (const bool modern : {true, false}) {
        ExportFixture fixture(modern);
        Input input(fixture.bytes);
        const auto parsed = Cpu::ParseSce(input.path);
        require(parsed.Type == 0xfe18 && parsed.Entry == 0 && parsed.Imports.size() == 2 &&
                parsed.Imports[0].LibraryName == "libc" && parsed.Imports[0].ModuleName == "libc" &&
                parsed.Imports[0].LibraryId == 1 && parsed.Imports[0].ModuleId == 1,
                "Export metadata changed the distinct imported module/library namespace");
        require(parsed.ExportModules.size() == 2 && parsed.ExportModules[0].Name == "guest-libc" &&
                parsed.ExportModules[0].Id == 0 && parsed.ExportModules[0].Major == 2 && parsed.ExportModules[0].Minor == 3 &&
                parsed.ExportModules[1].Name == "alternate-owner" && parsed.ExportModules[1].Id == 1 &&
                parsed.ExportModules[1].Major == 4 && parsed.ExportModules[1].Minor == 5 &&
                parsed.ExportLibraries.size() == 2 && parsed.ExportLibraries[0].Name == "guest-libc" &&
                parsed.ExportLibraries[0].Id == 1 && parsed.ExportLibraries[0].Version == 7 &&
                parsed.ExportLibraries[1].Name == "guest-objects" && parsed.ExportLibraries[1].Id == 2 &&
                parsed.ExportLibraries[1].Version == 9,
                "Export metadata lost distinct names, ID zero, record order, or packed versions");
        require(parsed.ExportLibraryAttributes.size() == 2 && parsed.ExportLibraryAttributes[0].LibraryId == 1 &&
                parsed.ExportLibraryAttributes[0].Attributes == 1 && parsed.ExportLibraryAttributes[1].LibraryId == 2 &&
                parsed.ExportLibraryAttributes[1].Attributes == 2,
                "Repeated export attributes lost their individual library identity");
        require(parsed.OriginalFilename && *parsed.OriginalFilename ==
                "W:/Build/J03341591/sys/internal/usermode/src/libc/Prospero_Release/libc.prx" &&
                std::none_of(parsed.UnsupportedReasons.begin(), parsed.UnsupportedReasons.end(), [](const auto& reason) {
                    return reason.find("unsupported dynamic tag") != std::string::npos;
                }), "Bounded original filename was lost or treated as an unsupported service");
        require(parsed.Exports.size() == 3, "Defined qualified function, object, and TLS symbols were omitted");
        const auto& function = parsed.Exports[0];
        const auto& object = parsed.Exports[1];
        const auto& tls = parsed.Exports[2];
        require(function.Identity.Nid == "z8GPiQwaAEY" && function.Identity.LibraryName == "guest-libc" &&
                function.Identity.LibraryId == 1 && function.Identity.LibraryVersion == 7 &&
                function.Identity.ModuleName == "guest-libc" && function.Identity.ModuleId == 0 &&
                function.Identity.ModuleMajor == 2 && function.Identity.ModuleMinor == 3 &&
                function.SymbolIndex == 4 && function.Value == 0 && function.Size == 4 && function.Section == 1 &&
                function.Type == 2 && function.Binding == 1 && function.Visibility == 3,
                "Zero-valued protected function definition was misclassified or lost its full export scope");
        require(object.Identity.Nid == "qG50MWOiS-Q" && object.Identity.LibraryName == "guest-objects" &&
                object.Identity.LibraryId == 2 && object.Identity.LibraryVersion == 9 &&
                object.Identity.ModuleName == "guest-libc" && object.Identity.ModuleId == 0 &&
                object.SymbolIndex == 5 && object.Value == 0x3000 && object.Size == 8 && object.Section == 1 &&
                object.Type == 1 && object.Binding == 2 && object.Visibility == 3 &&
                tls.Identity.Nid == "eul2MC3gaYs" && tls.Identity.LibraryName == "guest-libc" &&
                tls.Identity.LibraryId == 1 && tls.Identity.ModuleId == 0 && tls.SymbolIndex == 6 &&
                tls.Value == 0 && tls.Size == 8 && tls.Section == 1 && tls.Type == 6 && tls.Binding == 1 && tls.Visibility == 3,
                "Weak object or zero-offset TLS export lost its symbol type, value kind, or protected visibility");
        Cpu::Machine machine;
        unsigned resolutions = 0;
        rejects([&] { Cpu::LoadSce(machine, input.path, Bias, [&](const auto&) { ++resolutions; return 0; }); },
                "guest shared module loading");
        require(resolutions == 0 && machine.Get(Cpu::Register::FsBase) == 0,
                "PRX metadata inspection must not enable guest module execution or install TLS");
    }
}

void invalidExportMetadata() {
    struct Case { const char* Expected; std::function<void(ExportFixture&)> Change; };
    const std::array cases{
        Case{"conflicting exported identity", [](auto& f) { f.tag(0x61000047, (1ull << 48) | (7ull << 32) | f.libcOffset); f.finish(); }},
        Case{"conflicting exported identity", [](auto& f) { f.tag(0x61000043, (2ull << 40) | (3ull << 32) | f.alternateOwnerOffset); f.finish(); }},
        Case{"conflicting exported identity", [](auto& f) { f.tag(0x61000047, (1ull << 48) | (8ull << 32) | f.ownerOffset); f.finish(); }},
        Case{"conflicting exported library attributes", [](auto& f) { f.tag(0x61000017, (1ull << 48) | 2); f.finish(); }},
        Case{"attribute has no matching exported library", [](auto& f) { put(f.bytes, Dyn + f.attributeTag * 16 + 8, (3ull << 48) | 1); }},
        Case{"export qualifier has no matching", [](auto& f) { put(f.bytes, Dyn + f.moduleTag * 16, 0x61000007); }},
        Case{"export qualifier has no matching", [](auto& f) {
            put(f.bytes, Dyn + f.libraryTag * 16, 0x61000007);
            put(f.bytes, Dyn + f.attributeTag * 16 + 8, (2ull << 48) | 2);
        }},
        Case{"export qualifier has no matching", [](auto& f) { put(f.bytes, Strings + f.functionOffset + 14, 'C', 1); }},
        Case{"export requires full", [](auto& f) { put(f.bytes, Strings + f.functionOffset + 11, '!', 1); }},
        Case{"invalid export NID", [](auto& f) { put(f.bytes, Strings + f.functionOffset, '*', 1); }},
        Case{"dynamic string offset exceeds", [](auto& f) { put(f.bytes, Dyn + f.filenameTag * 16 + 8, f.stringsSize); }},
        Case{"unterminated dynamic string", [](auto& f) { put(f.bytes, Strings + f.stringsSize - 1, 'X', 1); }},
        Case{"conflicting original filename", [](auto& f) { f.tag(0x61000009, f.libcOffset); f.finish(); }},
        Case{"unmapped or inaccessible", [](auto& f) { put(f.bytes, Symbols + 4 * 24 + 16, 4097); }},
        Case{"TLS symbol exceeds", [](auto& f) { put(f.bytes, Symbols + 6 * 24 + 8, 16); }},
    };
    for (const auto& test : cases) {
        ExportFixture fixture;
        test.Change(fixture);
        Input input(fixture.bytes);
        rejects([&] { Cpu::ParseSce(input.path); }, test.Expected);
    }
}

void unsupportedStartup() {
    struct Case { const char* Expected; std::function<void(Fixture&)> Change; };
    const std::array cases{
        Case{"guest initializer", [](auto& f) { f.tag(12, 0x1000); f.finish(); }},
        Case{"guest initializer", [](auto& f) { f.tag(25, 0x3010); f.tag(27, 8); f.finish(); }},
        Case{"DT_NEEDED guest module", [](auto& f) { f.tag(1, f.libcOffset); f.finish(); }},
        Case{"guest module initialization/loading", [](auto& f) { f.header(5, 7, 4, 0x3100, 0x3100, 8, 16, 8); f.tag(0x61000045, (2ull << 48) | (2ull << 40) | (1ull << 32) | f.libcOffset); f.finish(); }},
        Case{"data or TLS imports", [](auto& f) { put(f.bytes, Symbols + 28, 0x11, 1); }},
        Case{"untyped imports", [](auto& f) { put(f.bytes, Symbols + 28, 0x10, 1); }},
        Case{"TLS alignment residue", [](auto& f) { f = TlsFixture(); f.header(5, 7, 4, 0x3041, 0x3041, 16, 32, 16); }},
        Case{"TLS template file mapping", [](auto& f) { f = TlsFixture(); f.header(5, 7, 4, 0x3140, 0x3040, 16, 32, 16); }},
        Case{"unmapped or inaccessible", [](auto& f) { f = TlsFixture(); f.header(5, 7, 4, 0x3200, 0x3200, 16, 32, 16); }},
        Case{"unmapped or inaccessible", [](auto& f) { f = TlsFixture(true); f.header(5, 7, 4, 0x3ff0, 0x3ff0, 0, 32, 16); }},
        Case{"invalid or duplicate TLS segment", [](auto& f) { f.header(5, 7, 4, 0, 0, 1, 0, 0); }},
        Case{"DTPMOD64 addend", [](auto& f) { f = TlsFixture(); put(f.bytes, Relocations + 16, 1); }},
        Case{"TLS relocation addend", [](auto& f) { f = TlsFixture(); put(f.bytes, Relocations + 24 + 16, 0xffffffffffffffffull); }},
        Case{"TLS relocation addend", [](auto& f) { f = TlsFixture(); put(f.bytes, Relocations + 24 + 16, 32); }},
        Case{"TLS relocation requires a defined", [](auto& f) { f = TlsFixture(); put(f.bytes, Relocations + 24 + 8, (2ull << 32) | 17); }},
        Case{"data or TLS imports", [](auto& f) { f = TlsFixture(); put(f.bytes, Symbols + 96, f.nidOffset, 4); put(f.bytes, Symbols + 102, 0, 2); }},
        Case{"TLS relocation requires a nonempty", [](auto& f) { put(f.bytes, Relocations + 8, 16); }},
        Case{"unsupported relocation type 999", [](auto& f) { put(f.bytes, Relocations + 8, (2ull << 32) | 999); }},
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
        require(machine.Get(Cpu::Register::FsBase) == 0, "Unsupported startup installed a guest TLS thread pointer before failing");
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
    struct Case { const char* Name; std::function<void()> Run; };
    const std::array cases{
        Case{"modern SCE entry", [] { translatedEntry(true); }},
        Case{"older SCE entry", [] { translatedEntry(false); }},
        Case{"malformed metadata", invalidInputs},
        Case{"distinct import scopes", distinctScopes},
        Case{"unsupported startup", unsupportedStartup},
        Case{"shared-page inspection", sharedPageInspection},
        Case{"repeated library attributes", repeatedLibraryAttributes},
        Case{"translated TLS entry", [] { translatedTlsEntry(); }},
        Case{"translated BSS-only TLS entry", [] { translatedTlsEntry(true); }},
        Case{"translated empty TLS identity", translatedEmptyTlsEntry},
        Case{"empty TLS module graph", emptyTlsModuleGraph},
        Case{"exported module metadata", exportedModuleMetadata},
        Case{"invalid export metadata", invalidExportMetadata},
    };
    unsigned failures = 0;
    for (const auto& test : cases) {
        try { test.Run(); }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.Name << ": " << error.what() << '\n';
        }
    }
    if (failures) return 1;
    std::cout << "PASS translated SCE entry, scoped host imports, relocations, TLS, shared-page inspection, RELRO, BSS, Orbis ABI, malformed metadata, and unsupported startup\n";
    return 0;
}
