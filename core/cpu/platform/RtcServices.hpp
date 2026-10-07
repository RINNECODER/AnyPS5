#pragma once
#include <cpu/Cpu.hpp>
#include <array>
#include <memory>
#include <optional>
#include <string_view>
namespace Cpu { struct SceImport; }
namespace Cpu::Platform {
// Bounded RTC RFC3339 component. Public PS4 ABI plus observed PS5 caller shapes;
// the packet records remaining PS5 firmware behavior qualification.
class RtcServices {
public:
    static constexpr std::uint64_t GateBase = 0x7ffdc7000000;
    static constexpr std::string_view Module = "libSceRtc";
    static constexpr std::array<std::string_view, 2> Nids{"WJ3rqFwymew", "99bMGglFW3I"};
    explicit RtcServices(Machine&, std::uint64_t gateBase = GateBase);
    ~RtcServices();
    RtcServices(const RtcServices&) = delete;
    RtcServices& operator=(const RtcServices&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
