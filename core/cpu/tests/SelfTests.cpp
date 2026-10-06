#include <cpu/Self.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceKernelImports.hpp>
#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/SceMemoryImports.hpp>
#include <cpu/SceUserImports.hpp>
#include <cpu/SceSystemImports.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace {
using Bytes = std::vector<std::byte>;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::uint64_t get(const Bytes& bytes, std::size_t offset, unsigned size = 8) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes.at(offset + i))) << (8 * i);
    return value;
}
void put(Bytes& bytes, std::size_t offset, std::uint64_t value, unsigned size = 8) {
    for (unsigned i = 0; i < size; ++i) bytes.at(offset + i) = static_cast<std::byte>(value >> (8 * i));
}
std::uint64_t align(std::uint64_t value) { return (value + 15) & ~15ull; }
Bytes readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), "Cannot read compiled SCE fixture");
    const auto length = file.tellg();
    require(length > 64 && length < 256 * 1024 * 1024, "Compiled SCE fixture size is invalid");
    Bytes bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), length);
    require(bool(file), "Cannot read complete compiled SCE fixture");
    return bytes;
}

struct Wrapped {
    struct Record { std::uint64_t Flags, Offset, Size; Bytes Data; unsigned Program; };
    Bytes Expected, Container;
    std::vector<std::size_t> DataRecords;
    std::size_t ElfBase = 0;
    std::size_t ProgramCount = 0;

    Wrapped(const Bytes& original, bool modern, bool omitDynlib = false, bool notes = false, bool version = false) {
        Bytes normalized = original;
        const auto originalOffset = get(original, 32), originalCount = get(original, 56, 2);
        std::vector<Bytes> programs;
        for (std::size_t i = 0; i < originalCount; ++i)
            programs.emplace_back(original.begin() + originalOffset + i * 56, original.begin() + originalOffset + (i + 1) * 56);
        for (const auto& dynamic : programs) {
            if (get(dynamic, 0, 4) != 2) continue;
            const auto dynamicOffset = get(dynamic, 8), dynamicEnd = dynamicOffset + get(dynamic, 32);
            for (auto& dynlib : programs) {
                if (get(dynlib, 0, 4) != 0x61000000) continue;
                const auto start = get(dynlib, 8);
                require(start <= dynamicOffset && dynamicEnd <= original.size(),
                        "Compiler fixture dynamic metadata cannot be represented within an SDK DYNLIBDATA payload");
                put(dynlib, 32, std::max(get(dynlib, 32), dynamicEnd - start));
            }
        }
        std::uint64_t logicalSize = 64 + (originalCount + (notes ? 2 : 0) + (version ? 1 : 0)) * 56;
        for (const auto& program : programs) logicalSize = std::max(logicalSize, get(program, 8) + get(program, 32));
        if (notes) {
            Bytes coveredNote(56), missingNote(56);
            bool found = false;
            for (const auto& program : programs) {
                if (get(program, 0, 4) != 1 || get(program, 32) < 16) continue;
                put(coveredNote, 0, 4, 4);
                put(coveredNote, 8, get(program, 8));
                put(coveredNote, 32, 16);
                found = true;
                break;
            }
            require(found, "Fixture needs file-backed LOAD data for a covered NOTE");
            put(missingNote, 0, 4, 4);
            put(missingNote, 8, logicalSize + 64);
            put(missingNote, 32, 16);
            programs.push_back(std::move(coveredNote));
            programs.push_back(std::move(missingNote));
        }
        Bytes versionData;
        if (version) {
            versionData = {std::byte{'v'}, std::byte{'e'}, std::byte{'r'}, std::byte{0}, std::byte{9}};
            Bytes header(56);
            put(header, 0, 0x6fffff01, 4);
            put(header, 8, logicalSize + 128);
            put(header, 32, versionData.size());
            put(header, 40, versionData.size());
            put(header, 48, 1);
            programs.push_back(std::move(header));
            logicalSize += 128 + versionData.size();
        }
        ProgramCount = programs.size();
        normalized.resize(std::max<std::uint64_t>(normalized.size(), 64 + ProgramCount * 56));
        put(normalized, 32, 64);
        put(normalized, 56, ProgramCount, 2);
        for (std::size_t i = 0; i < ProgramCount; ++i)
            std::copy(programs[i].begin(), programs[i].end(), normalized.begin() + 64 + i * 56);
        Expected.resize(logicalSize);
        std::copy_n(normalized.begin(), 64 + ProgramCount * 56, Expected.begin());
        std::vector<Record> records;
        for (unsigned i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            const auto type = get(program, 0, 4), offset = get(program, 8), size = get(program, 32);
            if (type != 1 && type != 0x61000000 && type != 0x61000010 && type != 0x6fffff00) continue;
            require(offset + size <= normalized.size(), "Compiled fixture payload range exceeds original file");
            std::copy_n(normalized.begin() + offset, size, Expected.begin() + offset);
            if (omitDynlib && type == 0x61000000) continue;
            const auto dataIndex = records.size() + 1;
            const auto digestSize = ((size + 16383) / 16384) * 32;
            records.push_back({(dataIndex << 20) | 0x10004, 0, digestSize, Bytes(digestSize, std::byte{0xa5}), i});
            records.push_back({(std::uint64_t(i) << 20) | 0x2804, 0, size,
                Bytes(normalized.begin() + offset, normalized.begin() + offset + size), i});
        }
        require(records.size() >= 4, "SELF fixture needs multiple actual compiler payloads");
        std::reverse(records.begin(), records.end());
        for (auto& record : records)
            if (!(record.Flags & 0x800)) record.Flags = ((records.size() - 1 - (record.Flags >> 20)) << 20) | 0x10004;
        ElfBase = 32 + records.size() * 32;
        const auto headerSize = align(ElfBase + 64 + ProgramCount * 56) + 64;
        const auto metaSize = records.size() * 80 + 336;
        auto cursor = align(headerSize + metaSize + 96);
        for (std::size_t i = 0; i < records.size(); ++i) {
            records[i].Offset = cursor;
            cursor = align(cursor + records[i].Size + 16 * (i + 1));
        }
        Container.resize(cursor + versionData.size());
        put(Container, 0, modern ? 0xeef51454 : 0x1d3d154f, 4);
        put(Container, 5, 1, 1);
        put(Container, 6, 1, 1);
        put(Container, 7, 0x12, 1);
        put(Container, 8, 0x101, 4);
        put(Container, 12, headerSize, 2);
        put(Container, 14, metaSize, 2);
        put(Container, 16, cursor);
        put(Container, 24, records.size(), 2);
        put(Container, 26, 0x22, 2);
        std::copy_n(normalized.begin(), 64 + ProgramCount * 56, Container.begin() + ElfBase);
        for (std::size_t i = 0; i < records.size(); ++i) {
            const auto location = 32 + i * 32;
            put(Container, location, records[i].Flags);
            put(Container, location + 8, records[i].Offset);
            put(Container, location + 16, records[i].Size);
            put(Container, location + 24, records[i].Size);
            std::copy(records[i].Data.begin(), records[i].Data.end(), Container.begin() + records[i].Offset);
            if (records[i].Flags & 0x800) DataRecords.push_back(i);
        }
        if (notes) std::fill_n(Expected.begin() + 64 + (ProgramCount - (version ? 2 : 1)) * 56, 56, std::byte{});
        if (version) {
            std::copy(versionData.begin(), versionData.end(), Container.begin() + cursor);
            std::copy(versionData.begin(), versionData.end(), Expected.end() - versionData.size());
        }
        put(Expected, 40, 0);
        put(Expected, 58, 0, 2);
        put(Expected, 60, 0, 2);
        put(Expected, 62, 0, 2);
    }
};

