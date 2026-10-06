#pragma once

#include <cpu/SceElf.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace Cpu {

enum class SceCrtArrayOwner { Unsupported, DtInit, DtFini };

struct SceCrtArrayContract {
    std::uint64_t Address = 0;
    std::uint64_t Size = 0;
    SceCrtArrayOwner Owner = SceCrtArrayOwner::Unsupported;
};

struct SceCrtCertificate {
    std::array<std::byte, 32> SourceSha256{};
    std::uint64_t SourceSize = 0;
    std::uint64_t Init = 0;
    std::uint64_t Fini = 0;
    SceCrtArrayContract Preinit;
    SceCrtArrayContract InitArray;
    SceCrtArrayContract FiniArray;
};

struct SceModuleFile {
    std::filesystem::path Path;
    std::uint64_t LoadBias = 0;
    std::optional<SceCrtCertificate> Crt = std::nullopt;
};

struct SceHostModule {
    std::string Filename;
    SceModuleIdentity Module;
    std::vector<SceLibraryIdentity> Libraries;
};

struct SceResolvedImport {
    std::uint64_t Address = 0;
    std::uint8_t Type = 0;
    std::uint64_t Size = 0;
    std::uint64_t TlsModuleId = 0;
    std::uint64_t TlsOffset = 0;
};

using SceModuleResolver = std::function<std::optional<SceResolvedImport>(const SceImport&, std::uint8_t)>;

struct SceModuleRecord {
    SceParsedImage Image;
    std::uint64_t LoadBias = 0;
    std::uint64_t TlsModuleId = 0;
    std::uint64_t Init = 0;
    std::uint64_t Fini = 0;
};

class SceModules {
public:
    SceModules(Machine& machine, const SceModuleFile& main,
               std::span<const SceModuleFile> dependencies,
               std::span<const SceHostModule> hostModules,
               const SceModuleResolver& resolver);
    ~SceModules();
    SceModules(const SceModules&) = delete;
    SceModules& operator=(const SceModules&) = delete;
    SceLoadedImage& Main();
    std::span<const SceModuleRecord> Modules() const;
    std::shared_ptr<SceTls> Tls() const;
    std::uint64_t EntryTerminationGate() const;
    StopReason RunMain(std::uint64_t entryPhaseBudget = 100000000,
                       std::uint64_t finalizerBudget = 1000000);
    void InitializeDependencies(std::uint64_t args = 0, std::uint64_t argp = 0,
                                std::uint64_t param = 0, std::uint64_t instructionBudget = 1000000);
    void FinalizeDependencies(std::uint64_t args = 0, std::uint64_t argp = 0,
                              std::uint64_t param = 0, std::uint64_t instructionBudget = 1000000);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
