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

int APS5_VABI sceNpSessionSignalingActivateSession(void) {
    return SCE_NP_SESSION_SIGNALING_ERROR_UNAVAILABLE;
}

int APS5_VABI sceNpSessionSignalingCreateContext2(const void* param, uint32_t* context_id) {
    (void)param;
    if (!context_id) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
    *context_id = g_nextContext.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

int APS5_VABI sceNpSessionSignalingCreateContext(const void* param, uint32_t* context_id) {
    return sceNpSessionSignalingCreateContext2(param, context_id);
}

int APS5_VABI sceNpSessionSignalingDeactivate(uint32_t context_id) {
    (void)context_id;
    return 0;
}

int APS5_VABI sceNpSessionSignalingDestroyContext(uint32_t context_id) {
    (void)context_id;
    return 0;
}

int APS5_VABI sceNpSessionSignalingGetConnectionInfo(void) {
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

int32_t APS5_VABI sceNpSessionSignalingGetConnectionStatus(int32_t context_id, int32_t connection_id, int32_t* status, void* peer_address, uint16_t* peer_port) {
 (void)context_id;
 (void)connection_id;
 (void)peer_address;
 (void)peer_port;
 if (!status) return SCE_NP_SESSION_SIGNALING_ERROR_INVALID_ARGUMENT;
 return SCE_NP_SESSION_SIGNALING_ERROR_CONN_NOT_FOUND;
}

int32_t APS5_VABI sceNpSessionSignalingGetLocalNetInfo(int32_t context_id, void* info) {
 (void)context_id;
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
