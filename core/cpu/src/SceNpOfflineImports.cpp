#include <cpu/SceNpOfflineImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Cpu {
namespace {

// libSceNpManager and libSceNpWebApi2 follow the upstream HLE exports
// (core/libs/prx/libSceNpManager, core/libs/prx/libSceNpWebApi2).
constexpr std::uint32_t NpInvalidArgument = 0x80550003;
constexpr std::uint32_t NpSignedOut = 0x80550006;
constexpr std::uint32_t NpCallbackAlreadyRegistered = 0x80550008;
constexpr std::uint32_t NpCallbackNotRegistered = 0x80550009;
constexpr std::uint32_t WebApi2InvalidArgument = 0x80553402;
constexpr std::uint32_t WebApi2Unavailable = 0x80553406;
// libSceNpWebApi has no upstream export yet; its codes sit in the same table as
// WebApi2's at 0x805529xx (INVALID_ARGUMENT 02, NOT_SIGNED_IN 07).
constexpr std::uint32_t WebApiInvalidArgument = 0x80552902;
constexpr std::uint32_t WebApiNotSignedIn = 0x80552907;

enum class Library { NpManager, WebApi, WebApi2 };
constexpr std::array<std::string_view, 3> LibraryNames{"libSceNpManager", "libSceNpWebApi", "libSceNpWebApi2"};

std::uint64_t argument(Machine& guest, unsigned index) {
    constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
    return guest.Get(registers.at(index));
}

template <typename Value>
bool store(Machine& guest, std::uint64_t address, Value value) {
    if (!address || sizeof(Value) > std::numeric_limits<std::uint64_t>::max() - address) return false;
    try { guest.CheckAccess(address, sizeof(Value), Permission::Write); }
    catch (const std::runtime_error&) { return false; }
    std::array<std::byte, sizeof(Value)> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<std::byte>(static_cast<std::uint64_t>(value) >> (index * 8));
    guest.Write(address, bytes);
    return true;
}

}

struct SceNpOfflineImports::Impl {
    using Handler = std::function<std::uint32_t(Impl&, Machine&)>;
    struct Function { Library Scope; std::string_view Name; Handler Run; };
    std::shared_ptr<SceHostTrampolines> trampolines;
    std::map<std::string, Function> functions;
    std::map<std::string, std::uint64_t> gates;
    std::int32_t nextHandle = 1;
    std::uint64_t reachabilityCallback = 0;

    std::uint32_t handle() {
        if (nextHandle == std::numeric_limits<std::int32_t>::max()) nextHandle = 1;
        return static_cast<std::uint32_t>(nextHandle++);
    }

