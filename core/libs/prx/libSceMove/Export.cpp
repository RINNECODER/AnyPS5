#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceMoveClose(int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceMoveGetExtensionPortInfo(int32_t handle, void* data) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceMoveReadStateLatest(int32_t handle, void* data) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceMoveReadStateRecent(int32_t handle, int64_t timestamp, void* data, int32_t* out_count) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceMoveResetLightSphere(int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceMoveSetLightSphere(int32_t handle, uint8_t red, uint8_t green, uint8_t blue) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceMoveGetBluetoothAddress(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveGetCalibrationInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveGetDeviceId(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveIsReproductionModel(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveIsValidHandle(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveReadStateRecentForTracker(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveSetCalibrationMode(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveSetExtensionPortOutput(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveSetLightSphereForTracker(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI sceMoveWriteCalibrationInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
