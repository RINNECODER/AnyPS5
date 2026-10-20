#include "prx/libc/include/general/VabiMacros.hpp"
#include "SceTypes.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
int APS5_VABI sceNpGetNpId(int user_id, NpId* np_id);
int APS5_VABI sceNpGetUserIdByAccountId(std::uint64_t account_id, int* user_id);
int APS5_VABI sceNpUnknown_8weiQyIdI3A(int userId, void* buffer);
int APS5_VABI sceNpUnknown_WxLcN76q19o(void);
int APS5_VABI sceNpGetAccountCountry(const NpOnlineId* online_id, void* country_code);
int APS5_VABI sceNpGetAccountDateOfBirth(const NpOnlineId* online_id, void* date_of_birth);
int APS5_VABI sceNpGetAccountLanguage(int req_id, const NpOnlineId* online_id, void* language);
int APS5_VABI sceNpGetParentalControlInfo(int req_id, const NpOnlineId* online_id, std::int8_t* age, void* info);
int APS5_VABI sceNpGetUserIdByOnlineId(const NpOnlineId* online_id, int* user_id);
int APS5_VABI sceNpCheckPlus(int req_id, const void* param, void* result);
int APS5_VABI sceNpNotifyPlusFeature(const void* param);
int APS5_VABI sceNpGetAccountId(const NpOnlineId* online_id, std::uint64_t* account_id);
int APS5_VABI sceNpSetGamePresenceOnline(int user_id, std::uint32_t timeout);
int APS5_VABI sceNpRegisterPlusEventCallback(void* callback, void* userdata);
int APS5_VABI sceNpUnregisterPlusEventCallback(void);
}

extern "C" {
int APS5_VABI sceNpCreateRequest(void);
int APS5_VABI sceNpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param);
int APS5_VABI sceNpDeleteRequest(int reqId);
int APS5_VABI sceNpSetTimeout(int reqId, std::int32_t resolveRetry, std::uint32_t resolveTimeout,
    std::uint32_t connTimeout, std::uint32_t sendTimeout, std::uint32_t recvTimeout);
}

namespace {

constexpr int InvalidArgument = static_cast<int>(0x80550003u);
constexpr int SignedOut = static_cast<int>(0x80550006u);
constexpr int RequestNotFound = static_cast<int>(0x80550014u);

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "NpManager: %s\n", message);
        std::abort();
    }
}

void CheckRequestTimeouts() {
    Require(sceNpSetTimeout(0x7fffffff, 2, 0, 0, 0, 0) == RequestNotFound, "never created request");
    Require(sceNpSetTimeout(0x7fffffff, 0, 0, 0, 0, 0) == InvalidArgument, "arguments checked before the request");
    const int requestId = sceNpCreateRequest();
    Require(requestId > 0, "sceNpCreateRequest must return a positive request id");
    Require(sceNpSetTimeout(requestId, 2, 1000000, 10000000, 10000000, 10000000) == 0, "minimum timeouts");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 0, 30000000) == 0, "only the receive timeout");
    Require(sceNpSetTimeout(requestId, 3, 0, 0, 0, 0) == 0, "only the resolve retries");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 0, 0) == InvalidArgument, "every value left at its default");
    Require(sceNpSetTimeout(0, 2, 0, 0, 0, 0) == InvalidArgument, "request id 0");
    Require(sceNpSetTimeout(-1, 2, 0, 0, 0, 0) == InvalidArgument, "negative request id");
    Require(sceNpSetTimeout(requestId, -1, 0, 0, 0, 30000000) == InvalidArgument, "negative resolve retries");
    Require(sceNpSetTimeout(requestId, 0, 999999, 0, 0, 0) == InvalidArgument, "resolve timeout below 1 s");
    Require(sceNpSetTimeout(requestId, 0, 0, 9999999, 0, 0) == InvalidArgument, "connect timeout below 10 s");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 9999999, 0) == InvalidArgument, "send timeout below 10 s");
    Require(sceNpSetTimeout(requestId, 0, 0, 0, 0, 1) == InvalidArgument, "receive timeout below 10 s");
    Require(sceNpDeleteRequest(requestId) == 0, "sceNpDeleteRequest failed");
    Require(sceNpSetTimeout(requestId, 2, 0, 0, 0, 0) == RequestNotFound, "deleted request");
    NpCreateAsyncRequestParameter param{};
    const int asyncId = sceNpCreateAsyncRequest(&param);
    Require(asyncId > 0, "sceNpCreateAsyncRequest must return a positive request id");
    Require(sceNpSetTimeout(asyncId, 2, 0, 0, 0, 0) == 0, "async request");
    Require(sceNpDeleteRequest(asyncId) == 0, "sceNpDeleteRequest failed on the async request");
    Require(sceNpSetTimeout(asyncId, 2, 0, 0, 0, 0) == RequestNotFound, "deleted async request");
}

