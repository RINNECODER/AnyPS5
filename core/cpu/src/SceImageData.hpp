#pragma once

#include <cpu/SceElf.hpp>
#include <map>
#include <span>

namespace Cpu {

enum class SceRequirement : unsigned {
    None = 0, Dependencies = 1, Initializers = 2, TypedImports = 4, ImportedTls = 8, Symbolic = 16, StaticTls = 32
};

struct SceImageData {
    struct Symbol {
        std::uint64_t Value = 0;
        std::uint64_t Size = 0;
        std::uint16_t Section = 0;
        std::uint8_t Type = 0;
        std::optional<std::size_t> Import;
        std::uint8_t Binding = 0;
        std::uint8_t Visibility = 0;
    };
    struct Relocation {
        std::uint64_t Target;
        std::uint64_t Symbol;
        std::uint32_t Type;
        std::int64_t Addend;
        bool Plt;
    };
    struct Blocker { SceRequirement Requirement; std::string Reason; };
    struct Array { std::uint64_t Address = 0; std::uint64_t Size = 0; };
    std::vector<std::byte> Bytes;
    std::vector<Symbol> Symbols;
    std::vector<Relocation> Relocations;
    std::vector<SceSegment> Relro;
    std::map<std::uint16_t, std::string> NeededModuleIds;
    std::vector<SceModuleIdentity> ImportedModules;
    std::vector<Blocker> Blockers;
    std::uint64_t Init = 0;
    std::uint64_t Fini = 0;
    Array PreinitArray;
    Array InitArray;
    Array FiniArray;
    bool Symbolic = false;
};

struct SceSymbolValue {
    std::uint64_t Address = 0;
    std::uint8_t Type = 0;
    std::uint64_t Size = 0;
    std::uint64_t TlsModuleId = 0;
    std::uint64_t TlsOffset = 0;
};

struct SceRelocationWrite { std::uint64_t Address; std::uint64_t Value; };

void RequireSceProfile(const SceParsedImage& image, unsigned fulfilledRequirements);
std::uint64_t SceAddress(std::uint64_t bias, std::uint64_t value);
void ValidateSceMapping(const SceParsedImage& image, std::uint64_t bias);
void MapSceImage(Machine& machine, const SceParsedImage& image, std::uint64_t bias);
void ProtectSceImage(Machine& machine, const SceParsedImage& image, std::uint64_t bias);
std::vector<SceRelocationWrite> PlanSceRelocations(const SceParsedImage& image, std::uint64_t bias,
    const std::function<SceSymbolValue(std::uint64_t)>& symbolResolver, const SceTls* tls);
void ApplySceRelocations(Machine& machine, std::span<const SceRelocationWrite> writes);

}
