#include "CandidateComponents.hpp"
#include "AudioServices.hpp"
#include "KernelPrimitives.hpp"
#include "NetworkServices.hpp"
#include "NpServices.hpp"
#include "RtcServices.hpp"
#include <stdexcept>

namespace Cpu::Platform {

struct CandidateComponents::Impl {
    std::unique_ptr<KernelPrimitives> kernel;
    std::unique_ptr<ContentServices> content;
    std::unique_ptr<NetworkServices> network;
    std::unique_ptr<NpServices> np;
    std::unique_ptr<AudioServices> audio;
    std::unique_ptr<RtcServices> rtc;

    Impl(Machine& machine, const Configuration& configuration) {
        const auto& enabled = configuration.PublicAbiFixtures;
        // Validate configuration before any provider changes guest mappings.
        if (enabled.contains(CandidateFamily::Kernel) && !configuration.ActiveGuestThread)
            throw std::invalid_argument("Kernel fixture registration needs active guest-thread ownership");
        if (enabled.contains(CandidateFamily::Np) &&
            (!configuration.SessionUser || *configuration.SessionUser == 0xffffffffu))
            throw std::invalid_argument("NP fixture registration needs an explicit owned session user");
        if (enabled.contains(CandidateFamily::Content)) {
            const auto record = configuration.InstalledContent.lock();
            if (!record || record->provenance.empty() || (record->skuFlag != 1 && record->skuFlag != 3))
                throw std::invalid_argument("Content fixture registration needs an owned installed record");
        }
        if (enabled.contains(CandidateFamily::Kernel))
            kernel = std::make_unique<KernelPrimitives>(machine, configuration.ActiveGuestThread);
        if (enabled.contains(CandidateFamily::Content))
            content = std::make_unique<ContentServices>(machine, configuration.InstalledContent,
                                                        ContentAbi::PublicOrbisCandidate);
        if (enabled.contains(CandidateFamily::Network)) network = std::make_unique<NetworkServices>(machine);
        if (enabled.contains(CandidateFamily::Np)) np = std::make_unique<NpServices>(machine, *configuration.SessionUser);
        if (enabled.contains(CandidateFamily::Audio)) audio = std::make_unique<AudioServices>(machine);
        if (enabled.contains(CandidateFamily::Rtc)) rtc = std::make_unique<RtcServices>(machine);
    }
};

CandidateComponents::CandidateComponents(Machine& machine, Configuration configuration)
    : impl(std::make_unique<Impl>(machine, configuration)) {}
CandidateComponents::~CandidateComponents() = default;

std::optional<std::uint64_t> CandidateComponents::Resolve(const SceImport& import, std::uint8_t type) {
    if (impl->kernel) if (auto gate = impl->kernel->Resolve(import, type)) return gate;
    if (impl->content) if (auto gate = impl->content->Resolve(import, type)) return gate;
    if (impl->network) if (auto gate = impl->network->Resolve(import, type)) return gate;
    if (impl->np) if (auto gate = impl->np->Resolve(import, type)) return gate;
    if (impl->audio) if (auto gate = impl->audio->Resolve(import, type)) return gate;
    if (impl->rtc) if (auto gate = impl->rtc->Resolve(import, type)) return gate;
    return std::nullopt;
}

}
