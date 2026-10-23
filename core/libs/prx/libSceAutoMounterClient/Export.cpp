#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

struct AutoMounterUsbDeviceInfo;

extern "C" {

int APS5_VABI sceAutoMounterClientGetUsbDeviceInfo(const char* deviceName, AutoMounterUsbDeviceInfo* info) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAutoMounterClientGetUsbDeviceList(char (*deviceNames)[64], std::int32_t* count) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAutoMounterClientInit(PthreadAttr* threadAttr) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
