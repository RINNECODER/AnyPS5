#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include <atomic>

// Peer-to-peer signaling needs the network: contexts exist, but sessions never activate.
static constexpr int SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT = static_cast<int>(0x80553303);
static constexpr int SCE_NP_SESSION_SIGNALING_ERROR_CONN_NOT_FOUND = static_cast<int>(0x8055330C);
static constexpr int SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE = static_cast<int>(0x80552D06);
static std::atomic<uint32_t> g_nextContext{1};

struct SessionSignalingInitParam {
    std::int32_t userId;
    std::uint64_t poolSize;
    std::uint64_t affinityMask;
    std::int32_t threadPriority;
    std::uint64_t stackSize;
};

struct SessionSignalingMemoryInfo {
    std::uint64_t poolSize;
    std::uint64_t usedSize;
    std::uint64_t peakUsedSize;
};

struct SessionSignalingPeerAddress {
    std::uint64_t accountId;
    std::uint32_t platform;
};

struct SessionSignalingSessionParam {
    std::uint32_t mode;
    std::uint32_t option;
};

struct SessionSignalingNetAddress {
    std::uint32_t address;
    std::uint16_t port;
};

union SessionSignalingConnectionInfo {
    std::uint32_t value;
    SessionSignalingNetAddress netAddress;
    SessionSignalingPeerAddress peerAddress;
};

struct SessionSignalingLocalNetInfo {
    std::uint32_t localAddress;
    std::uint32_t mappedAddress;
    std::uint32_t mappedPort;
    std::uint32_t mappingStatus;
};

using SessionSignalingEventCallback = void (APS5_VABI *)(std::uint32_t, std::uint32_t, std::int32_t, const SessionSignalingPeerAddress*, std::int32_t, void*);
using SessionSignalingConnectionCallback = void (APS5_VABI *)(std::uint32_t, std::uint32_t, std::int32_t, std::int32_t, void*);
using SessionSignalingConnectionCallback2 = void (APS5_VABI *)(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t, std::int32_t, void*);

template<typename TConnectionCallback>
struct SessionSignalingContextParam {
    std::int32_t userId;
    std::uint32_t serviceLabel;
    SessionSignalingEventCallback prepareCallback;
    void* prepareUserData;
    SessionSignalingEventCallback sessionCallback;
    void* sessionUserData;
    TConnectionCallback connectionCallback;
    void* connectionUserData;
};

using SessionSignalingContextParam1 = SessionSignalingContextParam<SessionSignalingConnectionCallback>;
using SessionSignalingContextParam2 = SessionSignalingContextParam<SessionSignalingConnectionCallback2>;

struct SessionSignalingConnectionStatistics {
    std::uint32_t peakConnectionCount;
    std::uint32_t connectionCount;
    std::uint32_t connectingCount;
    std::uint32_t connectedCount;
};

struct SessionSignalingGroupInfo {
    std::uint32_t connectionIds[64];
    std::uint64_t connectionCount;
};

static_assert(sizeof(SessionSignalingMemoryInfo) == 24);
static_assert(sizeof(SessionSignalingInitParam) == 40);
static_assert(offsetof(SessionSignalingInitParam, poolSize) == 8);
static_assert(offsetof(SessionSignalingInitParam, affinityMask) == 16);
static_assert(offsetof(SessionSignalingInitParam, threadPriority) == 24);
static_assert(offsetof(SessionSignalingInitParam, stackSize) == 32);
static_assert(offsetof(SessionSignalingMemoryInfo, usedSize) == 8);
static_assert(offsetof(SessionSignalingMemoryInfo, peakUsedSize) == 16);
static_assert(sizeof(SessionSignalingPeerAddress) == 16);
static_assert(sizeof(SessionSignalingSessionParam) == 8);
static_assert(offsetof(SessionSignalingSessionParam, option) == 4);
static_assert(sizeof(SessionSignalingNetAddress) == 8);
static_assert(offsetof(SessionSignalingNetAddress, port) == 4);
static_assert(sizeof(SessionSignalingConnectionInfo) == 16);
static_assert(sizeof(SessionSignalingLocalNetInfo) == 16);
static_assert(offsetof(SessionSignalingLocalNetInfo, mappedAddress) == 4);
static_assert(offsetof(SessionSignalingLocalNetInfo, mappedPort) == 8);
static_assert(offsetof(SessionSignalingLocalNetInfo, mappingStatus) == 12);
static_assert(sizeof(SessionSignalingContextParam1) == 56);
static_assert(sizeof(SessionSignalingContextParam2) == 56);
static_assert(offsetof(SessionSignalingContextParam1, prepareCallback) == 8);
static_assert(offsetof(SessionSignalingContextParam1, sessionCallback) == 24);
static_assert(offsetof(SessionSignalingContextParam1, connectionCallback) == 40);
static_assert(offsetof(SessionSignalingContextParam2, connectionCallback) == 40);
static_assert(offsetof(SessionSignalingContextParam2, connectionUserData) == 48);
static_assert(offsetof(SessionSignalingPeerAddress, platform) == 8);
static_assert(sizeof(SessionSignalingConnectionStatistics) == 16);
static_assert(offsetof(SessionSignalingConnectionStatistics, connectionCount) == 4);
static_assert(offsetof(SessionSignalingConnectionStatistics, connectingCount) == 8);
static_assert(offsetof(SessionSignalingConnectionStatistics, connectedCount) == 12);
static_assert(sizeof(SessionSignalingGroupInfo) == 264);
static_assert(offsetof(SessionSignalingGroupInfo, connectionCount) == 256);

