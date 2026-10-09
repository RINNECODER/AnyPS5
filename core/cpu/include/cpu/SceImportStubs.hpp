#pragma once

#include <cpu/SceModules.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Cpu {

// Readable name for a NID from the HLE export declarations, when known.
std::optional<std::string_view> SceNidName(std::string_view nid);

enum class SceImportStubBinding { Trap, WeakZero, ZeroObject };
enum class SceImportStubEventKind { Bound, Called, MissingModule };

struct SceImportStubEvent {
    SceImportStubEventKind Kind = SceImportStubEventKind::Bound;
    SceImportStubBinding Binding = SceImportStubBinding::Trap;
    std::string Consumer;
    SceImport Import;
    std::optional<std::string_view> Name;
    std::string Reason;
    // Called: the configured return value, or nullopt when the call aborts.
    std::optional<std::uint64_t> ReturnValue;
    // MissingModule: the DT_NEEDED file that has no provider.
    std::string Module;
};

struct SceImportStubOptions {
    // Without a value, calling a trap stub stops the guest with a diagnostic.
    std::optional<std::uint64_t> ReturnValue;
    std::function<void(const SceImportStubEvent&)> Log;
    std::uint64_t GateBase = 0x7ffdb0000000;
    std::uint64_t ObjectBase = 0x7ffdb8000000;
};

// Lazy import fallback: each unresolved function import gets a per-NID trap stub,
// weak imports bind to 0 and unresolved object imports bind to zeroed storage.
// Machine must outlive the stubs.
class SceImportStubs {
public:
    SceImportStubs(Machine& machine, SceImportStubOptions options);
    ~SceImportStubs();
    SceImportStubs(const SceImportStubs&) = delete;
    SceImportStubs& operator=(const SceImportStubs&) = delete;
    SceResolvedImport Bind(const SceUnresolvedImport& import);
    // Policy for SceModules that binds through this object.
    SceLazyImports Policy();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
