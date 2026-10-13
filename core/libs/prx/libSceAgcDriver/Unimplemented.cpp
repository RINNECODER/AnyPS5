#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAgcDriverRegisterDefaultOwner(uint32_t* owner_handle) {
    (void)owner_handle;
    return static_cast<int>(0x8A6C9018);
}

int APS5_VABI sceAgcDriverGetHsOffchipParam(uint16_t* first, uint16_t* second) {
    (void)first;
    (void)second;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
