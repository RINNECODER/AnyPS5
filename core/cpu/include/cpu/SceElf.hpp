#pragma once

#include <cpu/Cpu.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Cpu {

struct SceImport {
    std::string Nid;
    std::string LibraryName;
    std::uint16_t LibraryId = 0;
    std::string ModuleName;
    std::uint16_t ModuleId = 0;
    std::uint16_t LibraryVersion = 0;
    std::uint8_t ModuleMajor = 0;
    std::uint8_t ModuleMinor = 0;
};

struct SceSegment {
    std::uint32_t Type = 0;
    std::uint32_t Flags = 0;
    std::uint64_t Offset = 0;
    std::uint64_t Address = 0;
    std::uint64_t FileSize = 0;
    std::uint64_t MemorySize = 0;
    std::uint64_t Alignment = 0;
};

struct SceImageData;
class SceTls;

struct SceLibraryAttribute {
    std::uint16_t LibraryId = 0;
    std::uint64_t Attributes = 0;
};

struct SceModuleIdentity {
    std::string Name;
    std::uint16_t Id = 0;
    std::uint8_t Major = 0;
    std::uint8_t Minor = 0;
};

struct SceLibraryIdentity {
    std::string Name;
    std::uint16_t Id = 0;
    std::uint16_t Version = 0;
};

struct SceExport {
    SceImport Identity;
    std::uint64_t SymbolIndex = 0;
    std::uint64_t Value = 0;
    std::uint64_t Size = 0;
    std::uint16_t Section = 0;
    std::uint8_t Type = 0;
    std::uint8_t Binding = 0;
    std::uint8_t Visibility = 0;
};

struct SceParsedImage {
    std::filesystem::path Path;
    std::string SourceContainer = "elf";
    std::vector<std::string> ReconstructionNotes;
    std::uint16_t Type = 0;
    std::uint8_t OsAbi = 0;
    std::uint8_t AbiVersion = 0;
    std::uint64_t Entry = 0;
    std::vector<SceSegment> Segments;
    std::vector<SceImport> Imports;
    std::vector<std::string> NeededModules;
    std::vector<std::string> NeededFiles;
    std::vector<SceLibraryAttribute> ImportLibraryAttributes;
    std::vector<SceModuleIdentity> ExportModules;
    std::vector<SceLibraryIdentity> ExportLibraries;
    std::vector<SceLibraryAttribute> ExportLibraryAttributes;
    std::vector<SceExport> Exports;
    std::optional<std::string> OriginalFilename;
    std::vector<std::uint32_t> RelocationTypes;
    std::uint64_t RelocationCount = 0;
    std::vector<std::string> UnsupportedReasons;
    std::optional<SceSegment> Tls;
    std::optional<SceSegment> ProcParam;
    std::shared_ptr<const SceImageData> Data;
};

struct SceLoadedImage {
    std::filesystem::path Path;
    std::uint64_t Entry = 0;
    std::uint64_t LoadBias = 0;
    std::uint64_t StackPointer = 0;
    std::uint64_t ArgumentBlock = 0;
    std::vector<SceImport> Imports;
    std::vector<std::string> NeededModules;
    std::optional<SceSegment> ProcParam;
    std::shared_ptr<SceTls> Tls;
};

SceParsedImage ParseSce(const std::filesystem::path& path);
SceLoadedImage LoadSce(Machine& machine, const std::filesystem::path& path,
                      std::uint64_t loadBias,
                      const std::function<std::uint64_t(const SceImport&)>& resolver);
void SetupSceEntry(Machine& machine, SceLoadedImage& image,
                   const std::vector<std::string>& arguments, std::uint64_t exitGate);

}
