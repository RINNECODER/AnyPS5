#include "SceTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

extern "C" {
int APS5_VABI sceNpEntitlementAccessAbortRequest(std::int64_t);
int APS5_VABI sceNpEntitlementAccessDeleteRequest(std::int64_t);
int APS5_VABI sceHttpSetCookieEnabled(int, int);
int APS5_VABI sceHttpSendRequest(int, const void*, std::size_t);
int APS5_VABI sceNpEntitlementAccessGetEntitlementKey(
    std::uint32_t, const NpUnifiedEntitlementLabel*, NpEntitlementAccessEntitlementKey*);
int APS5_VABI sceNpEntitlementAccessGetGameTrialsFlag(std::uint32_t*);
int APS5_VABI sceNpEntitlementAccessGetPftFlag(std::uint32_t*);
int APS5_VABI sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(std::int32_t userId, std::uint32_t serviceLabel, const NpUnifiedEntitlementLabel* entitlementLabels, std::uint32_t labelCount, const NpEntitlementAccessListParameter* parameter, std::int64_t* requestId);
int APS5_VABI sceNpEntitlementAccessPollUnifiedEntitlementInfoList(std::int64_t requestId, std::int32_t* result, NpEntitlementAccessUnifiedInfo* infos, std::uint32_t capacity, std::int32_t* firstResult, std::int32_t* secondResult, std::int32_t* thirdResult);
int APS5_VABI sceNpEntitlementAccessRequestServiceEntitlementInfoList(std::int32_t userId, std::uint32_t serviceLabel, const NpServiceEntitlementLabel* entitlementLabels, std::uint32_t labelCount, const NpEntitlementAccessListParameter* parameter, std::int64_t* requestId);
int APS5_VABI sceNpEntitlementAccessPollServiceEntitlementInfoList(std::int64_t requestId, std::int32_t* result, NpEntitlementAccessServiceInfo* infos, std::uint32_t capacity, std::int32_t* firstResult, std::int32_t* secondResult, std::int32_t* thirdResult);
int APS5_VABI sceNpEntitlementAccessRequestUnifiedEntitlementInfo(std::int32_t userId, std::uint32_t serviceLabel, const NpUnifiedEntitlementLabel* entitlementLabel, std::int64_t* requestId);
int APS5_VABI sceNpEntitlementAccessPollUnifiedEntitlementInfo(std::int64_t requestId, std::int32_t* result, NpEntitlementAccessUnifiedInfo* info);
int APS5_VABI sceNpEntitlementAccessRequestServiceEntitlementInfo(std::int32_t userId, std::uint32_t serviceLabel, const NpServiceEntitlementLabel* entitlementLabel, std::int64_t* requestId);
int APS5_VABI sceNpEntitlementAccessPollServiceEntitlementInfo(std::int64_t requestId, std::int32_t* result, NpEntitlementAccessServiceInfo* info);
int APS5_VABI sceRudpInit_nid_postfix(void*, int);
int APS5_VABI sceRudpActivate();
int APS5_VABI sceRudpGetStatus(void*, std::size_t);
int APS5_VABI sceRudpEnd();
}

static void Require(bool value) { if (!value) std::abort(); }

