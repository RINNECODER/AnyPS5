#pragma once

#include <cpu/SceElf.hpp>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace Cpu {

// One budget is shared by every slice and guest call in an execution phase.
class GuestPhaseBudget {
public:
    explicit GuestPhaseBudget(std::uint64_t instructionLimit);
    GuestPhaseBudget(const GuestPhaseBudget&) = delete;
    GuestPhaseBudget& operator=(const GuestPhaseBudget&) = delete;
    std::uint64_t Remaining() const noexcept;
    std::uint64_t Consumed() const noexcept;
    void Charge(std::uint64_t instructions);
private:
    std::uint64_t remaining_;
    std::uint64_t consumed_ = 0;
};

using SceThreadTlsFactory = std::function<std::shared_ptr<SceTls>(std::uint64_t)>;

enum class GuestModuleCallKind { Initialize, Finalize };
struct GuestModuleCall {
    GuestModuleCallKind Kind;
    std::uint64_t Entry;
    std::uint64_t ReturnGate;
    std::array<std::uint64_t, 3> Arguments{};
};
struct GuestCallResult {
    StopReason Reason;
    std::optional<std::uint64_t> ReturnValue;
};
enum class GuestEntryControlKind { None, TerminationCallback, ProcessExit };
struct GuestEntryControl {
    GuestEntryControlKind Kind = GuestEntryControlKind::None;
    std::optional<int> ExitCode;
};

// All hooks bind the same adopted initial guest on the persistent owner runtime.
// Invoke preserves its full CPU context; host callbacks only queue a pause.
struct SceModuleExecutor {
    Machine* Owner = nullptr;
    std::uint64_t InitialThread = 0;
    std::function<GuestCallResult(const GuestModuleCall&, GuestPhaseBudget&)> Invoke;
    std::function<StopReason(GuestPhaseBudget&)> RunEntry;
    std::function<void()> PauseTerminationFromHostCall;
    std::function<GuestEntryControl()> PendingControl;
    std::function<void()> CompleteControl;
};

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

// Opt-in source identity for the supplied guest libc. Only the fixed, ABI-qualified
// Internal function allowlist can forward to its libc/library-v1 exports. Guest
// allocator, callback/DSO, errno and exception ownership remains with the provider.
struct SceLibcInternalProvider {
    std::string Filename;
    std::array<std::byte, 32> SourceSha256{};
    std::uint64_t SourceSize = 0;
};

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
               const SceModuleResolver& resolver,
               const std::optional<SceLibcInternalProvider>& libcInternal = std::nullopt);
    ~SceModules();
    SceModules(const SceModules&) = delete;
    SceModules& operator=(const SceModules&) = delete;
    SceLoadedImage& Main();
    std::span<const SceModuleRecord> Modules() const;
    std::shared_ptr<SceTls> Tls() const;
    std::shared_ptr<SceTls> CreateThreadTls(std::uint64_t allocationBase) const;
    SceThreadTlsFactory ThreadTlsFactory() const;
    Mapping InitialStack() const;
    void SetExecutor(SceModuleExecutor executor);
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
