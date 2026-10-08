#pragma once

#include <cpu/Cpu.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <memory>
#include <optional>

namespace Cpu { class GuestThreads; }
namespace Cpu::Platform {

// A bounded public PS4 engineering candidate, not a qualified retail PS5
// provider. Machine and its idle CPU owner must outlive this component.
class NativeSocketServices final {
public:
    struct Configuration {
        bool EnablePublicFixtureCandidate = false;
        // Pin the actual ParseSce result of the independently built public
        // NativeSocketGuest.elf fixture (library ID1/module ID1). Resolve also checks the parser's immutable source identity;
        // a caller-supplied consumer name or editable public digest is not enough.
        std::array<std::byte, 32> PublicFixtureSha256{};
        std::uint64_t PublicFixtureSize = 0;
        std::uint64_t GateBase = 0x7ffdc6000000ULL;
        std::uint64_t ErrnoBase = 0x7ffdc6100000ULL;
    };
    NativeSocketServices(Machine&, const std::shared_ptr<GuestThreads>&);
    NativeSocketServices(Machine&, const std::shared_ptr<GuestThreads>&, Configuration);
    ~NativeSocketServices();
    NativeSocketServices(const NativeSocketServices&) = delete;
    NativeSocketServices& operator=(const NativeSocketServices&) = delete;
    // Rejects unqualified consumer/source/type/size/scope before allocating any
    // gate. The retained parsed symbol must be an undefined global function
    // with the exact requested import row. Only Q4qBuN-c0ZM, 45ggEzakPJQ and
    // HQOwnfMGipQ are candidates.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t observedSymbolType,
                                       std::uint64_t observedSymbolSize,
                                       const SceParsedImage& actualConsumer);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
