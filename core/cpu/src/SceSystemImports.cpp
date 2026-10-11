#include <cpu/SceSystemImports.hpp>
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace Cpu {
namespace {

enum class Service {
    ParamGetInt, ParamGetString, HideSplashScreen, GetStatus, ReceiveEvent,
    GetHdrToneMapLuminance, InitializePlayerDialogParam, LaunchPlayerDialog
};

constexpr std::int64_t parameterError = std::bit_cast<std::int32_t>(0x80a10003u);
constexpr std::int64_t noEvent = std::bit_cast<std::int32_t>(0x80a10004u);
// SceSystemServiceStatus: event count, three flags and reserved bytes, padded to 136 bytes.
constexpr std::size_t statusSize = 136;
// SDR reference white for max full-frame and max tone-map luminance, 0 nits minimum.
constexpr float sdrReferenceWhiteNits = 100.0f;
constexpr std::uint64_t minimumNameCapacity = 65;
constexpr std::uint64_t maximumNameCapacity = 16 * 1024 * 1024;
constexpr std::array systemName{
    std::byte{'A'}, std::byte{'n'}, std::byte{'y'}, std::byte{'P'},
    std::byte{'S'}, std::byte{'5'}, std::byte{0}};

std::string identity(const SceImport& import) {
    return import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " id=" + std::to_string(import.LibraryId) + " module=" + import.ModuleName + ":" +
           std::to_string(import.ModuleMajor) + "." + std::to_string(import.ModuleMinor) +
           " id=" + std::to_string(import.ModuleId);
}

std::array<std::byte, 4> integer(std::uint32_t value) {
    std::array<std::byte, 4> bytes;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = std::byte((value >> (8 * index)) & 0xff);
    return bytes;
}

}

struct SceSystemImports::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint16_t,
                           std::uint16_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    const std::uint64_t base;
    std::size_t nextSlot = 0;
    const std::map<std::string, Service> services{
        {"fZo48un7LK4", Service::ParamGetInt}, {"SsC-m-S9JTA", Service::ParamGetString},
        {"Vo5V8KAwCmk", Service::HideSplashScreen}, {"rPo6tV8D9bM", Service::GetStatus},
        {"656LMQSrg6U", Service::ReceiveEvent}, {"mPpPxv5CZt4", Service::GetHdrToneMapLuminance},
        {"m5CYKX20wfg", Service::InitializePlayerDialogParam}, {"uaieF+glFPs", Service::LaunchPlayerDialog}};
    std::map<Key, std::uint64_t> gates;

    Impl(Machine& guest, std::uint64_t gateBase) : machine(guest), base(gateBase) {
        if (!base || (base & 4095) || base >= 0x7ffffffff000)
            throw std::invalid_argument("SCE system import gates require a nonzero aligned low canonical guest page");
        std::array<std::byte, 4096> bytes;
        bytes.fill(std::byte{0xcc});
        machine.Map(base, bytes.size(), Permission::Read | Permission::Write);
        machine.Write(base, bytes);
        machine.Protect(base, bytes.size(), Permission::Read | Permission::Execute);
    }

    bool writable(std::uint64_t address, std::size_t size) const {
        if (!address || size > std::numeric_limits<std::uint64_t>::max() - address) return false;
        try { machine.CheckAccess(address, size, Permission::Write); }
        catch (const std::runtime_error&) { return false; }
        return true;
    }

    // Virtual console profile: English (US), DD/MM/YYYY, 24-hour clock, UTC, no summertime, parental
    // control off and Cross as the enter button. Unknown IDs read as 0, as on the upstream HLE library.
    static std::uint32_t profileValue(std::uint32_t id) {
        switch (id) {
        case 1: return 1;     // LANG: English (US)
        case 2: return 1;     // DATE_FORMAT: DD/MM/YYYY
        case 3: return 1;     // TIME_FORMAT: 24-hour
        case 1000: return 1;  // ENTER_BUTTON_ASSIGN: Cross
        default: return 0;    // TIME_ZONE, SUMMERTIME, GAME_PARENTAL_LEVEL and unknown IDs
        }
    }

    std::int64_t paramGetInt(std::uint64_t param, std::uint64_t destination) {
        const auto bytes = integer(profileValue(static_cast<std::uint32_t>(param)));
        if (!writable(destination, bytes.size())) return parameterError;
        machine.Write(destination, bytes);
        return 0;
    }

    std::int64_t getStatus(std::uint64_t destination) {
        if (!writable(destination, statusSize)) return parameterError;
        const std::array<std::byte, statusSize> status{};
        machine.Write(destination, status);
        return 0;
    }

    std::int64_t hdrToneMapLuminance(std::uint64_t destination) {
        constexpr std::size_t size = 12;
        if (!writable(destination, size)) return parameterError;
        const auto white = integer(std::bit_cast<std::uint32_t>(sdrReferenceWhiteNits));
        std::array<std::byte, size> bytes{};
        std::copy(white.begin(), white.end(), bytes.begin());
        std::copy(white.begin(), white.end(), bytes.begin() + 4);
        machine.Write(destination, bytes);
        return 0;
    }

    std::int64_t paramGetString(std::uint64_t param, std::uint64_t destination, std::uint64_t capacity) {
        if (!destination || !capacity) return parameterError;
        const auto id = static_cast<std::uint32_t>(param);
        if (id != 6)
            throw std::runtime_error("Unsupported SCE system service string parameter: " + std::to_string(id));
        if (capacity < minimumNameCapacity || capacity > maximumNameCapacity)
            throw std::runtime_error("Unsupported SCE system service name buffer capacity: " + std::to_string(capacity));
        if (!writable(destination, static_cast<std::size_t>(capacity))) return parameterError;
        machine.Write(destination, systemName);
        return 0;
    }

    void invoke(Machine& guest, Service service) {
        const auto first = guest.Get(Register::Rdi);
        std::int64_t result = 0;
        switch (service) {
        case Service::ParamGetInt: result = paramGetInt(first, guest.Get(Register::Rsi)); break;
        case Service::ParamGetString:
            result = paramGetString(first, guest.Get(Register::Rsi), guest.Get(Register::Rdx)); break;
        case Service::HideSplashScreen: break;
        case Service::GetStatus: result = getStatus(first); break;
        // No system events are ever queued; the caller's buffer is left untouched.
        case Service::ReceiveEvent: result = first ? noEvent : parameterError; break;
        case Service::GetHdrToneMapLuminance: result = hdrToneMapLuminance(first); break;
        // The player dialog parameter is opaque here and no dialog UI exists; both accept any non-null parameter.
        case Service::InitializePlayerDialogParam:
        case Service::LaunchPlayerDialog: result = first ? 0 : parameterError; break;
        }
        guest.Set(Register::Rax, static_cast<std::uint64_t>(result));
    }
};