template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) == std::string::npos)
            throw std::runtime_error(std::string("Wrong SELF rejection: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing SELF rejection: ") + expected);
}

void malformed(const Bytes& compiled) {
    using Change = std::function<void(Wrapped&)>;
    struct Case { const char* Expected; Change Corrupt; };
    const std::array cases{
        Case{"encrypted SELF", [](auto& f) { const auto p = 32 + f.DataRecords[0] * 32; put(f.Container, p, get(f.Container, p) | 2); }},
        Case{"compressed SELF", [](auto& f) { const auto p = 32 + f.DataRecords[0] * 32; put(f.Container, p, get(f.Container, p) | 8); }},
        Case{"unsupported SELF segment properties", [](auto& f) { const auto p = 32 + f.DataRecords[0] * 32; put(f.Container, p, get(f.Container, p) | 0x10); }},
        Case{"invalid SELF header", [](auto& f) { put(f.Container, 16, std::numeric_limits<std::uint64_t>::max()); }},
        Case{"invalid embedded program header", [](auto& f) { put(f.Container, f.ElfBase + 32, std::numeric_limits<std::uint64_t>::max()); }},
        Case{"invalid plaintext SELF segment range", [](auto& f) { put(f.Container, 32 + f.DataRecords[0] * 32 + 8, 0xfffffffffffffff0); }},
        Case{"overlapping SELF container", [](auto& f) { put(f.Container, 32 + f.DataRecords[1] * 32 + 8, get(f.Container, 32 + f.DataRecords[0] * 32 + 8)); }},
        Case{"invalid or duplicate SELF data program index", [](auto& f) { const auto p = 32 + f.DataRecords[0] * 32; put(f.Container, p, (0xffffull << 20) | 0x2804); }},
        Case{"invalid plaintext SELF segment range or size", [](auto& f) { const auto p = 32 + f.DataRecords[0] * 32; put(f.Container, p + 24, get(f.Container, p + 24) + 1); }},
        Case{"SELF digest metadata size", [](auto& f) { std::size_t i = 0; while (get(f.Container, 32 + i * 32) & 0x800) ++i; put(f.Container, 32 + i * 32 + 16, 1); put(f.Container, 32 + i * 32 + 24, 1); }},
        Case{"conflicting reconstructed ELF byte ranges", [](auto& f) { const auto p = 32 + f.DataRecords[0] * 32; const auto index = (get(f.Container, p) >> 20) & 0xffff; put(f.Container, f.ElfBase + 64 + index * 56 + 8, 0); }},
    };
    for (const auto& test : cases) {
        Wrapped fixture(compiled, false);
        test.Corrupt(fixture);
        rejects([&] { Cpu::DecodePlainSelf(fixture.Container); }, test.Expected);
    }
    Wrapped missing(compiled, false, true);
    rejects([&] { Cpu::DecodePlainSelf(missing.Container); }, "missing required ELF program payload");
    Wrapped tail(compiled, false, false, false, true);
    tail.Container.pop_back();
    rejects([&] { Cpu::DecodePlainSelf(tail.Container); }, "unavailable appended SELF version");
}

