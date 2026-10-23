#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include "prx/libSceCommonDialog/CommonDialog.hpp"
#include <cstdlib>

extern "C" {
int APS5_VABI scePlayerReviewDialogInitialize(void);
int APS5_VABI scePlayerReviewDialogTerminate(void);
int APS5_VABI scePlayerReviewDialogOpen(const void* param);
int APS5_VABI scePlayerReviewDialogClose(void);
int APS5_VABI scePlayerReviewDialogGetStatus(void);
int APS5_VABI scePlayerReviewDialogUpdateStatus(void);
int APS5_VABI scePlayerReviewDialogGetResult(void* result);
}

namespace {

constexpr int COMMON_DIALOG_STATUS_NONE = 0;
constexpr int COMMON_DIALOG_STATUS_INITIALIZED = 1;
constexpr int COMMON_DIALOG_STATUS_FINISHED = 3;
constexpr int COMMON_DIALOG_RESULT_USER_CANCELED = 1;
constexpr int COMMON_DIALOG_ERROR_NOT_INITIALIZED = static_cast<int>(0x80B80003u);
constexpr int COMMON_DIALOG_ERROR_NOT_FINISHED = static_cast<int>(0x80B80005u);
constexpr int COMMON_DIALOG_ERROR_ARG_NULL = static_cast<int>(0x80B8000Du);

void Require(bool value) { if (!value) std::abort(); }

}

int main() {
    PlayerReviewDialogParam param{};
    param.base.size = sizeof(param.base);
    param.base.magic = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&param)) - 1060003575u;
    param.size = sizeof(param);
    param.userId = 1;
    std::int32_t result[4] = {-1, -1, -1, -1};
    Require(scePlayerReviewDialogTerminate() == COMMON_DIALOG_ERROR_NOT_INITIALIZED);
    Require(scePlayerReviewDialogOpen(nullptr) == static_cast<int>(0x80B8000Du));
    Require(scePlayerReviewDialogGetResult(nullptr) == static_cast<int>(0x80B8000Du));
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_NONE);
    Require(scePlayerReviewDialogOpen(&param) == COMMON_DIALOG_ERROR_NOT_INITIALIZED);
    Require(scePlayerReviewDialogClose() == COMMON_DIALOG_ERROR_NOT_INITIALIZED);
    Require(scePlayerReviewDialogGetResult(result) == COMMON_DIALOG_ERROR_NOT_INITIALIZED);

    Require(scePlayerReviewDialogInitialize() == 0);
    Require(scePlayerReviewDialogInitialize() == COMMON_DIALOG_ERROR_ALREADY_INITIALIZED);
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_INITIALIZED);
    Require(scePlayerReviewDialogUpdateStatus() == COMMON_DIALOG_STATUS_INITIALIZED);
    Require(scePlayerReviewDialogGetResult(result) == COMMON_DIALOG_ERROR_NOT_FINISHED);
    Require(scePlayerReviewDialogTerminate() == 0);
    Require(scePlayerReviewDialogTerminate() == COMMON_DIALOG_ERROR_NOT_INITIALIZED);
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_NONE);

    Require(scePlayerReviewDialogInitialize() == 0);
    Require(scePlayerReviewDialogOpen(nullptr) == COMMON_DIALOG_ERROR_ARG_NULL);
    param.base.size = 0;
    Require(scePlayerReviewDialogOpen(&param) == static_cast<int>(0x80B8000Au));
    param.base.size = sizeof(param.base);
    param.base.magic = 0;
    Require(scePlayerReviewDialogOpen(&param) == static_cast<int>(0x80B8000Au));
    param.base.magic = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&param)) - 1060003575u;
    param.size = 0;
    Require(scePlayerReviewDialogOpen(&param) == static_cast<int>(0x80B8000Au));
    param.size = sizeof(param);
    param.reserved[63] = 1;
    Require(scePlayerReviewDialogOpen(&param) == static_cast<int>(0x80B8000Au));
    param.reserved[63] = 0;
    param.userId = -1;
    Require(scePlayerReviewDialogOpen(&param) == static_cast<int>(0x80B8000Au));
    param.userId = 1;
    param.mode = 2;
    Require(scePlayerReviewDialogOpen(&param) == static_cast<int>(0x80B8000Au));
    param.mode = 0;
    Require(scePlayerReviewDialogOpen(&param) == 0);
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_FINISHED);
    Require(scePlayerReviewDialogUpdateStatus() == COMMON_DIALOG_STATUS_FINISHED);
    Require(scePlayerReviewDialogGetResult(nullptr) == COMMON_DIALOG_ERROR_ARG_NULL);
    Require(scePlayerReviewDialogGetResult(result) == COMMON_DIALOG_RESULT_USER_CANCELED);
    Require(result[0] == -1 && result[1] == -1 && result[3] == -1);
    Require(result[2] == COMMON_DIALOG_RESULT_USER_CANCELED);
    Require(scePlayerReviewDialogClose() == 0);
    Require(scePlayerReviewDialogTerminate() == 0);
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_NONE);
    Require(scePlayerReviewDialogInitialize() == 0);
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_INITIALIZED);
    Require(scePlayerReviewDialogClose() == 0);
    Require(scePlayerReviewDialogGetStatus() == COMMON_DIALOG_STATUS_FINISHED);
    Require(scePlayerReviewDialogUpdateStatus() == COMMON_DIALOG_STATUS_FINISHED);
    result[2] = -1;
    Require(scePlayerReviewDialogGetResult(result) == COMMON_DIALOG_RESULT_USER_CANCELED);
    Require(result[0] == -1 && result[1] == -1 && result[3] == -1);
    Require(result[2] == COMMON_DIALOG_RESULT_USER_CANCELED);
    Require(scePlayerReviewDialogTerminate() == 0);
}
