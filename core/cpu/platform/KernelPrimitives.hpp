#pragma once
#include <cpu/Cpu.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Cpu { struct SceImport; }
namespace Cpu::Platform {
// Public PS4 signatures plus exact PS5 import identity evidence. PS5 caller ABI
// qualification and blocking scheduler integration remain coordinator gates.
struct KernelPrimitiveImport { std::string_view Nid, Name; };
std::span<const KernelPrimitiveImport> KernelPrimitiveInventory();
class KernelPrimitives {
public:
    // Machine must outlive this provider. Destroy only while guest execution is idle;
    // the provider exclusively owns its gate page and unmaps it on destruction.
    // Must identify the active guest thread, never the host thread.
    KernelPrimitives(Machine&, std::function<std::uint64_t()> activeGuestThread,
                     std::uint64_t gateBase = 0x7ffdc1000000);
    ~KernelPrimitives();
    KernelPrimitives(const KernelPrimitives&) = delete;
    KernelPrimitives& operator=(const KernelPrimitives&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t symbolType);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
