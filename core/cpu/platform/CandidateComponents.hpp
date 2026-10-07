#pragma once

#include "ContentServices.hpp"
#include <cpu/SceElf.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <set>

namespace Cpu::Platform {

enum class CandidateFamily { Kernel, Content, Network, Np, Audio, Rtc };

// Registration fragment for explicit public-ABI component acceptance. This is
// not a qualified PS5 host-provider list. Main must not select these contracts
// until its typed importer and target ABI qualification are independently owned.
class CandidateComponents final {
public:
    struct Configuration {
        std::set<CandidateFamily> PublicAbiFixtures;
        std::function<std::uint64_t()> ActiveGuestThread;
        std::weak_ptr<const InstalledContentRecord> InstalledContent;
        std::optional<std::uint32_t> SessionUser;
    };
    explicit CandidateComponents(Machine&, Configuration configuration = {});
    ~CandidateComponents();
    CandidateComponents(const CandidateComponents&) = delete;
    CandidateComponents& operator=(const CandidateComponents&) = delete;
    // SceImport currently lacks symbol type. The integrating loader must supply
    // observed type explicitly; assuming STT_FUNC would erase an ABI gate.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t observedSymbolType);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
