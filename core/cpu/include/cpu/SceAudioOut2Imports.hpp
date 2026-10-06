#pragma once

#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// Bounded native main-output compatibility profile; unsupported title routing stays explicit.
class SceAudioOut2Imports {
public:
    explicit SceAudioOut2Imports(Machine& machine, std::uint64_t gateBase = 0x7ffdf8000000);
    ~SceAudioOut2Imports();
    SceAudioOut2Imports(const SceAudioOut2Imports&) = delete;
    SceAudioOut2Imports& operator=(const SceAudioOut2Imports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