void CheckOnlineIdQueries() {
    NpOnlineId onlineId{};
    std::memcpy(onlineId.data, "player", 6);
    unsigned char output[64];
    std::memset(output, 0x5a, sizeof(output));
    unsigned char untouched[64];
    std::memset(untouched, 0x5a, sizeof(untouched));
    std::int8_t age = 0x5a;
    int userId = 0x5a5a;
    Require(sceNpGetAccountCountry(&onlineId, output) == UserNotFound, "sceNpGetAccountCountry must find no user");
    Require(sceNpGetAccountCountry(nullptr, output) == InvalidArgument, "sceNpGetAccountCountry must reject a null online id");
    Require(sceNpGetAccountCountry(&onlineId, nullptr) == InvalidArgument, "sceNpGetAccountCountry must reject a null output");
    Require(sceNpGetAccountDateOfBirth(&onlineId, output) == UserNotFound, "sceNpGetAccountDateOfBirth must find no user");
    Require(sceNpGetAccountDateOfBirth(nullptr, output) == InvalidArgument, "sceNpGetAccountDateOfBirth must reject a null online id");
    Require(sceNpGetAccountDateOfBirth(&onlineId, nullptr) == InvalidArgument, "sceNpGetAccountDateOfBirth must reject a null output");
    Require(sceNpGetAccountLanguage(1, &onlineId, output) == UserNotFound, "sceNpGetAccountLanguage must find no user");
    Require(sceNpGetAccountLanguage(1, nullptr, output) == InvalidArgument, "sceNpGetAccountLanguage must reject a null online id");
    Require(sceNpGetAccountLanguage(1, &onlineId, nullptr) == InvalidArgument, "sceNpGetAccountLanguage must reject a null output");
    Require(sceNpGetParentalControlInfo(1, &onlineId, &age, output) == UserNotFound, "sceNpGetParentalControlInfo must find no user");
    Require(sceNpGetParentalControlInfo(1, nullptr, &age, output) == InvalidArgument, "sceNpGetParentalControlInfo must reject a null online id");
    Require(sceNpGetParentalControlInfo(1, &onlineId, nullptr, output) == InvalidArgument, "sceNpGetParentalControlInfo must reject a null age");
    Require(sceNpGetParentalControlInfo(1, &onlineId, &age, nullptr) == InvalidArgument, "sceNpGetParentalControlInfo must reject a null info");
    Require(sceNpGetUserIdByOnlineId(&onlineId, &userId) == SignedOut, "sceNpGetUserIdByOnlineId must report NP as signed out");
    Require(sceNpGetUserIdByOnlineId(nullptr, &userId) == InvalidArgument, "sceNpGetUserIdByOnlineId must reject a null online id");
    Require(sceNpGetUserIdByOnlineId(&onlineId, nullptr) == InvalidArgument, "sceNpGetUserIdByOnlineId must reject a null output");
    Require(sceNpCheckPlus(1, output, output + 32) == SignedOut, "sceNpCheckPlus must report NP as signed out");
    Require(sceNpCheckPlus(0, output, output + 32) == InvalidArgument, "sceNpCheckPlus must reject request id 0");
    Require(sceNpCheckPlus(1, nullptr, output + 32) == InvalidArgument, "sceNpCheckPlus must reject a null parameter");
    Require(sceNpCheckPlus(1, output, nullptr) == InvalidArgument, "sceNpCheckPlus must reject a null result");
    std::uint64_t accountId = 0x5a5a;
    Require(sceNpGetAccountId(&onlineId, &accountId) == SignedOut, "sceNpGetAccountId must report NP as signed out");
    Require(sceNpGetAccountId(nullptr, &accountId) == InvalidArgument, "sceNpGetAccountId must reject a null online id");
    Require(sceNpGetAccountId(&onlineId, nullptr) == InvalidArgument, "sceNpGetAccountId must reject a null output");
    Require(sceNpSetGamePresenceOnline(0x10000000, 0) == SignedOut, "sceNpSetGamePresenceOnline must report NP as signed out");
    Require(sceNpRegisterPlusEventCallback(output, nullptr) == 0 && sceNpUnregisterPlusEventCallback() == 0, "the plus event callback must register and unregister");
    Require(sceNpUnregisterPlusEventCallback() == 0, "unregistering the plus event callback again must succeed like the premium one");
    Require(std::memcmp(output, untouched, sizeof(output)) == 0 && age == 0x5a && userId == 0x5a5a && accountId == 0x5a5a, "a failed query wrote its output");
    try {
        sceNpNotifyPlusFeature(output);
        Require(false, "sceNpNotifyPlusFeature must report that it is not implemented");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "sceNpNotifyPlusFeature not implemented", "sceNpNotifyPlusFeature threw");
    }
}

}

int main() {
    CheckRequestTimeouts();
    NpId npId{};
    std::memset(&npId, 0x5a, sizeof(npId));
    NpId untouched{};
    std::memset(&untouched, 0x5a, sizeof(untouched));
    Require(sceNpGetNpId(0x10000, &npId) == SignedOut, "sceNpGetNpId must report the user as signed out");
    Require(std::memcmp(&npId, &untouched, sizeof(npId)) == 0, "sceNpGetNpId must leave the NpId untouched");
    Require(sceNpGetNpId(0x10000, nullptr) == InvalidArgument, "sceNpGetNpId must reject a null NpId");
    int userId = 0x5a5a;
    Require(sceNpGetUserIdByAccountId(0, &userId) == InvalidArgument, "sceNpGetUserIdByAccountId must reject account id 0");
    Require(sceNpGetUserIdByAccountId(0x1234, nullptr) == InvalidArgument, "sceNpGetUserIdByAccountId must reject a null output");
    Require(sceNpGetUserIdByAccountId(0x1234, &userId) == SignedOut, "sceNpGetUserIdByAccountId must report NP as signed out");
    Require(userId == 0x5a5a, "sceNpGetUserIdByAccountId must leave the user id untouched");
    unsigned char buffer[0x11]{};
    try {
        sceNpUnknown_8weiQyIdI3A(0x10000, buffer);
        Require(false, "sceNpUnknown_8weiQyIdI3A must report that it is not implemented");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "sceNpUnknown_8weiQyIdI3A not implemented",
            "sceNpUnknown_8weiQyIdI3A threw");
    }
    try {
        sceNpUnknown_WxLcN76q19o();
        Require(false, "sceNpUnknown_WxLcN76q19o must report that it is not implemented");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "sceNpUnknown_WxLcN76q19o not implemented",
            "sceNpUnknown_WxLcN76q19o threw");
    }
    return 0;
}
