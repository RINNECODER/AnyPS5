#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceScreenShotSetOverlayImage(const char* path, int32_t x, int32_t y) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceScreenShotSetOverlayImageWithOrigin(const char* path, int32_t x, int32_t y, int32_t origin) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