SceSystemImports::SceSystemImports(Machine& machine, std::uint64_t gateBase) :
    impl(std::make_shared<Impl>(machine, gateBase)) {}
SceSystemImports::~SceSystemImports() = default;

std::optional<std::uint64_t> SceSystemImports::Resolve(const SceImport& import) {
    if (import.LibraryName != "libSceSystemService" && import.ModuleName != "libSceSystemService") return std::nullopt;
    if (import.LibraryName != "libSceSystemService" || import.ModuleName != "libSceSystemService" ||
        import.LibraryVersion != 1 || import.ModuleMajor != 1 || import.ModuleMinor != 1)
        throw std::runtime_error("Unsupported SCE system import scope/version: " + identity(import));
    const auto service = impl->services.find(import.Nid);
    if (service == impl->services.end()) throw std::runtime_error("Unsupported SCE system import service: " + identity(import));
    const Impl::Key key{import.Nid, import.LibraryName, import.LibraryId, import.ModuleName, import.ModuleId,
                        import.LibraryVersion, import.ModuleMajor, import.ModuleMinor};
    if (const auto found = impl->gates.find(key); found != impl->gates.end()) return found->second;
    if (impl->nextSlot == 256) throw std::runtime_error("SCE system import gate page is exhausted");
    const auto gate = impl->base + impl->nextSlot * 16;
    const std::array ret{std::byte{0xc3}};
    impl->machine.Write(gate, ret);
    impl->machine.AddHostCall(gate, [state = std::weak_ptr<Impl>(impl), operation = service->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE system import runtime has expired");
        context->invoke(guest, operation);
    });
    impl->gates.emplace(key, gate);
    ++impl->nextSlot;
    return gate;
}

}
