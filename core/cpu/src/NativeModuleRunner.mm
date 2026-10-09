#include <cpu/NativeModuleRunner.hpp>
#include <cpu/GuestMemoryMetal.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceAgcImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include "KernelEvents.hpp"
#include "KernelPrimitives.hpp"
#include "KernelSemaphores.hpp"
#include "NativeNpIdentity.hpp"
#include "NativeUriEscape.hpp"
#include "SceImageData.hpp"
#include "NativeAgcBackend.hpp"
#include "BdaAbi.hpp"
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <algorithm>
#include <atomic>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <thread>

namespace Cpu {
namespace {
std::string sourceHash(const SceImportConsumer& source) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : source.SourceSha256) {
        const auto n = std::to_integer<unsigned>(byte);
        result.push_back(digits[n >> 4]); result.push_back(digits[n & 15]);
    }
    return result;
}
bool qualifiedSize(const SceImportConsumer& source) {
    const auto name = source.Path.filename().string();
    const auto hash = sourceHash(source);
    return (name == "eboot.bin" && source.SourceSize == 102560655 && hash ==
            "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397") ||
           (name == "libc.prx" && source.SourceSize == 1875018 && hash ==
            "78a080fdeccc28f2aa76356e97f82a35b3ba09deba8408dfce27db28fa0ce67f") ||
           (name == "libSceNpCppWebApi.prx" && source.SourceSize == 7895047 && hash ==
            "38db047fd9dfd27fc17dfc0dd2cff31a2e0533ac1be2350e5082f8499f59c6b9");
}
GuestMemoryMetalCompositor::OwnedGpuAccess ownedAccess(const OwnedMappingView& view) {
    const auto permissions = static_cast<unsigned>(view.Region.Permissions);
    if (!(permissions & static_cast<unsigned>(Permission::Read)))
        return GuestMemoryMetalCompositor::OwnedGpuAccess::CpuOnly;
    // The native written-page ABI encodes page+1 in uint32. High CPU
    // stack/TLS mappings remain genuinely readable by the GPU; GPU writes to
    // them are explicitly unsupported until that ABI can represent the page.
    constexpr auto writableLimit = std::uint64_t{std::numeric_limits<std::uint32_t>::max()}
        << ShaderRecompiler::BdaAbi::WrittenPageShift;
    const bool writable = (permissions & static_cast<unsigned>(Permission::Write)) &&
        view.Region.Address <= writableLimit && view.Region.Size <= writableLimit - view.Region.Address;
    return writable ? GuestMemoryMetalCompositor::OwnedGpuAccess::ReadWrite
                    : GuestMemoryMetalCompositor::OwnedGpuAccess::ReadOnly;
}
}