    Impl(Machine& machine, std::uint64_t base) : trampolines(std::make_shared<SceHostTrampolines>(machine, base, 64)) {
        const auto ok = [](Impl&, Machine&) -> std::uint32_t { return 0; };
        const auto signedOut = [](Impl&, Machine&) -> std::uint32_t { return NpSignedOut; };
        const auto newHandle = [](Impl& self, Machine&) -> std::uint32_t { return self.handle(); };
        const auto callback = [](Impl&, Machine& guest) -> std::uint32_t { return argument(guest, 0) ? 0 : NpInvalidArgument; };
        const auto webApi2Unavailable = [](Impl&, Machine&) -> std::uint32_t { return WebApi2Unavailable; };
        const auto webApiNotSignedIn = [](Impl&, Machine&) -> std::uint32_t { return WebApiNotSignedIn; };
        // CreateRequest(context, apiGroup, path, method, content, int64_t* requestId)
        const auto createRequest = [](std::uint32_t invalid) {
            return [invalid](Impl& self, Machine& guest) -> std::uint32_t {
                const auto request = static_cast<std::int64_t>(self.handle());
                return store(guest, argument(guest, 5), request) ? 0 : invalid;
            };
        };
        const std::initializer_list<Function> table{
            {Library::NpManager, "sceNpAbortRequest", ok},
            {Library::NpManager, "sceNpCheckCallback", ok},
            {Library::NpManager, "sceNpCheckNpAvailability", signedOut},
            {Library::NpManager, "sceNpCheckNpReachability", signedOut},
            {Library::NpManager, "sceNpCheckPremium", signedOut},
            {Library::NpManager, "sceNpCreateAsyncRequest", newHandle},
            {Library::NpManager, "sceNpCreateRequest", newHandle},
            {Library::NpManager, "sceNpDeleteRequest", ok},
            {Library::NpManager, "sceNpGetAccountAge", [](Impl&, Machine& guest) -> std::uint32_t {
                return argument(guest, 2) ? NpSignedOut : NpInvalidArgument; }},
            {Library::NpManager, "sceNpGetAccountCountryA", signedOut},
            {Library::NpManager, "sceNpGetAccountIdA", signedOut},
            {Library::NpManager, "sceNpGetAccountLanguage2", signedOut},
            {Library::NpManager, "sceNpGetNpId", [](Impl&, Machine& guest) -> std::uint32_t {
                return argument(guest, 1) ? NpSignedOut : NpInvalidArgument; }},
            {Library::NpManager, "sceNpGetNpReachabilityState", [](Impl&, Machine& guest) -> std::uint32_t {
                return store(guest, argument(guest, 1), std::uint32_t{0}) ? 0 : NpInvalidArgument; }},
            {Library::NpManager, "sceNpGetOnlineId", signedOut},
            {Library::NpManager, "sceNpGetUserIdByAccountId", signedOut},
            {Library::NpManager, "sceNpHasSignedUp", [](Impl&, Machine& guest) -> std::uint32_t {
                return store(guest, argument(guest, 1), std::uint8_t{0}) ? 0 : NpInvalidArgument; }},
            {Library::NpManager, "sceNpPollAsync", [](Impl&, Machine& guest) -> std::uint32_t {
                if (const auto result = argument(guest, 1)) store(guest, result, NpSignedOut);
                return 0; }},
            {Library::NpManager, "sceNpRegisterGamePresenceCallback", ok},
            {Library::NpManager, "sceNpRegisterNpReachabilityStateCallback", [](Impl& self, Machine& guest) -> std::uint32_t {
                const auto function = argument(guest, 0);
                if (!function) return NpInvalidArgument;
                if (self.reachabilityCallback) return NpCallbackAlreadyRegistered;
                self.reachabilityCallback = function;
                return 0; }},
            {Library::NpManager, "sceNpUnregisterNpReachabilityStateCallback", [](Impl& self, Machine&) -> std::uint32_t {
                if (!self.reachabilityCallback) return NpCallbackNotRegistered;
                self.reachabilityCallback = 0;
                return 0; }},
            {Library::NpManager, "sceNpRegisterPlusEventCallback", callback},
            {Library::NpManager, "sceNpRegisterPremiumEventCallback", callback},
            {Library::NpManager, "sceNpRegisterStateCallback", callback},
            {Library::NpManager, "sceNpRegisterStateCallbackA", [](Impl&, Machine& guest) -> std::uint32_t {
                return argument(guest, 0) ? 1 : NpInvalidArgument; }},
            {Library::NpManager, "sceNpSetContentRestriction", ok},
            {Library::NpManager, "sceNpSetNpTitleId", ok},
            {Library::NpManager, "sceNpNotifyPremiumFeature", ok},
            {Library::NpManager, "sceNpUnregisterStateCallback", ok},
            {Library::NpManager, "sceNpUnregisterStateCallbackA", ok},
            {Library::NpManager, "sceNpUnregisterPremiumEventCallback", ok},

            {Library::WebApi2, "sceNpWebApi2AbortRequest", ok},
            {Library::WebApi2, "sceNpWebApi2AddHttpRequestHeader", ok},
            {Library::WebApi2, "sceNpWebApi2CheckTimeout", ok},
            {Library::WebApi2, "sceNpWebApi2CreateRequest", createRequest(WebApi2InvalidArgument)},
            {Library::WebApi2, "sceNpWebApi2CreateUserContext", newHandle},
            {Library::WebApi2, "sceNpWebApi2DeleteRequest", ok},
            {Library::WebApi2, "sceNpWebApi2DeleteUserContext", ok},
            {Library::WebApi2, "sceNpWebApi2GetHttpResponseHeaderValue", webApi2Unavailable},
            {Library::WebApi2, "sceNpWebApi2GetHttpResponseHeaderValueLength", webApi2Unavailable},
            {Library::WebApi2, "sceNpWebApi2Initialize", newHandle},
            {Library::WebApi2, "sceNpWebApi2PushEventCreateFilter", newHandle},
            {Library::WebApi2, "sceNpWebApi2PushEventCreateHandle", newHandle},
            {Library::WebApi2, "sceNpWebApi2PushEventDeleteHandle", ok},
            {Library::WebApi2, "sceNpWebApi2PushEventDeletePushContext", ok},
            {Library::WebApi2, "sceNpWebApi2PushEventRegisterCallback", newHandle},
            {Library::WebApi2, "sceNpWebApi2ReadData", webApi2Unavailable},
            {Library::WebApi2, "sceNpWebApi2SendRequest", webApi2Unavailable},
            {Library::WebApi2, "sceNpWebApi2Terminate", ok},
            {Library::WebApi2, "sceNpWebApi2PushEventCreatePushContext", webApi2Unavailable},
            {Library::WebApi2, "sceNpWebApi2PushEventDeleteFilter", ok},
            {Library::WebApi2, "sceNpWebApi2PushEventRegisterPushContextCallback", newHandle},
            {Library::WebApi2, "sceNpWebApi2PushEventStartPushContextCallback", ok},
            {Library::WebApi2, "sceNpWebApi2PushEventUnregisterCallback", ok},
            {Library::WebApi2, "sceNpWebApi2PushEventUnregisterPushContextCallback", ok},
            {Library::WebApi2, "sceNpWebApi2SetRequestTimeout", ok},

            {Library::WebApi, "sceNpWebApiInitialize", newHandle},
            {Library::WebApi, "sceNpWebApiTerminate", ok},
            {Library::WebApi, "sceNpWebApiCreateContextA", newHandle},
            {Library::WebApi, "sceNpWebApiDeleteContext", ok},
            {Library::WebApi, "sceNpWebApiCreateRequest", createRequest(WebApiInvalidArgument)},
            {Library::WebApi, "sceNpWebApiDeleteRequest", ok},
            {Library::WebApi, "sceNpWebApiAbortRequest", ok},
            {Library::WebApi, "sceNpWebApiAddHttpRequestHeader", ok},
            {Library::WebApi, "sceNpWebApiCheckTimeout", ok},
            {Library::WebApi, "sceNpWebApiSendRequest", webApiNotSignedIn},
            {Library::WebApi, "sceNpWebApiSendRequest2", webApiNotSignedIn},
            {Library::WebApi, "sceNpWebApiReadData", webApiNotSignedIn},
        };
        for (const auto& function : table)
            if (!functions.emplace(Nid::ComputeNid(std::string(function.Name), ""), function).second)
                throw std::logic_error("Duplicate offline NP NID for " + std::string(function.Name));
    }
};

SceNpOfflineImports::SceNpOfflineImports(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceNpOfflineImports::~SceNpOfflineImports() = default;

std::optional<std::uint64_t> SceNpOfflineImports::Resolve(const SceImport& import) {
    const auto found = impl->functions.find(import.Nid);
    if (found == impl->functions.end()) return std::nullopt;
    const auto library = LibraryNames[static_cast<std::size_t>(found->second.Scope)];
    if (import.LibraryName != library || import.ModuleName != library || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    const auto gate = impl->trampolines->Add([state = std::weak_ptr<Impl>(impl), run = found->second.Run](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE offline NP import runtime has expired");
        // Results are signed 32-bit SCE status codes, sign-extended into RAX.
        guest.Set(Register::Rax, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(run(*context, guest)))));
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
