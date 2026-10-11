#include <cpu/SceUserImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace Cpu {
namespace {

enum class Service { Initialize, InitialUser, LoginUsers, UserName };

constexpr std::uint32_t localUser = 0x10000000;
constexpr std::string_view localName = "Player";
constexpr std::uint64_t maxNameCapacity = 16 * 1024 * 1024;
constexpr std::size_t minimumNameCapacity = 17;
constexpr std::int64_t notInitialized = std::bit_cast<std::int32_t>(0x80960002u);
constexpr std::int64_t alreadyInitialized = std::bit_cast<std::int32_t>(0x80960003u);
constexpr std::int64_t invalidArgument = std::bit_cast<std::int32_t>(0x80960005u);
constexpr std::int64_t bufferTooShort = std::bit_cast<std::int32_t>(0x8096000au);

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

std::uint32_t readInt(const std::array<std::byte, 4>& bytes) {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        result |= std::uint32_t(std::to_integer<unsigned char>(bytes[index])) << (8 * index);
    return result;
}

template<std::size_t Count>
std::array<std::byte, Count * 4> integers(const std::array<std::uint32_t, Count>& values) {
    std::array<std::byte, Count * 4> bytes{};
    for (std::size_t index = 0; index < Count; ++index)
        for (std::size_t byte = 0; byte < 4; ++byte)
            bytes[index * 4 + byte] = std::byte((values[index] >> (8 * byte)) & 0xff);
    return bytes;
}

}

struct SceUserImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    SceHostTrampolines trampolines;
    bool initialized = false;
    const std::map<std::string, Service> services{
        {"j3YMu1MVNNo", Service::Initialize}, {"CdWp0oHWGr0", Service::InitialUser},
        {"fPhymKNvK-A", Service::LoginUsers}, {"1xxcMiGu2fo", Service::UserName}};
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint64_t base)
        : machine(guest), trampolines(guest, base, SceHostTrampolines::DefaultCapacity, "SCE user import") {}

    bool accessible(std::uint64_t address, std::size_t size, Permission permission) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address) return false;
        try { machine.CheckAccess(address, size, permission); }
        catch (const std::runtime_error&) { return false; }
        return true;
    }

    std::int64_t initialize(std::uint64_t params) {
        if (params) {
            std::array<std::byte, 4> bytes;
            if (!accessible(params, bytes.size(), Permission::Read)) return invalidArgument;
            machine.Read(params, bytes);
            const auto priority = readInt(bytes);
            // Compatibility policy: these synchronous services accept the declared FIFO range;
            // this does not configure a host service thread or claim full SDK priority semantics.
            if (priority < 0x100 || priority > 0x2ff)
                throw std::runtime_error("Unsupported SCE user service initialization priority: " + std::to_string(priority));
        }
        if (initialized) return alreadyInitialized;
        initialized = true;
        return 0;
    }

    std::int64_t userName(std::uint64_t user, std::uint64_t destination, std::uint64_t capacity) {
        if (static_cast<std::uint32_t>(user) != localUser || !destination) return invalidArgument;
        // Preserve the repository's 17-byte minimum as a compatibility policy.
        if (capacity < minimumNameCapacity) return bufferTooShort;
        if (capacity > maxNameCapacity)
            throw std::runtime_error("Unsupported SCE user service name buffer exceeds 16 MiB");
        if (!accessible(destination, static_cast<std::size_t>(capacity), Permission::Write)) return invalidArgument;
        std::vector<std::byte> bytes(static_cast<std::size_t>(capacity), std::byte{0});
        for (std::size_t index = 0; index < localName.size(); ++index)
            bytes[index] = std::byte(static_cast<unsigned char>(localName[index]));
        machine.Write(destination, bytes);
        return 0;
    }

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        std::int64_t result;
        // The initialization prerequisite is emulator policy, not a claim about every SDK's
        // error precedence. All output spans are preflighted before any guest write.
        if (service == Service::Initialize) result = initialize(first);
        else if (!initialized) result = notInitialized;
        else if (service == Service::UserName)
            result = userName(first, guest.Get(Register::Rsi), guest.Get(Register::Rdx));
        else if (service == Service::InitialUser) {
            const auto bytes = integers(std::array{localUser});
            if (!accessible(first, bytes.size(), Permission::Write)) result = invalidArgument;
            else { machine.Write(first, bytes); result = 0; }
        } else {
            const auto bytes = integers(std::array{localUser, 0xffffffffu, 0xffffffffu, 0xffffffffu});
            if (!accessible(first, bytes.size(), Permission::Write)) result = invalidArgument;
            else { machine.Write(first, bytes); result = 0; }
        }
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceUserImports::SceUserImports(Machine& machine, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, gateBase)) {}
SceUserImports::~SceUserImports() = default;

std::optional<std::uint64_t> SceUserImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libSceUserService" && import.ModuleName != "libSceUserService") return std::nullopt;
    if (import.LibraryName != "libSceUserService" || import.ModuleName != "libSceUserService" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE user import scope/version: " + identity(import));
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) throw std::runtime_error("Unsupported SCE user import service: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), operation = service->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE user import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    return gate;
}

}
