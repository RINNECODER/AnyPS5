#include <cpu/SceUserImports.hpp>
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace Cpu {
namespace {

enum class Service {
    Initialize, Initialize2, Terminate, InitialUser, ForegroundUser, LoginUsers, RegisteredUsers, UserName,
    GetEvent, UserValue
};

// Fixed per-user output: `size` bytes at `offset` from the caller's pointer, holding `value` as a LE integer
// in its first four bytes and zero after that. A nonzero `offset` is the caller's u64 this_size field: it is
// kept, and only the part of the output the caller's structure declares is written.
struct Output { std::size_t offset = 0; std::size_t size = 0; std::uint32_t value = 0; };
struct Entry { Service service; Output output{}; };

constexpr std::uint32_t localUser = 0x10000000;
constexpr std::uint32_t invalidUser = 0xffffffff;
constexpr std::uint32_t loginEvent = 0;
constexpr std::string_view localName = "Player";
constexpr std::uint64_t maxNameCapacity = 16 * 1024 * 1024;
constexpr std::size_t minimumNameCapacity = 17;
constexpr std::int64_t notInitialized = std::bit_cast<std::int32_t>(0x80960002u);
constexpr std::int64_t alreadyInitialized = std::bit_cast<std::int32_t>(0x80960003u);
constexpr std::int64_t invalidArgument = std::bit_cast<std::int32_t>(0x80960005u);
constexpr std::int64_t noEvent = std::bit_cast<std::int32_t>(0x80960007u);
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
    const std::uint64_t base;
    bool initialized = false;
    bool loginReported = false;
    std::size_t nextSlot = 0;
    const std::map<std::string, Entry> services{
        {"j3YMu1MVNNo", {Service::Initialize}}, {"az-0R6eviZ0", {Service::Initialize2}},
        {"bwFjS+bX9mA", {Service::Terminate}}, {"CdWp0oHWGr0", {Service::InitialUser}},
        {"eNb53LQJmIM", {Service::ForegroundUser}}, {"fPhymKNvK-A", {Service::LoginUsers}},
        {"5EiQCnL2G1Y", {Service::RegisteredUsers}}, {"1xxcMiGu2fo", {Service::UserName}},
        {"yH17Q6NWtVg", {Service::GetEvent}},
        {"lUoqwTQu4Go", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetUserColor
        {"qbwy0Ub8b3M", {Service::UserValue, {0, 4, 1}}},   // sceUserServiceGetUserNumber
        {"woNpu+45RLk", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetAgeLevel
        {"6dfDreosXGY", {Service::UserValue, {0, 8, 0}}},   // sceUserServiceGetNpAccountId: no account
        {"-sD02mFDBh4", {Service::UserValue, {8, 32, 0}}},  // sceUserServiceGetGamePresets: none set
        {"D-CzAxQL0XI", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetPlatformPrivacyWs1
        {"rnEhHqG-4xo", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetAccessibilityChatTranscription
        {"ZKJtxdgvzwg", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetAccessibilityPressAndHoldDelay
        {"-3Y5GO+-i78", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetAccessibilityTriggerEffect
        {"qWYHOFwqCxY", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetAccessibilityVibration
        {"hD-H81EN9Vg", {Service::UserValue, {0, 4, 0}}},   // sceUserServiceGetAccessibilityZoomEnabled
        {"O6IW1-Dwm-w", {Service::UserValue, {0, 4, 0}}}};  // sceUserServiceGetAccessibilityZoomFollowFocus
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint64_t gateBase) : machine(guest), base(gateBase) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE user import gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

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

    std::int64_t write(std::uint64_t destination, std::span<const std::byte> bytes) {
        if (!accessible(destination, bytes.size(), Permission::Write)) return invalidArgument;
        machine.Write(destination, bytes);
        return 0;
    }

    std::int64_t userValue(std::uint64_t user, std::uint64_t destination, const Output& output) {
        if (static_cast<std::uint32_t>(user) != localUser) return invalidArgument;
        std::size_t size = output.size;
        if (output.offset) {
            std::array<std::byte, 8> declared;
            if (!accessible(destination, declared.size(), Permission::Read)) return invalidArgument;
            machine.Read(destination, declared);
            std::uint64_t thisSize = 0;
            for (std::size_t index = 0; index < declared.size(); ++index)
                thisSize |= std::uint64_t(std::to_integer<unsigned char>(declared[index])) << (8 * index);
            if (thisSize < output.offset + 4) return invalidArgument;
            size = static_cast<std::size_t>(std::min<std::uint64_t>(thisSize - output.offset, output.size));
        }
        if (!accessible(destination, output.offset + size, Permission::Write)) return invalidArgument;
        std::vector<std::byte> bytes(size, std::byte{0});
        const auto value = integers(std::array{output.value});
        std::copy(value.begin(), value.end(), bytes.begin());
        machine.Write(destination + output.offset, bytes);
        return 0;
    }

    std::int64_t event(std::uint64_t destination) {
        // SceUserServiceEvent {u32 type; i32 user}: the local user's login once per session, then no events.
        if (!accessible(destination, 8, Permission::Write)) return invalidArgument;
        if (loginReported) return noEvent;
        machine.Write(destination, integers(std::array{loginEvent, localUser}));
        loginReported = true;
        return 0;
    }

    void invoke(Machine& guest, const Entry& entry) {
        const auto first = guest.Get(Register::Rdi);
        std::int64_t result = 0;
        // The initialization prerequisite is emulator policy, not a claim about every SDK's
        // error precedence. All output spans are preflighted before any guest write.
        if (entry.service == Service::Initialize) result = initialize(first);
        // Initialize2(priority, affinity) has no option block; these services run synchronously.
        else if (entry.service == Service::Initialize2) result = initialize(0);
        else if (!initialized) result = notInitialized;
        else switch (entry.service) {
        case Service::Terminate: initialized = false; loginReported = false; break;
        case Service::UserName: result = userName(first, guest.Get(Register::Rsi), guest.Get(Register::Rdx)); break;
        case Service::InitialUser:
        case Service::ForegroundUser: result = write(first, integers(std::array{localUser})); break;
        case Service::LoginUsers:
            result = write(first, integers(std::array{localUser, invalidUser, invalidUser, invalidUser}));
            break;
        case Service::RegisteredUsers: {
            std::array<std::uint32_t, 16> users;
            users.fill(invalidUser);
            users[0] = localUser;
            result = write(first, integers(users));
            break;
        }
        case Service::GetEvent: result = event(first); break;
        case Service::UserValue: result = userValue(first, guest.Get(Register::Rsi), entry.output); break;
        case Service::Initialize:
        case Service::Initialize2: break;
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
    if (impl->nextSlot == 256) throw std::runtime_error("SCE user import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation = service->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE user import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
