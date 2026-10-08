#pragma once

#include <cpu/SceElf.hpp>
#include <array>
#include <memory>
#include <optional>

namespace Cpu { class GuestThreads; }
namespace Cpu::Platform {

// Consumer-bound signed-out engineering profile. This does not authenticate a
// user or serialize an unqualified PS5 OnlineId layout. The Machine and idle
// owner must outlive this provider, which must be destroyed while idle.
class NativeNpIdentity final {
public:
    struct Configuration {
        bool EnableQualifiedConsumer = false;
        bool EnablePublicFixtureCandidate = false;
        std::array<std::byte, 32> PublicFixtureSha256{};
        std::uint64_t PublicFixtureSize = 0;
        std::uint32_t SessionUserId = 0x10000000;
        std::uint64_t GateBase = 0x7ffdc4000000ULL;
    };
    NativeNpIdentity(Machine&, const std::shared_ptr<GuestThreads>&);
    NativeNpIdentity(Machine&, const std::shared_ptr<GuestThreads>&, Configuration);
    ~NativeNpIdentity();
    NativeNpIdentity(const NativeNpIdentity&) = delete;
    NativeNpIdentity& operator=(const NativeNpIdentity&) = delete;

    // Preflight source, original symbol and complete import scope before the
    // existing NpServices allocates its first gate. Editable import rows alone
    // cannot admit a different retained symbol or immutable source identity.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t type,
                                       std::uint64_t size, const SceParsedImage&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