struct NativeModuleRunner::Impl {
    enum class Phase { Loading, Active, Drained };
    struct Dispatch {
        Phase phase = Phase::Loading;
        GuestMemoryRuntime::Transaction active;
        void transact(const GuestMemorySnapshot& previous, const GuestMemorySnapshot& candidate,
                      const std::function<void()>& commit) {
            if (phase == Phase::Drained) { commit(); return; }
            if (phase != Phase::Active || !active)
                throw std::runtime_error("Native module runtime mutation before initial publication");
            active(previous, candidate, commit);
        }
    };
    struct Completion {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        // No target flip-event subscription is admitted. These are bounded host
        // ordering receipts, never a fabricated guest event or guest-memory write.
        std::atomic<std::uint64_t> flipReceipts{0};
        static std::uint64_t time(void* context) {
            auto& c = *static_cast<Completion*>(context);
            return std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - c.start).count();
        }
        static void flip(void* context, VideoOutConfig&, std::int64_t) {
            ++static_cast<Completion*>(context)->flipReceipts;
        }
    };
    Machine& machine;
    std::shared_ptr<GuestThreads> threads;
    SceImportConsumer source;
    NativeModuleRunnerConfiguration config;
    std::thread::id owner = std::this_thread::get_id();
    std::shared_ptr<Dispatch> dispatch = std::make_shared<Dispatch>();
    std::shared_ptr<GuestMemoryRuntime> memory;
    std::unique_ptr<SceThreadImports> threadImports;
    std::unique_ptr<Platform::TargetKernelMutexes> mutexes;
    std::unique_ptr<Platform::TargetKernelEvents> events;
    std::unique_ptr<Platform::TargetKernelSemaphores> semaphores;
    std::map<std::filesystem::path, SceParsedImage> serviceConsumers;
    std::unique_ptr<Platform::NativeNpIdentity> npIdentity;
    std::unique_ptr<Platform::NativeUriEscape> uriEscape;
    Completion completion;
    std::unique_ptr<SceNativeGraphicsSession> graphics;
    std::unique_ptr<SceAgcImports> agc;
    // Retained until graphics is drained, including all failed candidate owners.
    std::unique_ptr<GuestMemoryMetalCompositor> compositor;
    std::chrono::steady_clock::time_point started;
    bool hookInstalled = false, shutdown = false;
    std::exception_ptr shutdownFailure;

    Impl(Machine& m, std::shared_ptr<GuestThreads> scheduler, SceImportConsumer mainSource,
         NativeModuleRunnerConfiguration configuration)
        : machine(m), threads(std::move(scheduler)), source(std::move(mainSource)), config(std::move(configuration)) {
        if (!threads || config.MaximumWallTime.count() < 0 || config.MaximumIdleWait.count() < 0 ||
            config.SessionUserId == 0xffffffffu)
            throw std::invalid_argument("Native module runner requires scheduler and non-negative diagnostic bounds");
        threads->CheckIdleOwner();
        memory = std::make_shared<GuestMemoryRuntime>(machine, 12ULL << 30,
            [slot = dispatch](const auto& previous, const auto& candidate, const auto& commit) {
                slot->transact(previous, candidate, commit);
            });
        threadImports = std::make_unique<SceThreadImports>(machine, threads);
        mutexes = std::make_unique<Platform::TargetKernelMutexes>(machine, threads);
        semaphores = std::make_unique<Platform::TargetKernelSemaphores>(machine, threads);
        const auto hash = qualifiedSize(source) ? sourceHash(source) : std::string{};
        events = std::make_unique<Platform::TargetKernelEvents>(machine, threads,
            Platform::QualifiedKernelEventAdmissionsForImage(hash));
        AnyPS5::Host::NativeMetalSessionConfiguration native;
        native.window = {config.WindowTitle, config.Width, config.Height};
        native.utilityMetallib = config.UtilityMetallib;
        native.initialGeneration = 0;
        native.eopInterrupt = events->Provider().EopPublisher();
        const VideoOutCompletionCallbacks callbacks{&completion, Completion::time, Completion::time, Completion::flip};
        graphics = SceNativeGraphicsSession::CreateMainThread(machine, native, callbacks, {},
            0x7ffdfd000000, QualifiedVideoOutAdmissionsForImage(hash));
        auto backend = MakeNativeAgcBackend(graphics->Driver());
        backend.AddEvent = [this](std::uint64_t handle, std::int32_t id, std::uint64_t user) {
            return events->Provider().AddGraphicsEvent(handle, id, user);
        };
        backend.DeleteEvent = [this](std::uint64_t handle, std::int32_t id) {
            return events->Provider().DeleteGraphicsEvent(handle, id);
        };
        // Main also owns SceNetAddressImports at the legacy AGC default.
        // Keep this native profile's gate separate from every legacy provider.
        agc = std::make_unique<SceAgcImports>(machine, std::move(backend),
            QualifiedAgcAdmissionsForImage(hash), 0x7ffdc8000000);
    }
    ~Impl() {
        try { close(); }
        catch (const std::exception& error) {
            // Keep the primary loader/execution exception intact on unwinding,
            // but retain bounded secondary teardown evidence in the launch log.
            try { std::cerr << "Native module secondary teardown failure: "
                            << std::string_view(error.what()).substr(0, 1024) << '\n'; } catch (...) {}
        } catch (...) {
            try { std::cerr << "Native module secondary teardown failure: unknown exception\n"; } catch (...) {}
        }
    }
    void checkOwner() const {
        if (std::this_thread::get_id() != owner)
            throw std::runtime_error("Native module runner requires persistent CPU/AppKit owner");
        auto idle = machine.CaptureContext();
    }
    void close() {
        checkOwner();
        if (shutdown) { if (shutdownFailure) std::rethrow_exception(shutdownFailure); return; }
        const auto attempt = [&](auto&& operation) {
            try { operation(); } catch (...) { if (!shutdownFailure) shutdownFailure = std::current_exception(); }
        };
        machine.RequestStop();
        if (graphics) {
            attempt([&] { graphics->RequestStop(); });
            // On this persistent main owner shutdown joins producers before
            // reporting retained failures. Keep all CPU/compositor owners alive.
            attempt([&] { graphics->ShutdownAfterCpuStoppedMainThread(); });
            graphics.reset();
        }
        if (hookInstalled) {
            attempt([&] { machine.SetOwnedMappingTransaction({}); });
            hookInstalled = false;
        }
        dispatch->phase = Phase::Drained;
        dispatch->active = {};
        attempt([&] { memory->Shutdown(); });
        npIdentity.reset(); uriEscape.reset();
        attempt([&] { threads->Withdraw(); });
        if (events) attempt([&] { events->Provider().Shutdown(); });
        if (semaphores) attempt([&] { semaphores->Provider().Shutdown(); });
        agc.reset(); events.reset(); semaphores.reset(); mutexes.reset(); threadImports.reset();
        compositor.reset();
        shutdown = true;
        if (shutdownFailure) std::rethrow_exception(shutdownFailure);
    }
    GuestMemoryMetalCompositor::Publisher publisher() {
        return [this](auto ranges, auto generation, const auto& commit, auto oldOwner, auto newOwner) {
            if (!graphics) throw std::runtime_error("Native graphics publication after drain");
            graphics->MutateBorrowedRanges(ranges, generation, commit, std::move(oldOwner), std::move(newOwner));
        };
    }
    void activate() {
        checkOwner();
        if (shutdown || compositor || dispatch->phase != Phase::Loading)
            throw std::runtime_error("Native module initial publication may occur only once");
        const auto empty = memory->Snapshot();
        auto full = machine.PinOwnedMappings();
        auto selected = full;
        std::erase_if(selected.Views, [](const auto& view) {
            return ownedAccess(view) == GuestMemoryMetalCompositor::OwnedGpuAccess::CpuOnly;
        });
        std::vector<std::uint64_t> writable;
        for (const auto& view : selected.Views)
            if (ownedAccess(view) == GuestMemoryMetalCompositor::OwnedGpuAccess::ReadWrite)
                writable.push_back(view.Region.Address);
        compositor = std::make_unique<GuestMemoryMetalCompositor>(machine, std::move(selected), writable);
        compositor->BindRuntime(empty);
        const auto initial = compositor->InitialMappings();
        // previousOwner is an actual empty runtime snapshot lease, not a guessed
        // address or a fabricated CPU allocation owner.
        auto bootstrap = std::make_shared<GuestMemorySnapshot>(empty);
        graphics->MutateBorrowedRanges(initial.Ranges, compositor->Generation(), [] {}, bootstrap, initial.Owner);
        auto publish = publisher();
        dispatch->active = compositor->MakeTransaction(publish);
        machine.SetOwnedMappingTransaction(compositor->MakeOwnedTransaction(publish, ownedAccess));
        hookInstalled = true;
        dispatch->phase = Phase::Active;
        started = std::chrono::steady_clock::now();
        threads->SetOwnerBoundary([this](bool waiting) {
            graphics->Window().PumpMainThread(waiting ? std::chrono::milliseconds{1} : std::chrono::milliseconds{0}, 256);
            const auto state = graphics->Window().Snapshot();
            if (!state.open || state.closeRequested || (config.MaximumWallTime.count() > 0 &&
                std::chrono::steady_clock::now() - started >= config.MaximumWallTime)) {
                machine.RequestStop(); graphics->RequestStop();
            }
        }, config.MaximumIdleWait);
    }
};

