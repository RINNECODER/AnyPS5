#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <memory>
#include <optional>
#include <string_view>

namespace Cpu { struct SceImport; }
namespace Cpu::Platform {

// Component candidate inventory: offline branch only. PS5 argument ABI must be
// corroborated before production registration; this is not an admission list.
struct NpRegistration {
    std::string_view Nid;
    std::string_view Library;
    std::string_view Module;
    std::uint16_t LibraryVersion;
    std::uint8_t ModuleMajor, ModuleMinor, Type;
};
inline constexpr std::array NpRegistrations{
    NpRegistration{"XDncXQIJUSk", "libSceNpManager", "libSceNpManager", 1, 1, 1, 2}
};

// Local session policy must agree with the signed-out state upstream sceNpGetState reports.
// Machine must outlive this provider. No credentials or fabricated account data.
class NpServices {
public:
    explicit NpServices(Machine& machine, std::uint32_t sessionUserId = 0x10000000,
                        std::uint64_t gateBase = 0x7ffdc4000000);
    ~NpServices();
    NpServices(const NpServices&) = delete;
    NpServices& operator=(const NpServices&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t type);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
