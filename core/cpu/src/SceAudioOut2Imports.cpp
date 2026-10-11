#include <cpu/SceAudioOut2Imports.hpp>
#include <cpu/NativeAudioOutput.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace Cpu {
namespace {

enum class Service {
    Initialize, UserCreate, UserDestroy, ResetParam, QueryMemory, ContextCreate, ContextDestroy,
    QueueLevel, Advance, Push, PortCreate, PortDestroy, PortState, PortAttributes,
    ContextAttributes, SpeakerInfo, Latency, MasteringInit, MasteringTerm, MasteringParam
};
constexpr std::uint32_t localUser = 0x10000000;
constexpr std::size_t scratchSize = 0x10000;
constexpr auto rw = Permission::Read | Permission::Write;

[[noreturn]] void fault(const std::string& message) {
    throw std::runtime_error("Unsupported SCE AudioOut2: " + message);
}
std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
        " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
        std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
        " id=" + std::to_string(import.ModuleId);
}
std::uint64_t integer(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8);
    return value;
}
void integer(std::span<std::byte> bytes, std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index)
        bytes[offset + index] = std::byte((value >> (index * 8)) & 0xff);
}
bool zero(std::span<const std::byte> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte value) { return value == std::byte{0}; });
}

}

struct SceAudioOut2Imports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    struct Parameters {
        std::uint32_t ports, objects, guaranteed, depth, grain;
    };
    struct Context {
        Parameters parameters;
        std::uint64_t scratch;
        std::unique_ptr<NativeAudioOutput> output;
    };
    struct Port {
        std::uint64_t context, user;
        std::uint32_t channels;
        std::uint64_t pcm = 0;
        std::array<float, 2> gains{1, 1};
    };
    Machine& machine;
    SceHostTrampolines trampolines;
    bool initialized = false;
    // Creation serials never recycle within a session, including after destruction.
    std::uint64_t nextHandle = 1;
    std::map<std::uint64_t, std::uint32_t> users;
    std::map<std::uint64_t, Context> contexts;
    std::map<std::uint64_t, Port> ports;
    std::map<Key, std::uint64_t> gates;
    const std::map<std::string, Service> services{
        {"g2tViFIohHE", Service::Initialize}, {"xywYcRB7nbQ", Service::UserCreate},
        {"IaZXJ9M79uo", Service::UserDestroy}, {"t5YrizufpQc", Service::ResetParam},
        {"pDmme7Bgm6E", Service::QueryMemory}, {"0x6o1VVAYSY", Service::ContextCreate},
        {"on6ZH7Abo10", Service::ContextDestroy}, {"R7d0F1g2qsU", Service::QueueLevel},
        {"PE2zHMqLSHs", Service::Advance}, {"aII9h5nli9U", Service::Push},
        {"JK2wamZPzwM", Service::PortCreate}, {"cd+Rtw+D1x8", Service::PortDestroy},
        {"gatEUKG+Ea4", Service::PortState}, {"8XTArSPyWHk", Service::PortAttributes},
        {"4dq2rblWlg0", Service::ContextAttributes}, {"DImz2Ft9E2g", Service::SpeakerInfo},
        {"TViD1EZXkNI", Service::Latency}, {"XHl38ZNknbs", Service::MasteringInit},
        {"2bbBBOkH4CY", Service::MasteringTerm}, {"v8iOE+j8a5o", Service::MasteringParam}};

    Impl(Machine& guest, std::uint64_t base)
        : machine(guest), trampolines(guest, base, SceHostTrampolines::DefaultCapacity, "SCE AudioOut2 import") {}
    // Backend destruction owns nonthrowing native teardown; explicit Close can report failures.
    ~Impl() = default;
    void access(std::uint64_t address, std::size_t size, Permission permission) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address)
            fault("invalid guest span");
        machine.CheckAccess(address, size, permission);
    }
    template<std::size_t Size> std::array<std::byte, Size> read(std::uint64_t address) const {
        access(address, Size, Permission::Read);
        std::array<std::byte, Size> bytes;
        machine.Read(address, bytes);
        return bytes;
    }
    void write(std::uint64_t address, std::uint64_t value, std::size_t size) {
        access(address, size, Permission::Write);
        std::array<std::byte, 8> bytes{};
        integer(bytes, 0, value, size);
        machine.Write(address, std::span(bytes).first(size));
    }
    std::uint64_t handle() {
        if (nextHandle == std::numeric_limits<std::uint64_t>::max()) fault("session handle serial exhausted");
        return nextHandle++;
    }
    void requireInitialized() const {
        if (!initialized) fault("audio session is not initialized");
    }
    Parameters parameters(std::uint64_t address) const {
        const auto bytes = read<64>(address);
        Parameters value{
            static_cast<std::uint32_t>(integer(bytes, 0, 4)), static_cast<std::uint32_t>(integer(bytes, 4, 4)),
            static_cast<std::uint32_t>(integer(bytes, 8, 4)), static_cast<std::uint32_t>(integer(bytes, 12, 4)),
            static_cast<std::uint32_t>(integer(bytes, 16, 4))};
        // Explicit native compatibility bounds, not recovered SDK limits/defaults.
        if (!value.ports || value.ports > 256 || value.objects > 256 || value.ports + value.objects > 256 ||
            value.guaranteed > value.objects || value.depth < 2 || value.depth > 64 ||
            value.grain < 64 || value.grain > 4096 || integer(bytes, 20, 4) != 1 ||
            !zero(std::span(bytes).subspan(24)))
            fault("context parameters outside bounded native profile");
        return value;
    }
    Context& context(std::uint64_t id) {
        const auto found = contexts.find(id);
        if (found == contexts.end()) fault("invalid or stale context handle");
        access(found->second.scratch, scratchSize, rw);
        return found->second;
    }
    NativeAudioOutput::Snapshot live(Context& value) const {
        const auto snapshot = value.output->GetSnapshot();
        if (!snapshot.running || snapshot.nativeError)
            fault("native audio output stopped or failed (nativeError=" + std::to_string(snapshot.nativeError) + ")");
        if (snapshot.capacityFrames != value.parameters.depth * value.parameters.grain || snapshot.queuedFrames > snapshot.capacityFrames)
            fault("native audio queue snapshot is inconsistent with its context");
        return snapshot;
    }
    Port& port(std::uint64_t id) {
        const auto found = ports.find(id);
        if (found == ports.end()) fault("invalid or stale port handle");
        return found->second;
    }
    void userCreate(std::uint32_t id, std::uint64_t destination) {
        requireInitialized();
        if (id != localUser) fault("unsupported local user ID");
        if (users.size() == 256) fault("native profile user-handle capacity exceeded");
        access(destination, 8, Permission::Write);
        const auto idHandle = handle();
        users.emplace(idHandle, id);
        try { write(destination, idHandle, 8); }
        catch (...) { users.erase(idHandle); throw; }
    }
    void userDestroy(std::uint64_t id) {
        if (!users.contains(id)) fault("invalid or stale user handle");
        if (std::any_of(ports.begin(), ports.end(), [id](const auto& item) { return item.second.user == id; }))
            fault("user handle still owns ports");
        users.erase(id);
    }
    void reset(std::uint64_t address) {
        access(address, 64, Permission::Write);
        std::array<std::byte, 64> bytes{};
        // Caller-grounded conditional defaults; these are not a complete SDK default contract.
        constexpr std::array<std::uint32_t, 6> defaults{16, 128, 0, 4, 512, 1};
        for (std::size_t index = 0; index < defaults.size(); ++index) integer(bytes, index * 4, defaults[index], 4);
        machine.Write(address, bytes);
    }
    void create(std::uint64_t parameterAddress, std::uint64_t buffer, std::uint64_t size, std::uint64_t destination) {
        requireInitialized();
        const auto params = parameters(parameterAddress);
        if (contexts.size() == 4) fault("native profile context capacity exceeded");
        if ((buffer & 15) || size < scratchSize) fault("context scratch requires aligned 65536-byte guest buffer");
        access(buffer, scratchSize, rw);
        access(destination, 8, Permission::Write);
        auto output = NativeAudioOutput::Open({params.depth * params.grain});
        if (!output) fault("native output creation returned no stream");
        Context value{params, buffer, std::move(output)};
        (void)live(value);
        const auto id = handle();
        contexts.emplace(id, std::move(value));
        try { write(destination, id, 8); }
        catch (...) { contexts.erase(id); throw; }
    }
    void destroy(std::uint64_t id) {
        const auto found = contexts.find(id);
        if (found == contexts.end()) fault("invalid or stale context handle");
        found->second.output->Close();
        std::erase_if(ports, [id](const auto& item) { return item.second.context == id; });
        contexts.erase(found);
    }
    void queue(std::uint64_t id, std::uint64_t levelAddress, std::uint64_t availableAddress) {
        auto& value = context(id);
        const auto snapshot = live(value);
        if (levelAddress) access(levelAddress, 4, Permission::Write);
        if (availableAddress) access(availableAddress, 4, Permission::Write);
        const auto grain = value.parameters.grain;
        const auto queued = snapshot.queuedFrames;
        const auto level = (queued + grain - 1) / grain;
        const auto available = (snapshot.capacityFrames - queued) / grain;
        if (levelAddress) write(levelAddress, level, 4);
        if (availableAddress) write(availableAddress, available, 4);
    }
    void portCreate(std::uint64_t contextId, std::uint64_t parameterAddress, std::uint64_t destination) {
        auto& owner = context(contextId);
        (void)live(owner);
        const auto bytes = read<64>(parameterAddress);
        const auto format = integer(bytes, 4, 4), user = integer(bytes, 16, 8);
        // Main output only. Object type0x100, pad type6 and spatial routing stay unsupported.
        if (integer(bytes, 0, 2) != 0 || (format != 0x100 && format != 0x200) ||
            integer(bytes, 8, 4) != 48000 || integer(bytes, 12, 4) || !zero(std::span(bytes).subspan(24)))
            fault("port parameters outside float32 main mono/stereo 48000Hz profile");
        if (!users.contains(user)) fault("invalid or stale port user handle");
        const auto used = std::count_if(ports.begin(), ports.end(), [contextId](const auto& item) { return item.second.context == contextId; });
        if (static_cast<std::size_t>(used) >= owner.parameters.ports) fault("context main-port capacity exceeded");
        access(destination, 8, Permission::Write);
        const auto id = handle();
        ports.emplace(id, Port{contextId, user, static_cast<std::uint32_t>(format >> 8)});
        try { write(destination, id, 8); }
        catch (...) { ports.erase(id); throw; }
    }
    void portState(std::uint64_t id, std::uint64_t destination) {
        const auto& value = port(id);
        (void)live(context(value.context));
        access(destination, 64, Permission::Write);
        // Virtual native main/stereo output profile, not a claim of console hardware layout fidelity.
        std::array<std::byte, 64> bytes{};
        integer(bytes, 0, 1, 2); integer(bytes, 2, 2, 1); integer(bytes, 4, 127, 2);
        machine.Write(destination, bytes);
    }
    void attributes(std::uint64_t id, std::uint64_t address, std::uint32_t count) {
        auto& value = port(id);
        (void)live(context(value.context));
        if (count > 256) fault("attribute count exceeds bounded native profile");
        if (!count) return;
        access(address, std::size_t(count) * 24, Permission::Read);
        std::vector<std::byte> bytes(std::size_t(count) * 24);
        machine.Read(address, bytes);
        auto updated = value;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto offset = std::size_t(index) * 24;
            const auto attribute = integer(bytes, offset, 4);
            const auto payload = integer(bytes, offset + 8, 8), size = integer(bytes, offset + 16, 8);
            if (attribute == 0) {
                if (size != 8) fault("PCM attribute requires an eight-byte address payload");
                const auto data = read<8>(payload);
                updated.pcm = integer(data, 0, 8);
                if (!updated.pcm) fault("PCM attribute contains a null guest address");
            } else if (attribute == 1) {
                if (size != updated.channels * 4) fault("gain payload does not match main-port channels");
                access(payload, static_cast<std::size_t>(size), Permission::Read);
                std::array<std::byte, 8> gains{};
                machine.Read(payload, std::span(gains).first(static_cast<std::size_t>(size)));
                for (std::size_t channel = 0; channel < updated.channels; ++channel) {
                    updated.gains[channel] = std::bit_cast<float>(static_cast<std::uint32_t>(integer(gains, channel * 4, 4)));
                    if (!std::isfinite(updated.gains[channel])) fault("gain payload contains a nonfinite value");
                }
            } else fault("unsupported spatial/routing attribute " + std::to_string(attribute));
        }
        value = updated;
    }
    void push(std::uint64_t id, std::uint32_t blocking) {
        auto& value = context(id);
        if (blocking > 1) fault("push blocking argument must be zero or one");
        const auto grain = value.parameters.grain;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        // Single machine-thread producer per stream. Capacity is checked before host allocation;
        // Submit performs the final atomic queue publication and can still refuse native failure.
        for (;;) {
            const auto snapshot = live(value);
            if (snapshot.queuedFrames <= snapshot.capacityFrames && snapshot.capacityFrames - snapshot.queuedFrames >= grain) break;
            if (!blocking || std::chrono::steady_clock::now() >= deadline) fault("native audio queue has no capacity for a complete grain");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const auto& [portId, entry] : ports) {
            (void)portId;
            if (entry.context == id && entry.pcm) access(entry.pcm, std::size_t(grain) * entry.channels * 4, Permission::Read);
        }
        std::vector<double> mixed(std::size_t(grain) * 2, 0);
        for (const auto& [portId, entry] : ports) {
            (void)portId;
            if (entry.context != id || !entry.pcm) continue;
            std::vector<std::byte> pcm(std::size_t(grain) * entry.channels * 4);
            machine.Read(entry.pcm, pcm);
            for (std::size_t frame = 0; frame < grain; ++frame) {
                for (std::size_t channel = 0; channel < 2; ++channel) {
                    const auto source = entry.channels == 1 ? 0 : channel;
                    const auto sample = std::bit_cast<float>(static_cast<std::uint32_t>(integer(pcm, (frame * entry.channels + source) * 4, 4)));
                    if (!std::isfinite(sample)) fault("PCM contains a nonfinite sample");
                    mixed[frame * 2 + channel] += double(sample) * entry.gains[source];
                }
            }
        }
        // Compatibility mixer: mono duplicates to stereo, unity gains sum, and output clips to [-1,1].
        std::vector<float> output(mixed.size());
        for (std::size_t index = 0; index < mixed.size(); ++index) output[index] = static_cast<float>(std::clamp(mixed[index], -1.0, 1.0));
        switch (value.output->SubmitInterleavedStereo(output)) {
        case NativeAudioOutput::SubmitStatus::Accepted: return;
        case NativeAudioOutput::SubmitStatus::WouldBlock: fault("native audio queue rejected complete-grain publication");
        case NativeAudioOutput::SubmitStatus::Stopped: fault("native audio output stopped before publication");
        case NativeAudioOutput::SubmitStatus::Fault: fault("native audio output failed before publication");
        }
        fault("unknown native audio submission status");
    }
    void invoke(Machine& guest, Service service) {
        const auto a = guest.Get(Register::Rdi), b = guest.Get(Register::Rsi), c = guest.Get(Register::Rdx);
        switch (service) {
        case Service::Initialize:
            if (initialized) {
                // Compatibility repeat policy uses a literal tolerated by the actual title;
                // this does not establish the SDK meaning of that status.
                guest.Set(Register::Rax, static_cast<std::uint64_t>(std::int64_t(std::bit_cast<std::int32_t>(0x80268004u))));
                return;
            }
            initialized = true; break;
        case Service::UserCreate: userCreate(static_cast<std::uint32_t>(a), b); break;
        case Service::UserDestroy: userDestroy(a); break;
        case Service::ResetParam: reset(a); break;
        case Service::QueryMemory: (void)parameters(a); write(b, scratchSize, 8); break;
        case Service::ContextCreate: create(a, b, c, guest.Get(Register::Rcx)); break;
        case Service::ContextDestroy: destroy(a); break;
        case Service::QueueLevel: queue(a, b, c); break;
        case Service::PortCreate: portCreate(a, b, c); break;
        case Service::PortDestroy: (void)port(a); ports.erase(a); break;
        case Service::PortState: portState(a, b); break;
        case Service::PortAttributes: attributes(a, b, static_cast<std::uint32_t>(c)); break;
        case Service::Push: push(a, static_cast<std::uint32_t>(b)); break;
        case Service::Advance: fault("context advance semantics are not established for the native profile");
        case Service::ContextAttributes: fault("context attribute semantics are not established");
        case Service::SpeakerInfo: fault("speaker information contract is not established");
        case Service::Latency: fault("3D latency contract is not established");
        case Service::MasteringInit: case Service::MasteringTerm: case Service::MasteringParam:
            fault("mastering contract is not established");
        }
        guest.Set(Register::Rax, 0);
    }
};

SceAudioOut2Imports::SceAudioOut2Imports(Machine& machine, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, gateBase)) {}
SceAudioOut2Imports::~SceAudioOut2Imports() = default;

std::optional<std::uint64_t> SceAudioOut2Imports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libSceAudioOut2") return std::nullopt;
    if (import.ModuleName != "libSceAudioOut" || import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        fault("import scope/version: " + identity(import));
    const auto found = impl->services.find(import.Nid);
    if (found == impl->services.end()) fault("unknown import: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                       import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto cached = impl->gates.find(key); cached != impl->gates.end()) return cached->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), service = found->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) fault("runtime has expired");
        context->invoke(guest, service);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
