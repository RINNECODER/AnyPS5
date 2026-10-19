#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAutoMounterClientGetUsbDeviceInfo(const char* device, void* info) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAutoMounterClientGetUsbDeviceList(char (*devices)[64], int32_t* count) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAutoMounterClientInit(uint64_t* value) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
