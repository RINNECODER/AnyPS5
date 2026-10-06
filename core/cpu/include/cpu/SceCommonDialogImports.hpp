#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// Own one provider for each Machine/session. Initialization does not open a
// dialog or manufacture UI completion, results or user decisions.
class SceCommonDialogImports {
public:
    explicit SceCommonDialogImports(Machine& machine, std::uint64_t gateBase);
    ~SceCommonDialogImports();
    SceCommonDialogImports(const SceCommonDialogImports&) = delete;
    SceCommonDialogImports& operator=(const SceCommonDialogImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
