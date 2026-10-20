#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

struct BgftInitParam {
    void* heap;
    uint64_t heapSize;
};

struct BgftProgress;
struct BgftStorageTaskParam;

static_assert(sizeof(BgftInitParam) == 16);
static_assert(offsetof(BgftInitParam, heapSize) == 8);

extern "C" {

int APS5_VABI sceBgftServiceInit(const BgftInitParam* params) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceTerm(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadPauseTaskAll() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadResumeTaskAll() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadStartTaskAll() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadStopTaskAll() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadFindActivePatchTask(const char* contentId, int32_t* taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadFindActivePupTask(int32_t* taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadFindTaskByContentId(const char* contentId, int32_t subType, int32_t* taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadGetProgress(int32_t taskId, BgftProgress* progress) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadPauseTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadResumeTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadStartTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceDownloadStopTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadFindActiveGameAndGameAcTask(const char* contentId, int32_t* taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadFindActiveTask(const char* contentId, int32_t subType, int32_t* taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadGetGameAndGameAcProgress(const char* contentId, BgftProgress* progress) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadGetPatchProgress() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadGetPlayGoProgress(const char* contentId, BgftProgress* progress) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadGetProgress(int32_t taskId, BgftProgress* progress) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadGetTaskInfo() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadPauseTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadRegisterTaskByStorageEx(const BgftStorageTaskParam* params, int32_t* taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadResumeTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadStopTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadStartTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntDownloadUnregisterTask(int32_t taskId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}
int APS5_VABI sceBgftServiceIntInit(const BgftInitParam* params) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