NativeModuleRunner::NativeModuleRunner(Machine& machine, std::shared_ptr<GuestThreads> threads,
    SceImportConsumer source, NativeModuleRunnerConfiguration config)
    : impl(std::make_unique<Impl>(machine, std::move(threads), std::move(source), std::move(config))) {}
NativeModuleRunner::~NativeModuleRunner() = default;
std::shared_ptr<GuestMemoryRuntime> NativeModuleRunner::Memory() const { return impl->memory; }
void NativeModuleRunner::RegisterParsedConsumer(const SceParsedImage& image) {
    impl->checkOwner();
    if (impl->shutdown || impl->dispatch->phase != Impl::Phase::Loading)
        throw std::runtime_error("Native service consumer registration after loading");
    if (!image.Data || image.Path.empty() || !image.SourceSize ||
        image.SourceSize != image.Data->SourceSize || image.SourceSha256 != image.Data->SourceSha256 ||
        !impl->serviceConsumers.emplace(image.Path, image).second)
        throw std::runtime_error("Unsupported native service parsed consumer snapshot");
}
SceNativeGraphicsSession& NativeModuleRunner::Graphics() {
    impl->checkOwner();
    if (!impl->graphics) throw std::runtime_error("Native module graphics is drained");
    return *impl->graphics;
}
std::uint64_t NativeModuleRunner::MappingGeneration() const {
    return impl->compositor ? impl->compositor->Generation() : 0;
}
void NativeModuleRunner::ActivateBeforeInitializers() { impl->activate(); }
void NativeModuleRunner::Shutdown() { impl->close(); }

