#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAgcDriverRegisterDefaultOwner(uint32_t owner) {
    (void)owner;
    return 0;
}

int APS5_VABI sceAgcDriverGetHsOffchipParam(uint16_t* first, uint16_t* second) {
    (void)first;
    (void)second;
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
