#include <cstdint>
#include <cstddef>
#include <mutex>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr std::int32_t VOICE_QOS_APP_TYPE_GAME = 0x20000000;
constexpr std::int32_t VOICE_QOS_APP_TYPE_10000000 = 0x10000000;
constexpr std::uint32_t VOICE_QOS_MEMORY_SIZE = 0x40000;
constexpr int SCE_VOICE_ERROR_LIBVOICEQOS_NOT_INIT = static_cast<int>(0x804E0901);
constexpr int SCE_VOICE_ERROR_LIBVOICEQOS_ARGUMENT_INVALID = static_cast<int>(0x804E0902);
constexpr int SCE_VOICE_ERROR_LIBVOICEQOS_INITIALIZED = static_cast<int>(0x804E0905);

std::mutex g_mutex;
bool g_initialized = false;

}

extern "C" {

int APS5_VABI sceVoiceQoSInit(void* mem_block, uint32_t mem_size, int32_t app_type) {
    std::lock_guard lock(g_mutex);
    if (g_initialized) return SCE_VOICE_ERROR_LIBVOICEQOS_INITIALIZED;
    if (!mem_block || mem_size != VOICE_QOS_MEMORY_SIZE || (app_type != VOICE_QOS_APP_TYPE_GAME && app_type != VOICE_QOS_APP_TYPE_10000000)) return SCE_VOICE_ERROR_LIBVOICEQOS_ARGUMENT_INVALID;
    g_initialized = true;
    return 0;
}

int APS5_VABI sceVoiceQoSEnd() {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return SCE_VOICE_ERROR_LIBVOICEQOS_NOT_INIT;
    g_initialized = false;
    return 0;
}

int APS5_VABI sceVoiceQoSCreateLocalEndpoint() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSDeleteLocalEndpoint() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSCreateRemoteEndpoint() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSDeleteRemoteEndpoint() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSConnect() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSDisconnect() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSSetConnectionAttribute() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSSetLocalEndpointAttribute() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSSetRemoteEndpointAttribute() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSReadPacket() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSWritePacket() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceVoiceQoSGetLocalEndpointAttribute() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSGetRemoteEndpointAttribute(int32_t remoteId, int32_t attributeId, const void* value, int32_t size) {
    (void)remoteId;
    (void)attributeId;
    (void)value;
    (void)size;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSGetConnectionAttribute(int32_t connectionId, int32_t attributeId, void* value, int32_t size) {
    (void)connectionId;
    (void)attributeId;
    (void)value;
    (void)size;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSGetLocalEndpoint(int32_t connectionId, int32_t* localId) {
    return Locked([&](QoS& qos) {
        if (!localId) return SCE_VOICE_ERROR_LIBVOICEQOS_ARGUMENT_INVALID;
        const auto it = qos.connections.find(connectionId);
        if (it == qos.connections.end()) return SCE_VOICE_ERROR_LIBVOICEQOS_ARGUMENT_INVALID;
        *localId = it->second.local;
        return 0;
    });
}

int APS5_VABI sceVoiceQoSGetRemoteEndpoint(int32_t connectionId, int32_t* remoteId) {
    return Locked([&](QoS& qos) {
        if (!remoteId) return SCE_VOICE_ERROR_LIBVOICEQOS_ARGUMENT_INVALID;
        const auto it = qos.connections.find(connectionId);
        if (it == qos.connections.end()) return SCE_VOICE_ERROR_LIBVOICEQOS_ARGUMENT_INVALID;
        *remoteId = it->second.remote;
        return 0;
    });
}

int APS5_VABI sceVoiceQoSEnableChat() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSDisableChat() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSGetStatus(int32_t connectionId, int32_t statusId, void* value, int32_t size) {
    (void)connectionId;
    (void)statusId;
    (void)value;
    (void)size;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSDebugGetStatus() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSInitHQ() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSSetMode() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceVoiceQoSTerminate() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
