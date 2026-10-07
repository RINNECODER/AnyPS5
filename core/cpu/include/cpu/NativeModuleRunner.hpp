#pragma once

#include <cpu/GuestMemoryRuntime.hpp>
#include <cpu/SceModules.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace Cpu {
class GuestThreads;
class SceNativeGraphicsSession;

// This is a bounded diagnostic execution profile, not vendor clock semantics.
struct NativeModuleRunnerConfiguration {
    std::filesystem::path UtilityMetallib;
    std::string WindowTitle = "MacPS — native diagnostic";
    std::uint32_t Width = 1280, Height = 720;
    std::chrono::milliseconds MaximumWallTime{30000};
    std::chrono::milliseconds MaximumIdleWait{5000};
};

// Construct on the persistent CPU/AppKit main owner before graph relocation.
// Machine and scheduler must outlive this object. Its Memory() is the SAME
// deferred runtime supplied to SceMemoryImports before any guest initializer.
// Activate only after graph relocation, SetupSceEntry and AdoptInitial/SetExecutor.
// Shutdown while CPU is idle, before destroying any graph/provider/Runtime owner.
class NativeModuleRunner {
public:
    NativeModuleRunner(Machine&, std::shared_ptr<GuestThreads>,
                       SceImportConsumer mainSource, NativeModuleRunnerConfiguration);
    ~NativeModuleRunner();
    NativeModuleRunner(const NativeModuleRunner&) = delete;
    NativeModuleRunner& operator=(const NativeModuleRunner&) = delete;
    std::shared_ptr<GuestMemoryRuntime> Memory() const;
    std::optional<SceResolvedImport> Resolve(const SceImportConsumer&, const SceImport&,
                                            std::uint8_t type, std::uint64_t size);
    // The actual parsed source must have qualified the corresponding providers.
    // Guest-supplied provider precedence remains SceModules' responsibility.
    void AddHostModules(std::vector<SceHostModule>&) const;
    void ActivateBeforeInitializers();
    std::uint64_t MappingGeneration() const;
    SceNativeGraphicsSession& Graphics();
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
