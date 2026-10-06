#include <cpu/SceSystemImports.hpp>
#include <cpu/SceElf.hpp>
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

const char* unsupportedName(Service service) {
    switch (service) {
    case Service::GetStatus: return "sceSystemServiceGetStatus";
    case Service::ReceiveEvent: return "sceSystemServiceReceiveEvent";
    case Service::GetHdrToneMapLuminance: return "sceSystemServiceGetHdrToneMapLuminance";
    case Service::InitializePlayerDialogParam: return "sceSystemServiceInitializePlayerDialogParam";
    case Service::LaunchPlayerDialog: return "sceSystemServiceLaunchPlayerDialog";
    default: return nullptr;
    }
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

    std::int64_t paramGetInt(std::uint64_t param, std::uint64_t destination) {
        const auto id = static_cast<std::uint32_t>(param);
        // Virtual console policy for the title's observed language/time-zone/DST queries.
        std::uint32_t value;
        switch (id) {
        case 1: value = 1; break; // English (US).
        case 4: case 5: value = 0; break; // UTC, no summertime offset.
        default:
            throw std::runtime_error("Unsupported SCE system service integer parameter: " + std::to_string(id));
        }
        const auto bytes = integer(value);
        if (!writable(destination, bytes.size())) return parameterError;
        machine.Write(destination, bytes);
        return 0;
    }

    std::int64_t paramGetString(std::uint64_t param, std::uint64_t destination, std::uint64_t capacity) {
        if (!destination || !capacity) return parameterError;
        const auto id = static_cast<std::uint32_t>(param);
        if (id != 6)
            throw std::runtime_error("Unsupported SCE system service string parameter: " + std::to_string(id));
        // The title/repository's 65-byte buffer contract and the 16 MiB limit are compatibility policy.
        if (capacity < minimumNameCapacity || capacity > maximumNameCapacity)
            throw std::runtime_error("Unsupported SCE system service name buffer capacity: " + std::to_string(capacity));
        if (!writable(destination, static_cast<std::size_t>(capacity))) return parameterError;
        machine.Write(destination, systemName);
        return 0;
    }

    void invoke(Machine& guest, Service service) {
        // These layouts and host UI/HDR/event semantics are not established for this title yet.
        if (const auto name = unsupportedName(service))
            throw std::runtime_error(std::string("Unsupported SCE system service invocation: ") + name);
        std::int64_t result;
        if (service == Service::ParamGetInt)
            result = paramGetInt(guest.Get(Register::Rdi), guest.Get(Register::Rsi));
        else if (service == Service::ParamGetString)
            result = paramGetString(guest.Get(Register::Rdi), guest.Get(Register::Rsi), guest.Get(Register::Rdx));
        else result = 0; // HideSplashScreen is idempotent: this virtual engine has no OS splash.
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