int main() {
    for (std::int64_t requestId : {std::int64_t{0}, std::int64_t{1}, INT64_MAX}) {
        bool abortFailed = false;
        bool deleteFailed = false;
        try {
            sceNpEntitlementAccessAbortRequest(requestId);
        } catch (const std::logic_error&) {
            abortFailed = true;
        }
        try {
            sceNpEntitlementAccessDeleteRequest(requestId);
        } catch (const std::logic_error&) {
            deleteFailed = true;
        }
        Require(abortFailed && deleteFailed);
    }

    constexpr int invalidValue = static_cast<int>(0x804311FE);
    constexpr int network = static_cast<int>(0x80431063);
    Require(sceHttpSetCookieEnabled(1, 0) == 0);
    Require(sceHttpSendRequest(1, nullptr, 0) == network);
    bool cookieUnsupported = false;
    try {
        sceHttpSetCookieEnabled(1, 1);
    } catch (const std::runtime_error& error) {
        cookieUnsupported = std::string_view(error.what()) == "sceHttpSetCookieEnabled not implemented";
    }
    Require(cookieUnsupported);
    for (int enabled : {-1, 2, 0x100}) {
        Require(sceHttpSetCookieEnabled(1, enabled) == invalidValue);
    }

    constexpr int parameter = static_cast<int>(0x817D0002);
    constexpr int noEntitlement = static_cast<int>(0x817D0007);
    static_assert(sizeof(NpEntitlementAccessEntitlementKey) == 16);
    NpUnifiedEntitlementLabel label{};
    std::memcpy(label.data, "unowned-addon", 14);
    struct KeyBuffer {
        std::uint64_t before;
        NpEntitlementAccessEntitlementKey key;
        std::uint64_t after;
    } output;
    std::memset(&output, 0xa5, sizeof(output));
    std::array<unsigned char, sizeof(output)> original{};
    std::memcpy(original.data(), &output, sizeof(output));
    Require(sceNpEntitlementAccessGetEntitlementKey(0, nullptr, &output.key) == parameter);
    Require(sceNpEntitlementAccessGetEntitlementKey(0, &label, nullptr) == parameter);
    Require(sceNpEntitlementAccessGetEntitlementKey(0, nullptr, nullptr) == parameter);
    for (std::uint32_t serviceLabel : {0u, 1u, 0xffffffffu}) {
        Require(sceNpEntitlementAccessGetEntitlementKey(serviceLabel, &label, &output.key) == noEntitlement);
        Require(std::memcmp(&output, original.data(), sizeof(output)) == 0);
    }

    std::array<std::uint32_t, 2> trialsFlag{0xa5a5a5a5u, 0x5a5a5a5au};
    Require(sceNpEntitlementAccessGetGameTrialsFlag(trialsFlag.data()) == 0);
    Require(trialsFlag[0] == 0 && trialsFlag[1] == 0x5a5a5a5au);
    Require(sceNpEntitlementAccessGetGameTrialsFlag(nullptr) == parameter);

    std::array<std::uint32_t, 2> pftFlag{0xa5a5a5a5u, 0x5a5a5a5au};
    Require(sceNpEntitlementAccessGetPftFlag(pftFlag.data()) == 0);
    Require(pftFlag[0] == 0 && pftFlag[1] == 0x5a5a5a5au);
    Require(sceNpEntitlementAccessGetPftFlag(nullptr) == parameter);

    constexpr int signedOut = static_cast<int>(0x80550006);
    Require(sceNpEntitlementAccessRequestUnifiedEntitlementInfoList(0, 0, nullptr, 0, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessPollUnifiedEntitlementInfoList(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessRequestServiceEntitlementInfoList(0, 0, nullptr, 0, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessPollServiceEntitlementInfoList(0, nullptr, nullptr, 0, nullptr, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessRequestUnifiedEntitlementInfo(0, 0, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessPollUnifiedEntitlementInfo(0, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessRequestServiceEntitlementInfo(0, 0, nullptr, nullptr) == signedOut);
    Require(sceNpEntitlementAccessPollServiceEntitlementInfo(0, nullptr, nullptr) == signedOut);

    Require(sceRudpActivate() == 0);
    std::array<unsigned char, 248> status;
    status.fill(0x5a);
    const auto originalStatus = status;
    constexpr int rudpNotInitialized = static_cast<int>(0x80770001);
    Require(sceRudpGetStatus(status.data(), status.size()) == rudpNotInitialized);
    Require(status == originalStatus);
    alignas(8) static unsigned char rudpPool[0xC58];
    Require(sceRudpInit_nid_postfix(rudpPool, sizeof(rudpPool)) == 0);
    Require(sceRudpActivate() == 0);
    Require(sceRudpGetStatus(status.data(), status.size()) == 0);
    for (unsigned char byte : status) Require(byte == 0);
    Require(sceRudpGetStatus(nullptr, 0) == static_cast<int>(0x80770004));
    Require(sceRudpEnd() == 0);
    return 0;
}
