#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Cpu { struct SceImport; }
namespace Cpu::Platform {

// This public Orbis contract is a candidate for target corroboration, not a PS5 ABI claim.
enum class ContentAbi { PublicOrbisCandidate };
enum class ContentRegistrationStatus { Candidate };
struct ContentRegistration {
    std::string_view nid;
    std::string_view library;
    std::uint16_t libraryVersion;
    std::string_view module;
    std::uint8_t moduleMajor;
    std::uint8_t moduleMinor;
    ContentRegistrationStatus status;
};

// Supplied by the owning title's installed-metadata provider; no implied full SKU or zero values.
// The owner retains this immutable record for the lifetime of its title session.
struct InstalledContentRecord {
    std::int32_t skuFlag;
    std::array<std::optional<std::int32_t>, 4> userDefined;
    std::string provenance;
};

class ContentServices {
public:
    // The Machine owner must outlive this provider and tear it down while the Machine is idle.
    explicit ContentServices(Machine& machine,
        std::weak_ptr<const InstalledContentRecord> record = {},
        std::optional<ContentAbi> abi = std::nullopt,
        std::uint64_t gateBase = 0x7ffdc2000000);
    ~ContentServices();
    ContentServices(const ContentServices&) = delete;
    ContentServices& operator=(const ContentServices&) = delete;
    // No explicit ABI opt-in means unresolved. Local importer IDs are not provider identities.
    std::optional<std::uint64_t> Resolve(const SceImport& import, std::uint8_t type);
    static constexpr std::array<ContentRegistration, 1> Inventory{{
        {"99b82IKXpH4", "libSceAppContent", 1, "libSceAppContentUtil", 1, 1,
         ContentRegistrationStatus::Candidate}
    }};
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