void NativeModuleRunner::AddHostModules(std::vector<SceHostModule>& hosts) const {
    impl->checkOwner();
    const auto serviceSource = [&](const char* targetName, const char* publicName, const auto& profile) {
        return std::any_of(impl->serviceConsumers.begin(), impl->serviceConsumers.end(), [&](const auto& entry) {
            const auto& image = entry.second;
            const SceImportConsumer identity{image.Path, image.SourceSize, image.SourceSha256};
            return (impl->config.EnableQualifiedServiceConsumers && image.Path.filename() == targetName &&
                    image.SourceContainer == "plain_self" && qualifiedSize(identity)) ||
                   (profile && image.Path.filename() == publicName && image.SourceContainer == "elf" &&
                    image.Type == 0xfe10 && image.SourceSize == profile->SourceSize &&
                    image.SourceSha256 == profile->SourceSha256);
        });
    };
    const auto add = [&](const char* name) {
        if (std::none_of(hosts.begin(), hosts.end(), [&](const auto& host) { return host.Module.Name == name; }))
            hosts.push_back({std::string(name) + ".prx", {name, 0, 1, 1}, {{name, 0, 1}}});
    };
    if (serviceSource("eboot.bin", "NativeNpIdentityGuest.elf", impl->config.PublicNpIdentity)) add("libSceNpManager");
    if (serviceSource("libSceNpCppWebApi.prx", "NativeUriGuest.elf", impl->config.PublicUriEscape)) add("libSceHttp");
    if (qualifiedSize(impl->source) && !QualifiedAgcAdmissionsForImage(sourceHash(impl->source)).empty()) {
        for (const auto* name : {"libSceAgc", "libSceAgcDriver", "libSceVideoOut"})
            hosts.push_back({std::string(name) + ".prx", {name, 0, 1, 1}, {{name, 0, 1}}});
        for (auto& host : hosts) if (host.Module.Name == "libkernel")
            host.Libraries.push_back({"libScePosix", 0, 1});
    }
}

