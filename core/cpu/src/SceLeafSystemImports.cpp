#include <cpu/SceLeafSystemImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Cpu {
namespace {

// Status codes below are the constants of the upstream core/libs/prx bodies each handler
// stands in for. tools drift test UpstreamPrxBridgeDriftTest.py pins them to upstream.
constexpr std::int32_t Error(std::uint32_t code) { return static_cast<std::int32_t>(code); }

constexpr std::int32_t AppContentParameter = Error(0x80D90002);
constexpr std::int32_t AppContentBusy = Error(0x80D90003);
constexpr std::int32_t AppContentNotMounted = Error(0x80D90004);
constexpr std::int32_t AppContentNotFound = Error(0x80D90005);
constexpr std::int32_t AppContentSkuFlagFull = 3;
constexpr std::size_t AppContentBootParamSize = 40;
constexpr std::size_t AppContentMountPointSize = 16;
constexpr std::string_view TemporaryMountPoint = "/temp0";

constexpr std::int32_t ImeDialogNotOpened = Error(0x80bc0002);
constexpr std::uint32_t ImeDialogEndStatusOk = 0;
constexpr std::uint32_t ImeDialogEndStatusAborted = 2;
constexpr std::size_t ImeDialogResultSize = 16;
constexpr std::uint32_t ImeDialogPanelWidth = 1920;
constexpr std::uint32_t ImeDialogPanelHeight = 1080;

constexpr std::int32_t DialogNone = 0;
constexpr std::int32_t DialogInitialized = 1;
constexpr std::int32_t DialogRunning = 2;
constexpr std::int32_t DialogFinished = 3;
constexpr std::int32_t SaveDataDialogNotInitialized = Error(0x80B80003);
constexpr std::int32_t SaveDataDialogAlreadyInitialized = Error(0x80B80004);
constexpr std::int32_t SaveDataDialogInvalidState = Error(0x80B80006);
constexpr std::int32_t SaveDataDialogArgNull = Error(0x80B8000D);
constexpr std::int32_t SaveDataDialogModeProgressBar = 5;
constexpr std::uint64_t SaveDataDialogModeOffset = 52;

constexpr std::int32_t SaveDataNotInitialized = -2137063423;

constexpr std::int32_t NpInvalidArgument = Error(0x80550003);
constexpr std::int32_t NpSignedOut = Error(0x80550006);

// Opaque stand-ins for the objects upstream allocates with new; the guest only passes them
// back to the property setters, which never dereference them.
constexpr std::uint64_t UdsTokenBase = 0x7ffd90f00000;

struct Scope {
    std::string_view Module, Library, File;
    std::uint16_t Major, Minor;
};
constexpr Scope AppContentUtil{"libSceAppContentUtil", "libSceAppContent", "libSceAppContent.prx", 1, 1};
constexpr Scope ImeDialog{"libSceImeDialog", "libSceImeDialog", "libSceImeDialog.prx", 1, 1};
constexpr Scope SaveDataDialog{"libSceSaveDataDialog", "libSceSaveDataDialog.native", "libSceSaveDataDialog.native.prx", 1, 1};
constexpr Scope SaveData{"libSceSaveData_native", "libSceSaveData_native", "libSceSaveData.native.prx", 1, 1};
constexpr Scope SystemGesture{"libSceSystemGesture", "libSceSystemGesture", "libSceSystemGesture.prx", 1, 1};
constexpr Scope Ime{"libSceIme", "libSceIme", "libSceIme.prx", 1, 1};
constexpr Scope Uds{"libSceNpUniversalDataSystem", "libSceNpUniversalDataSystem", "libSceNpUniversalDataSystem.prx", 1, 1};
constexpr Scope Acm{"libSceAcm", "libSceAcm", "libSceAcm.prx", 1, 1};
constexpr Scope NpAuth{"libSceNpAuth", "libSceNpAuth", "libSceNpAuth.prx", 1, 1};
constexpr std::array<const Scope*, 9> Scopes{&AppContentUtil, &ImeDialog, &SaveDataDialog, &SaveData, &SystemGesture,
                                             &Ime, &Uds, &Acm, &NpAuth};

std::uint64_t argument(Machine& guest, unsigned index) {
    constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
    return guest.Get(registers.at(index));
}

[[noreturn]] void Fault(std::string_view function, const std::string& what) {
    throw std::runtime_error(std::string(function) + ": " + what);
}

void put(Machine& guest, std::uint64_t address, std::span<const std::byte> bytes, std::string_view function) {
    if (bytes.empty()) return;
    try { guest.CheckAccess(address, bytes.size(), Permission::Write); }
    catch (const std::exception& error) { Fault(function, "unwritable guest buffer: " + std::string(error.what())); }
    guest.Write(address, bytes);
}

template <typename Value>
void putValue(Machine& guest, std::uint64_t address, Value value, std::string_view function) {
    std::array<std::byte, sizeof(Value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>(static_cast<std::uint64_t>(value) >> (index * 8));
    put(guest, address, bytes, function);
}

void putZero(Machine& guest, std::uint64_t address, std::size_t size, std::string_view function) {
    const std::vector<std::byte> zero(size);
    put(guest, address, zero, function);
}

std::vector<std::byte> fetch(Machine& guest, std::uint64_t address, std::size_t size, std::string_view function) {
    std::vector<std::byte> bytes(size);
    try { guest.CheckAccess(address, size, Permission::Read); }
    catch (const std::exception& error) { Fault(function, "unreadable guest buffer: " + std::string(error.what())); }
    guest.Read(address, bytes);
    return bytes;
}

}

struct SceLeafSystemImports::Impl {
    using Handler = std::function<std::int32_t(Impl&, Machine&)>;
    struct Function { const Scope* Where; std::string_view Name; Handler Run; };
    SceHostTrampolines trampolines;
    std::map<std::string, Function> functions;
    std::map<std::string, std::uint64_t> gates;

