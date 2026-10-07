#pragma once
#include <cpu/Cpu.hpp>
#include <cpu/SceElf.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace Cpu::Platform {
// Component candidate: the PS5 target metadata confirms this identity, but its
// argument ABI still needs target caller corroboration before engine admission.
class NetworkServices {
public:
    static constexpr std::string_view UriEscapeNid = "YuOW3dDAKYc";
    // The machine must outlive this provider; destruction releases its gate page
    // and registered callbacks on the owning CPU thread.
    explicit NetworkServices(Machine&, std::uint64_t gateBase = 0x7ffdc3000000ULL);
    ~NetworkServices();
    NetworkServices(const NetworkServices&) = delete;
    NetworkServices& operator=(const NetworkServices&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
