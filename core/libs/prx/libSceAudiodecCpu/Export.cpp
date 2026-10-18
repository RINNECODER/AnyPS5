#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"


extern "C" {

int32_t APS5_VABI sceAudiodecCpuClearContext(void*pRes, uint32_t uiCodecType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceAudiodecCpuDecode(void*pCtrl, void*pRes, uint32_t uiCodecType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceAudiodecCpuInitDecoder(void*pCtrl, void*pRes, uint32_t uiCodecType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int32_t APS5_VABI sceAudiodecCpuQueryMemSize(void*pCtrl, void*pRes, uint32_t uiCodecType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
