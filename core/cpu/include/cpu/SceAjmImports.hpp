#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// libSceAjm (batch/context/instance API) and libSceAudio3d for guests running under TCG.
// All guest memory is marshalled through Machine::Read/Write. No audio codec is linked: decode
// jobs complete with the AJM codec-error result and zero bytes consumed/produced, while
// initialize/clear/gapless/resample/control/statistics jobs, batch start/wait/cancel and the
// context and instance lifecycle keep the SDK semantics.
class SceAjmImports {
public:
    explicit SceAjmImports(Machine& machine, std::uint64_t gateBase = 0x7ffdd9000000);
    ~SceAjmImports();
    SceAjmImports(const SceAjmImports&) = delete;
    SceAjmImports& operator=(const SceAjmImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