void execute(const Bytes& compiled) {
    Wrapped fixture(compiled, true);
    const auto path = std::filesystem::temp_directory_path() / ("anyps5-self-" + std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    struct Remove { std::filesystem::path Path; ~Remove() { std::error_code ignored; std::filesystem::remove(Path, ignored); } } cleanup{path};
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(fixture.Container.data()), fixture.Container.size());
        require(bool(file), "Cannot write owned plaintext SELF fixture");
    }
    const auto parsed = Cpu::ParseSce(path);
    require(parsed.SourceContainer == "plain_self" && parsed.Path == path,
            "SCE inspection lost SELF source identity or caller path");
    Cpu::Machine machine;
    auto memory = std::make_shared<Cpu::GuestMemoryRuntime>(machine, 12ULL << 30);
    Cpu::SceMemoryImports memoryImports(machine, memory);
    Cpu::SceImports imports(machine);
    Cpu::SceKernelImports kernel(machine, path.parent_path());
    Cpu::SceUserImports users(machine);
    Cpu::SceSystemImports system(machine);
    auto image = Cpu::LoadSce(machine, path, 0x1000000, [&](const auto& import) {
        if (const auto gate = memoryImports.Resolve(import)) return *gate;
        if (const auto gate = system.Resolve(import)) return *gate;
        if (const auto gate = users.Resolve(import)) return *gate;
        if (import.ModuleName == "libkernel") return kernel.Resolve(import);
        return imports.Resolve(import);
    });
    kernel.SetTls(image.Tls);
    Cpu::SetupSceEntry(machine, image, {"fixture", "not-a-number", "0", "0", "0", "0", "import", "0", "0", "0"}, imports.ExitGate());
    require(machine.Run(image.Entry, 0, 2000) == Cpu::StopReason::Exit && machine.ExitCode() == 82,
            "Reconstructed compiler-produced x86 SCE program did not reach its independently specified invalid-number exit");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "SELF tests require the compiler-produced SCE fixture path");
        const auto compiled = readFile(argv[1]);
        require(!Cpu::IsSelf(compiled), "Raw ELF was misidentified as SELF");
        for (const bool modern : {false, true}) {
            Wrapped fixture(compiled, modern);
            require(Cpu::IsSelf(fixture.Container), "Valid SELF magic was not recognized");
            const auto decoded = Cpu::DecodePlainSelf(fixture.Container);
            require(decoded.Bytes == fixture.Expected, "Reordered SELF entries/gaps did not reconstruct exact compiler-produced payload bytes and normalized ELF headers");
        }
        Wrapped notes(compiled, false, false, true, true);
        const auto decoded = Cpu::DecodePlainSelf(notes.Container);
        require(decoded.Bytes == notes.Expected, "Covered NOTE or bounded appended version bytes were lost, or absent NOTE was fabricated");
        require(std::any_of(decoded.NormalizationNotes.begin(), decoded.NormalizationNotes.end(), [](const auto& note) {
            return note.find("unavailable nonloadable PT_NOTE omitted") != std::string::npos;
        }), "Absent informational NOTE normalization was not reported");
        malformed(compiled);
        execute(compiled);
        std::cout << "PASS plaintext SELF reconstruction, real compiled SCE execution, reordered entries, alias coverage, version tail, normalization diagnostics, and explicit malformed/encrypted/compressed rejection\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
