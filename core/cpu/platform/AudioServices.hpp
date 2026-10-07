#pragma once
#include <cpu/Cpu.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <array>
#include <string_view>

namespace Cpu { struct SceImport; }
namespace Cpu::Platform {
// Bounded AJM context provider. Machine must outlive the provider and its gate page.
// No decoder, instance or batch is admitted.
// Public signature provenance: boykopovar/AnyPS5 9ab937b2 Ajm.cpp;
// reserved/error corroboration: shadps4-emu/shadPS4 945dbc3c ajm.cpp/ajm_error.h.
class AudioServices final {
public:
    static constexpr std::array<std::string_view, 3> Nids{
        "dl+4eHSzUu4", "MHur6qCsUus", "Q3dyFuwGn64"};
    explicit AudioServices(Machine&, std::uint64_t gateBase = 0x7ffdc6000000);
    ~AudioServices();
    AudioServices(const AudioServices&) = delete;
    AudioServices& operator=(const AudioServices&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t type);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
