#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

void APS5_VABI sceSysUtilSendSystemNotificationWithText(int32_t type, const char* message) {
    NotImplemented_nid_no_patch(__func__);
}

}