extern "C" {

int APS5_VABI sceNpSessionSignalingInitialize(const SessionSignalingInitParam* param) {
    (void)param;
    return 0;
}

int APS5_VABI sceNpSessionSignalingActivateSession(std::uint32_t contextId, const char* sessionId, std::uint32_t sessionType, const SessionSignalingSessionParam* param, std::uint32_t* groupId) {
    return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingCreateContext2(const SessionSignalingContextParam2* param, std::uint32_t* contextId) {
    if (!param || !contextId) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
    *contextId = g_nextContext.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int APS5_VABI sceNpSessionSignalingCreateContext(const SessionSignalingContextParam1* param, std::uint32_t* contextId) {
    if (!param || !contextId) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
    *contextId = g_nextContext.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int APS5_VABI sceNpSessionSignalingDeactivate(uint32_t context_id) {
    (void)context_id;
    return 0;
}

int APS5_VABI sceNpSessionSignalingDestroyContext(uint32_t context_id) {
    (void)context_id;
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetConnectionInfo(std::uint32_t contextId, std::uint32_t connectionId, std::uint32_t code, SessionSignalingConnectionInfo* info) {
    return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingActivateUser(std::uint32_t contextId, const SessionSignalingPeerAddress* peerAddress, std::uint32_t* connectionId) {
    return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingGetConnectionFromPeerAddress2(std::uint32_t contextId, std::uint32_t groupId, const SessionSignalingPeerAddress* peerAddress, std::uint32_t* connectionId) {
    return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingTerminate(void) {
    return 0;
}

int32_t APS5_VABI sceNpSessionSignalingGetConnectionStatus(std::uint32_t contextId, std::uint32_t connectionId, std::int32_t* status, std::uint32_t* peerAddress, std::uint16_t* peerPort) {
 (void)contextId;
 (void)connectionId;
 (void)peerAddress;
 (void)peerPort;
 if (!status) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
 return SCE_NP_SESSION_SIGNALING_ERROR_CONN_NOT_FOUND;
}

int32_t APS5_VABI sceNpSessionSignalingGetLocalNetInfo(std::uint32_t contextId, SessionSignalingLocalNetInfo* info) {
 (void)contextId;
 if (!info) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
 return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingRequestPrepare(uint32_t contextId, uint32_t* requestId) {
 (void)contextId;
 if (!requestId) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
 return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingGetMemoryInfo(SessionSignalingMemoryInfo* memoryInfo) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetConnectionStatistics(SessionSignalingConnectionStatistics* statistics) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetConnectionFromPeerAddress(std::uint32_t contextId, const SessionSignalingPeerAddress* peerAddress, std::uint32_t* connectionId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetGroupFromSessionId(std::uint32_t contextId, const char* sessionId, std::uint32_t* groupId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetGroupInfo(std::uint32_t contextId, std::uint32_t groupId, SessionSignalingGroupInfo* groupInfo) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetConnectionFromNetAddress2(std::uint32_t contextId, std::uint32_t groupId, std::uint32_t address, std::uint16_t port, SessionSignalingGroupInfo* groupInfo) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingCreateJoinActivate() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetConnectionFromNetAddress() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetCustomProperty() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetGroupFromPeerAddress() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingInitializeWithPort() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingJoinActivate() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceNpSessionSignalingSetCustomProperty() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
