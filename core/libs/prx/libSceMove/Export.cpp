#include <atomic>
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {

constexpr int MOVE_OK = 0;
constexpr int MOVE_NO_CONTROLLER_CONNECTED = 1;
constexpr int MOVE_ERROR_NOT_INIT = static_cast<int>(0x80EE0001);
constexpr int MOVE_ERROR_ALREADY_INIT = static_cast<int>(0x80EE0002);
constexpr int MOVE_ERROR_INVALID_ARG = static_cast<int>(0x80EE0003);
constexpr int32_t MOVE_FIRST_HANDLE = 0x30B0100;
constexpr int32_t MOVE_HANDLE_STEP = 0x100;

std::atomic<bool> initialized{false};
std::atomic<int32_t> nextHandle{MOVE_FIRST_HANDLE};

}

extern "C" {

int APS5_VABI sceMoveInit(void) {
    bool expected = false;
    return initialized.compare_exchange_strong(expected, true) ? MOVE_OK : MOVE_ERROR_ALREADY_INIT;
}

int APS5_VABI sceMoveTerm(void) {
    return initialized.exchange(false) ? MOVE_OK : MOVE_ERROR_NOT_INIT;
}

int APS5_VABI sceMoveOpen(int32_t userId, int32_t type, int32_t index) {
    (void)userId;
    (void)type;
    (void)index;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    return nextHandle.fetch_add(MOVE_HANDLE_STEP);
}

int APS5_VABI sceMoveClose(int32_t handle) {
    (void)handle;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    return MOVE_OK;
}

int APS5_VABI sceMoveGetDeviceInfo(int32_t handle, void* info) {
    (void)handle;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    if (!info) return MOVE_ERROR_INVALID_ARG;
    return MOVE_NO_CONTROLLER_CONNECTED;
}

int APS5_VABI sceMoveGetExtensionPortInfo(int32_t handle, void* data) {
    (void)handle;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    if (!data) return MOVE_ERROR_INVALID_ARG;
    return MOVE_NO_CONTROLLER_CONNECTED;
}

int APS5_VABI sceMoveReadStateLatest(int32_t handle, void* data) {
    (void)handle;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    if (!data) return MOVE_ERROR_INVALID_ARG;
    return MOVE_NO_CONTROLLER_CONNECTED;
}

int APS5_VABI sceMoveReadStateRecent(int32_t handle, int64_t timestamp, void* data, int32_t* outCount) {
    (void)handle;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    if (timestamp < 0 || !data || !outCount) return MOVE_ERROR_INVALID_ARG;
    return MOVE_NO_CONTROLLER_CONNECTED;
}

int APS5_VABI sceMoveSetVibration(int32_t handle, uint8_t motor) {
    (void)handle;
    (void)motor;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    return MOVE_NO_CONTROLLER_CONNECTED;
}

int APS5_VABI sceMoveSetLightSphere(int32_t handle, uint8_t red, uint8_t green, uint8_t blue) {
    (void)handle;
    (void)red;
    (void)green;
    (void)blue;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    return MOVE_NO_CONTROLLER_CONNECTED;
}

int APS5_VABI sceMoveResetLightSphere(int32_t handle) {
    (void)handle;
    if (!initialized) return MOVE_ERROR_NOT_INIT;
    return MOVE_OK;
}

}