    bool temporaryMounted = false;
    std::int32_t imeDialogStatus = 0;  // 0 none, 1 running, 2 finished
    std::uint32_t imeDialogPolls = 0;
    std::uint32_t imeDialogEnd = ImeDialogEndStatusOk;
    std::int32_t saveDataDialogStatus = DialogNone;
    int saveDataInitializations = 0;
    std::int32_t transactionCounter = 1;
    std::uint64_t nextToken = UdsTokenBase;

    std::uint64_t token() { return nextToken += 0x10; }

    Impl(Machine& machine, std::uint64_t base) : trampolines(machine, base, SceHostTrampolines::DefaultCapacity, "SCE leaf system") {
        const auto ok = [](Impl&, Machine&) -> std::int32_t { return 0; };
        const std::initializer_list<Function> table{
            // libSceAppContent: add-on content is never entitled; temporary data mounts at /temp0.
            {&AppContentUtil, "sceAppContentAddcontMount", [](Impl&, Machine& guest) -> std::int32_t {
                return argument(guest, 1) && argument(guest, 2) ? AppContentNotFound : AppContentParameter; }},
            {&AppContentUtil, "sceAppContentInitialize", [](Impl&, Machine& guest) -> std::int32_t {
                if (!argument(guest, 1)) return AppContentParameter;
                putZero(guest, argument(guest, 1), AppContentBootParamSize, "sceAppContentInitialize");
                return 0; }},
            {&AppContentUtil, "sceAppContentTemporaryDataMount2", [](Impl& self, Machine& guest) -> std::int32_t {
                const auto option = static_cast<std::uint32_t>(argument(guest, 0));
                if (!argument(guest, 1) || option > 1) return AppContentParameter;
                if (self.temporaryMounted) return AppContentBusy;
                std::array<std::byte, AppContentMountPointSize> point{};
                std::memcpy(point.data(), TemporaryMountPoint.data(), TemporaryMountPoint.size());
                put(guest, argument(guest, 1), point, "sceAppContentTemporaryDataMount2");
                self.temporaryMounted = true;
                return 0; }},
            {&AppContentUtil, "sceAppContentTemporaryDataUnmount", [](Impl& self, Machine& guest) -> std::int32_t {
                if (!argument(guest, 0)) return AppContentParameter;
                const auto point = fetch(guest, argument(guest, 0), AppContentMountPointSize, "sceAppContentTemporaryDataUnmount");
                const bool temporary = std::strncmp(reinterpret_cast<const char*>(point.data()),
                    std::string(TemporaryMountPoint).c_str(), AppContentMountPointSize) == 0;
                if (!self.temporaryMounted || !temporary) return AppContentNotMounted;
                self.temporaryMounted = false;
                return 0; }},
            // Param ids 1 to 4 are the title's user-defined params, which no param.json feeds yet.
            {&AppContentUtil, "sceAppContentAppParamGetInt", [](Impl&, Machine& guest) -> std::int32_t {
                if (!argument(guest, 1)) return AppContentParameter;
                const auto id = static_cast<std::uint32_t>(argument(guest, 0));
                if (id > 4) return AppContentParameter;
                putValue(guest, argument(guest, 1), id == 0 ? AppContentSkuFlagFull : 0, "sceAppContentAppParamGetInt");
                return 0; }},

            // libSceImeDialog: no panel exists, so a dialog finishes on the second status poll with
            // the text unchanged and an OK end status, or aborted when the title aborts it.
            {&ImeDialog, "sceImeDialogInit", [](Impl& self, Machine&) -> std::int32_t {
                self.imeDialogPolls = 0;
                self.imeDialogEnd = ImeDialogEndStatusOk;
                self.imeDialogStatus = 1;
                return 0; }},
            {&ImeDialog, "sceImeDialogTerm", [](Impl& self, Machine&) -> std::int32_t {
                if (self.imeDialogStatus == 0) return ImeDialogNotOpened;
                self.imeDialogStatus = 0;
                return 0; }},
            {&ImeDialog, "sceImeDialogAbort", [](Impl& self, Machine&) -> std::int32_t {
                if (self.imeDialogStatus != 1) return ImeDialogNotOpened;
                self.imeDialogEnd = ImeDialogEndStatusAborted;
                self.imeDialogStatus = 2;
                return 0; }},
            {&ImeDialog, "sceImeDialogGetStatus", [](Impl& self, Machine&) -> std::int32_t {
                if (self.imeDialogStatus == 1 && ++self.imeDialogPolls > 1) self.imeDialogStatus = 2;
                return self.imeDialogStatus; }},
            {&ImeDialog, "sceImeDialogGetResult", [](Impl& self, Machine& guest) -> std::int32_t {
                if (!argument(guest, 0) || self.imeDialogStatus != 2) return ImeDialogNotOpened;
                putZero(guest, argument(guest, 0), ImeDialogResultSize, "sceImeDialogGetResult");
                putValue(guest, argument(guest, 0), self.imeDialogEnd, "sceImeDialogGetResult");
                return 0; }},
            {&ImeDialog, "sceImeDialogGetPanelSizeExtended", [](Impl&, Machine& guest) -> std::int32_t {
                if (argument(guest, 2)) putValue(guest, argument(guest, 2), ImeDialogPanelWidth, "sceImeDialogGetPanelSizeExtended");
                if (argument(guest, 3)) putValue(guest, argument(guest, 3), ImeDialogPanelHeight, "sceImeDialogGetPanelSizeExtended");
                return 0; }},

            // libSceSaveDataDialog: the dialog finishes at once (progress bars keep running).
            {&SaveDataDialog, "sceSaveDataDialogInitialize", [](Impl& self, Machine&) -> std::int32_t {
                if (self.saveDataDialogStatus != DialogNone) return SaveDataDialogAlreadyInitialized;
                self.saveDataDialogStatus = DialogInitialized;
                return 0; }},
            {&SaveDataDialog, "sceSaveDataDialogOpen", [](Impl& self, Machine& guest) -> std::int32_t {
                if (self.saveDataDialogStatus != DialogInitialized && self.saveDataDialogStatus != DialogFinished)
                    return SaveDataDialogInvalidState;
                if (!argument(guest, 0)) return SaveDataDialogArgNull;
                const auto mode = fetch(guest, argument(guest, 0) + SaveDataDialogModeOffset, sizeof(std::int32_t), "sceSaveDataDialogOpen");
                std::int32_t value = 0;
                std::memcpy(&value, mode.data(), sizeof(value));
                self.saveDataDialogStatus = value == SaveDataDialogModeProgressBar ? DialogRunning : DialogFinished;
                return 0; }},
            {&SaveDataDialog, "sceSaveDataDialogTerminate", [](Impl& self, Machine&) -> std::int32_t {
                self.saveDataDialogStatus = DialogNone;
                return 0; }},
            {&SaveDataDialog, "sceSaveDataDialogUpdateStatus", [](Impl& self, Machine&) -> std::int32_t {
                return self.saveDataDialogStatus; }},

            // libSceSaveData: initialisation, commit and prepare only. Mount3/Umount2 need the
            // save-directory aliasing of GuestFiles and are not served.
            {&SaveData, "sceSaveDataInitialize3", [](Impl& self, Machine&) -> std::int32_t {
                ++self.saveDataInitializations;
                return 0; }},
            {&SaveData, "sceSaveDataTerminate", [](Impl& self, Machine&) -> std::int32_t {
                if (self.saveDataInitializations == 0) return SaveDataNotInitialized;
                --self.saveDataInitializations;
                return 0; }},
            {&SaveData, "sceSaveDataCreateTransactionResource", [](Impl& self, Machine&) -> std::int32_t {
                return self.transactionCounter++; }},
            {&SaveData, "sceSaveDataCommit", ok},
            {&SaveData, "sceSaveDataPrepare", ok},

            // Upstream throws NotImplemented for these; with no touch input there is nothing to do.
            {&SystemGesture, "sceSystemGestureAppendTouchRecognizer", ok},
            {&SystemGesture, "sceSystemGestureRemoveTouchRecognizer", ok},
            {&SystemGesture, "sceSystemGestureUpdateAllTouchRecognizer", ok},

            // The event handler is never invoked: no keyboard events exist.
            {&Ime, "sceImeUpdate", [](Impl&, Machine& guest) -> std::int32_t {
                if (!argument(guest, 0)) Fault("sceImeUpdate", "invalid argument");
                return 0; }},

            // libSceNpUniversalDataSystem objects the upstream bodies allocate.
            {&Uds, "sceNpUniversalDataSystemCreateEvent", [](Impl& self, Machine& guest) -> std::int32_t {
                if (!argument(guest, 0) || !argument(guest, 2)) Fault("sceNpUniversalDataSystemCreateEvent", "invalid argument");
                putValue(guest, argument(guest, 2), self.token(), "sceNpUniversalDataSystemCreateEvent");
                if (argument(guest, 3))
                    putValue(guest, argument(guest, 3), argument(guest, 1) ? argument(guest, 1) : self.token(), "sceNpUniversalDataSystemCreateEvent");
                return 0; }},
            {&Uds, "sceNpUniversalDataSystemDestroyEvent", ok},
            {&Uds, "sceNpUniversalDataSystemEventPropertyObjectSetArray", [](Impl& self, Machine& guest) -> std::int32_t {
                if (!argument(guest, 0) || !argument(guest, 1)) Fault("sceNpUniversalDataSystemEventPropertyObjectSetArray", "invalid argument");
                if (argument(guest, 3))
                    putValue(guest, argument(guest, 3), argument(guest, 2) ? argument(guest, 2) : self.token(), "sceNpUniversalDataSystemEventPropertyObjectSetArray");
                return 0; }},
            // The float value arrives in xmm0, which the property setter ignores anyway.
            {&Uds, "sceNpUniversalDataSystemEventPropertyObjectSetFloat32", [](Impl&, Machine& guest) -> std::int32_t {
                if (!argument(guest, 0) || !argument(guest, 1)) Fault("sceNpUniversalDataSystemEventPropertyObjectSetFloat32", "invalid argument");
                return 0; }},

            // Upstream throws NotImplemented for the panner command; batches are no-ops.
            {&Acm, "sceAcm_Panner", ok},

            // Upstream throws NotImplemented; the user stays signed out like the other auth exports.
            {&NpAuth, "sceNpAuthGetIdTokenV3", [](Impl&, Machine& guest) -> std::int32_t {
                return !argument(guest, 1) || !argument(guest, 2) ? NpInvalidArgument : NpSignedOut; }},
        };
        for (const auto& function : table)
            if (!functions.emplace(Nid::ComputeNid(std::string(function.Name), ""), function).second)
                throw std::logic_error("Duplicate leaf system NID for " + std::string(function.Name));
    }
};

SceLeafSystemImports::SceLeafSystemImports(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceLeafSystemImports::~SceLeafSystemImports() = default;

std::vector<SceUpstreamHost> SceLeafSystemImports::Hosts() {
    std::vector<SceUpstreamHost> hosts;
    for (const auto* scope : Scopes)
        hosts.push_back({std::string(scope->File), std::string(scope->Module), std::string(scope->Library), scope->Major, scope->Minor});
    return hosts;
}

std::optional<std::uint64_t> SceLeafSystemImports::Resolve(const SceImport& import) {
    const auto found = impl->functions.find(import.Nid);
    if (found == impl->functions.end()) return std::nullopt;
    const auto& scope = *found->second.Where;
    if (import.ModuleName != scope.Module || import.LibraryName != scope.Library || import.LibraryVersion != 1 ||
        import.ModuleMajor != scope.Major || import.ModuleMinor != scope.Minor)
        return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), run = found->second.Run](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE leaf system import runtime has expired");
        // Results are signed 32-bit SCE status codes, sign-extended into RAX.
        guest.Set(Register::Rax, static_cast<std::uint64_t>(static_cast<std::int64_t>(run(*context, guest))));
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