std::optional<SceResolvedImport> NativeModuleRunner::Resolve(const SceImportConsumer& consumer,
    const SceImport& import, std::uint8_t type, std::uint64_t size) {
    impl->checkOwner();
    if (impl->shutdown || impl->dispatch->phase == Impl::Phase::Drained)
        throw std::runtime_error("Native module import resolution after shutdown");
    const auto name = consumer.Path.filename().string();
    const auto hash = sourceHash(consumer);
    if (import.Nid == "XDncXQIJUSk" || import.Nid == "YuOW3dDAKYc") {
        const auto found = impl->serviceConsumers.find(consumer.Path);
        if (type != 2 || size || found == impl->serviceConsumers.end() ||
            found->second.SourceSize != consumer.SourceSize || found->second.SourceSha256 != consumer.SourceSha256)
            throw std::runtime_error("Unsupported native service actual consumer/type/size");
        std::optional<std::uint64_t> address;
        if (import.Nid == "XDncXQIJUSk") {
            if (!impl->npIdentity) {
                Platform::NativeNpIdentity::Configuration config;
                config.EnableQualifiedConsumer = impl->config.EnableQualifiedServiceConsumers;
                config.SessionUserId = impl->config.SessionUserId;
                if (impl->config.PublicNpIdentity) {
                    config.EnablePublicFixtureCandidate = true;
                    config.PublicFixtureSha256 = impl->config.PublicNpIdentity->SourceSha256;
                    config.PublicFixtureSize = impl->config.PublicNpIdentity->SourceSize;
                }
                impl->npIdentity = std::make_unique<Platform::NativeNpIdentity>(impl->machine, impl->threads, config);
            }
            address = impl->npIdentity->Resolve(import, type, size, found->second);
        } else {
            if (!impl->uriEscape) {
                Platform::NativeUriEscape::Configuration config;
                config.EnableTargetConsumer = impl->config.EnableQualifiedServiceConsumers;
                if (impl->config.PublicUriEscape) {
                    config.EnablePublicFixture = true;
                    config.PublicFixtureSha256 = impl->config.PublicUriEscape->SourceSha256;
                    config.PublicFixtureSize = impl->config.PublicUriEscape->SourceSize;
                }
                impl->uriEscape = std::make_unique<Platform::NativeUriEscape>(impl->machine, impl->threads, config);
            }
            address = impl->uriEscape->Resolve(import, type, size, found->second);
        }
        if (!address) throw std::runtime_error("Unsupported native service retained consumer/import row");
        return SceResolvedImport{*address, type};
    }
    // Pass the actual parsed importing image's identity. This provider admits
    // only the four observed semaphore rows for the exact target consumer,
    // and validates function type/size and full scope before allocating gates.
    if (const auto address = impl->semaphores->Resolve(import, type, size,
            {name, hash, consumer.SourceSize}))
        return SceResolvedImport{*address, type};
    const bool graphicsScope = import.ModuleName == "libSceAgc" || import.ModuleName == "libSceAgcDriver" ||
        import.LibraryName == "libSceAgc" || import.LibraryName == "libSceAgcDriver" ||
        import.ModuleName == "libSceVideoOut" || import.LibraryName == "libSceVideoOut";
    if (graphicsScope) {
        if (!qualifiedSize(consumer) || name != impl->source.Path.filename().string() ||
            consumer.SourceSize != impl->source.SourceSize || consumer.SourceSha256 != impl->source.SourceSha256)
            throw std::runtime_error("Unsupported native graphics actual consumer source");
        if (import.ModuleName == "libSceVideoOut" || import.LibraryName == "libSceVideoOut")
            return SceResolvedImport{impl->graphics->ResolveVideoOut(import, type, size), type};
        return SceResolvedImport{impl->agc->Resolve(import, type, size), type};
    }
    // Priority/attribute contracts have their own actual-source route. The
    // ordinary thread family retains its explicitly declared legacy scope.
    // Invalid size is supplied as an invalid observed type so a recognized row
    // rejects BEFORE allocating its callable gate, while unrelated objects pass.
    const auto functionType = type == 2 && size == 0 ? type : std::uint8_t{0};
    const auto admittedName = qualifiedSize(consumer) ? name : std::string{};
    if (const auto address = impl->threadImports->ResolveTargetPriority(import, functionType, {admittedName, hash}))
        return SceResolvedImport{*address, type};
    if (const auto address = impl->threadImports->Resolve(import, functionType))
        return SceResolvedImport{*address, type};
    const bool primitive = std::any_of(Platform::KernelPrimitiveInventory().begin(), Platform::KernelPrimitiveInventory().end(),
        [&](const auto& row) { return row.Nid == import.Nid; });
    const bool condition = std::any_of(Platform::KernelConditionInventory().begin(), Platform::KernelConditionInventory().end(),
        [&](const auto& row) { return row.Nid == import.Nid; });
    const bool event = std::any_of(Platform::KernelEventInventory().begin(), Platform::KernelEventInventory().end(),
        [&](const auto& row) { return row.Nid == import.Nid; });
    if ((primitive || condition || event) && (!qualifiedSize(consumer) || type != 2 || size != 0))
        throw std::runtime_error("Unsupported native kernel actual consumer source/type/size");
    if (const auto address = impl->mutexes->ResolveCondition(import, type, size, {admittedName, hash}))
        return SceResolvedImport{*address, type};
    if (const auto address = impl->mutexes->Resolve(import, functionType, {admittedName, hash}))
        return SceResolvedImport{*address, type};
    if (const auto address = impl->events->Resolve(import, type, size, {name, hash}))
        return SceResolvedImport{*address, type};
    return std::nullopt;
}
}
