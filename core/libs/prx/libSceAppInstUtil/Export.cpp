#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

int APS5_VABI sceAppInstUtilInitialize(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilTerminate(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppInstallAll(void* reserved) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppInstallTitleDir(const char* titleId, const char* titleDir, void* reserved) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppIsInInstalling(int32_t* installing) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppIsInUpdating(const char* titleId, int32_t* updating) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppExists(const char* titleId, int32_t* exists) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppGetSize(const char* titleId, uint64_t* size) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppInstallPkg(const char* pkgPath, void* reserved) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppPrepareOverwritePkg(const char* pkgPath) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppRecoverApp(const char* titleId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppUnInstall(const char* titleId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppUnInstallAddcont(const char* titleId, const char* addcontName) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppUnInstallPat(const char* titleId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppUnInstallTheme(const char* contentId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilAppUnInstallTypes(const char* titleId, uint32_t deleteTypes) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilGetInstallProgressInfo(const char* contentId, uint32_t* state, uint32_t* progress, uint64_t* progressSize, uint64_t* totalSize, uint32_t* restSec) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilGetInstallProgress(const char* contentId, uint32_t* progress) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceAppInstUtilGetPrimaryAppSlot(const char* titleId, int32_t* slot) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
