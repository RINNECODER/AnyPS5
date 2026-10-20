#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <chrono>
#include <thread>

#include "SceTypes.hpp"
#include "prx//libc/include/General.hpp"
#include "prx/libScePad/include/Pad.hpp"
#include "prx/libScePad/include/PadState.hpp"

namespace {

constexpr int PAD_ERROR_DEVICE_NOT_CONNECTED = static_cast<int>(0x80920007);
constexpr int PAD_ERROR_DEVICE_NO_HANDLE = static_cast<int>(0x80920008);
constexpr int PAD_UNKNOWN_N3KSX62FGNO_BEFORE_INIT = static_cast<int>(0x80920005);
constexpr int PAD_UNKNOWN_N3KSX62FGNO_AFTER_INIT = static_cast<int>(0x809200ff);

bool g_opened = false;
std::atomic_bool g_initialized{false};

bool ValidPort(int userId, int type, int index) {
    const bool personalPort = type == PAD_PORT_TYPE_STANDARD || type == PAD_PORT_TYPE_SPECIAL;
    const bool systemRemote = userId == PAD_USER_ID_SYSTEM && type == PAD_PORT_TYPE_REMOTE;
    return index == 0 && (personalPort || systemRemote);
}

}

struct PadUpdateData {
    std::size_t size;
    std::uint8_t data[32768];
};

static_assert(sizeof(PadUpdateData) == 32776);
static_assert(offsetof(PadUpdateData, data) == 8);

struct PadMotionSensorPosition {
    float x;
    float y;
    float z;
};

static_assert(sizeof(PadMotionSensorPosition) == 12);

extern "C" {

int APS5_VABI scePadClose_nid_postfix(int handle) {
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 g_opened = false;
 return PAD_OK;
}

int APS5_VABI scePadVrControllerSetVibration() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVrControllerSetVibrationMode() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVrControllerReadCombinedState() {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDeviceClassGetExtendedInformation(int handle, PadDeviceClassExtendedInformation* info) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (info == nullptr) return PAD_ERROR_INVALID_ARG;
 std::memset(info, 0, sizeof(*info));
 info->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 return PAD_OK;
}

int APS5_VABI scePadDeviceClassParseData(int handle, const PadData* data, PadDeviceClassData* class_data) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (data == nullptr || class_data == nullptr) return PAD_ERROR_INVALID_ARG;
 std::memset(class_data, 0, sizeof(*class_data));
 class_data->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 class_data->dataValid = data->connected;
 return PAD_OK;
}

int APS5_VABI scePadGetControllerInformation(int handle, PadControllerInformation* info) {
 if (handle != PAD_HANDLE) {
  return PAD_ERROR_INVALID_HANDLE;
 }
 if (info == nullptr) {
  return PAD_ERROR_INVALID_ARG;
 }
 std::memset(info, 0, sizeof(*info));
 info->touchPadInfo.pixelDensity = 44.86f;
 info->touchPadInfo.resolution.x = 1920;
 info->touchPadInfo.resolution.y = 943;
 info->stickInfo.deadZoneLeft = 2;
 info->stickInfo.deadZoneRight = 2;
 info->connectionType = PAD_CONNECTION_TYPE_LOCAL;
 info->connectedCount = 1;
 info->connected = true;
 info->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 return PAD_OK;
}

int APS5_VABI scePadGetHandle(int user_id, int type, int index) {
 if (!g_opened || !ValidPort(user_id, type, index) || type == PAD_PORT_TYPE_SPECIAL) return PAD_ERROR_DEVICE_NO_HANDLE;
 return PAD_HANDLE;
}

int APS5_VABI scePadGetInfo_nid_postfix(PadInfo* info) {
 if (info == nullptr) return PAD_ERROR_INVALID_ARG;
 std::memset(info, 0, sizeof(*info));
 info->maxConnectCount = 4;
 info->connectedCount[0] = 1;
 info->connectedCount[1] = 0;
 PadInfo::PadTypeInfo& slot = info->padTypeInfo[0];
 slot.connectPort = 0;
 slot.status = 1;
 slot.deviceType = PAD_DEVICE_TYPE_DUAL_SENSE;
 slot.connectType = PAD_CONNECT_TYPE_BLUETOOTH;
 std::memcpy(slot.bluetoothMacAddress, PAD_BD_ADDRESS, sizeof(slot.bluetoothMacAddress));
 return PAD_OK;
}

int APS5_VABI scePadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (info == nullptr) return PAD_ERROR_INVALID_ARG;
 std::memset(info, 0, sizeof(*info));
 return PAD_OK;
}

int APS5_VABI scePadGetExtControllerInformation(int handle, void* info) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (info == nullptr) return PAD_ERROR_INVALID_ARG;
 std::memset(info, 0, 0x2c);
 PadControllerInformation* base = static_cast<PadControllerInformation*>(info);
 base->touchPadInfo.pixelDensity = 44.86f;
 base->touchPadInfo.resolution.x = 1920;
 base->touchPadInfo.resolution.y = 943;
 base->stickInfo.deadZoneLeft = 2;
 base->stickInfo.deadZoneRight = 2;
 base->connectionType = PAD_CONNECTION_TYPE_LOCAL;
 base->connectedCount = 1;
 base->connected = true;
 base->deviceClass = PAD_DEVICE_CLASS_STANDARD;
 return PAD_OK;
}

int APS5_VABI scePadSetProcessPrivilege(int privilege) {
 (void)privilege;
 return PAD_OK;
}

int APS5_VABI scePadSetParticularMode(bool mode) {
 (void)mode;
 return PAD_OK;
}

int APS5_VABI scePadInit_nid_postfix(void) {
 Pad::Initialize();
 g_initialized.store(true, std::memory_order_release);
 return PAD_OK;
}

int APS5_VABI scePadOpen_nid_postfix(int userId, int type, int index, const void* param) {
 (void)param;
 if (!ValidPort(userId, type, index)) {
  return PAD_ERROR_INVALID_ARG;
 }
 if (type == PAD_PORT_TYPE_SPECIAL) return PAD_ERROR_DEVICE_NOT_CONNECTED;
 g_opened = true;
 return PAD_HANDLE;
}

int APS5_VABI scePadOpenExt(int userId, int type, int index, const void* param) {
 if (!ValidPort(userId, type, index) || param == nullptr) {
  return PAD_ERROR_INVALID_ARG;
 }
 if (type != PAD_PORT_TYPE_SPECIAL) NotImplemented_nid_no_patch(__func__);
 return PAD_ERROR_DEVICE_NOT_CONNECTED;
}

int APS5_VABI scePadReadExt() { NotImplemented_nid_no_patch(__func__); return 0; }

int APS5_VABI scePadGetFeatureReport() { NotImplemented_nid_no_patch(__func__); return 0; }

int APS5_VABI scePadSetFeatureReport() { NotImplemented_nid_no_patch(__func__); return 0; }

int APS5_VABI scePadOutputReport() { NotImplemented_nid_no_patch(__func__); return 0; }

int APS5_VABI scePadRead_nid_postfix(int handle, PadData* data, int num) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (data == nullptr || num <= 0) APS5_INVALID_ARG_EX;
 return Pad::Read(data, num);
}

int APS5_VABI scePadReadState(int handle, PadData* data) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (data == nullptr) APS5_INVALID_ARG_EX;

 *data = Pad::ReadState();

 return 0;
}

int APS5_VABI scePadResetLightBar(int handle) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 Pad::SetLightBar(false, 0, 0, 0);
 return PAD_OK;
}

int APS5_VABI scePadResetOrientation(int handle) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 Pad::ResetOrientation();
 return PAD_OK;
}

int APS5_VABI scePadSetAngularVelocityDeadbandState(int handle, bool enable) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 Pad::SetAngularVelocityDeadband(enable);
 return PAD_OK;
}

int APS5_VABI scePadSetLightBar(int handle, const PadLightBarParam* param) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (param == nullptr) return PAD_ERROR_INVALID_ARG;
 Pad::SetLightBar(true, param->r, param->g, param->b);
 return PAD_OK;
}

int APS5_VABI scePadSetMotionSensorState(int handle, bool enable) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 Pad::SetMotionEnabled(enable);
 return PAD_OK;
}

int APS5_VABI scePadSetTiltCorrectionState(int handle, bool enabled) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 Pad::SetTiltCorrection(enabled);
 return PAD_OK;
}

int APS5_VABI scePadSetTriggerEffect(int handle, const void* param) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (param == nullptr) return PAD_ERROR_INVALID_ARG;
 const std::uint8_t* bytes = static_cast<const std::uint8_t*>(param);
 const std::uint8_t mask = bytes[0];
 if ((mask & ~0x3u) != 0) {
  return PAD_ERROR_INVALID_ARG;
 }
 std::uint32_t mode[2] = {};
 for (int trigger = 0; trigger < 2; ++trigger) std::memcpy(&mode[trigger], bytes + 8 + 56 * trigger, 4);
 if (mode[0] > 6 || mode[1] > 6) {
  return PAD_ERROR_INVALID_ARG;
 }
 for (int trigger = 0; trigger < 2; ++trigger) {
  if ((mask & (1u << trigger)) != 0) Pad::SetTriggerCommand(trigger, bytes + 8 + 56 * trigger);
 }
 return PAD_OK;
}

int APS5_VABI scePadSetVibration(int handle, const PadVibrationParam* param) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (param == nullptr) return PAD_ERROR_INVALID_ARG;
 Pad::SetVibration(param->large_motor, param->small_motor);
 return PAD_OK;
}

int APS5_VABI scePadSetVibrationMode(int handle, int mode) {
 if (!Pad::IsInitialized()) return PAD_ERROR_NOT_INITIALIZED;
 if (mode <= 0) return PAD_ERROR_INVALID_ARG;
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (mode > 3 && mode != 5) return PAD_ERROR_INVALID_ARG;
 Pad::SetVibrationMode(mode);
 return PAD_OK;
}

int APS5_VABI scePadSetVibrationTriggerEffectWeakWhileEmbeddedMicInUse(bool enabled) {
 (void)enabled;
 return PAD_OK;
}

int APS5_VABI scePadVrControllerGetDeviceInformation() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadVrControllerRead() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadVrControllerGetTriggerEffectState() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadVrControllerReadState(int handle, void* state) {
 (void)handle;
 (void)state;
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadVrControllerSetTriggerEffect() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadVrControllerSetTriggerEffects() {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI scePadIsRemoteController(int handle, bool* remote) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (remote == nullptr) return PAD_ERROR_INVALID_ARG;
 *remote = false;
 return 0;
}


int APS5_VABI scePadSetAngularVelocityBiasCorrectionState(int handle, bool enabled) {
 if (handle != PAD_HANDLE) return PAD_ERROR_INVALID_HANDLE;
 if (enabled) NotImplemented_nid_no_patch(__func__);
 return PAD_OK;
}

APS5_EXPORT("n3kSX62fgNo", scePadUnknown_n3kSX62fgNo);
int APS5_VABI scePadUnknown_n3kSX62fgNo() {
    if (g_opened) NotImplemented_nid_no_patch(__func__);
    return g_initialized.load(std::memory_order_acquire) ? PAD_UNKNOWN_N3KSX62FGNO_AFTER_INIT : PAD_UNKNOWN_N3KSX62FGNO_BEFORE_INIT;
}

int APS5_VABI scePadVrControllerGetTriggerEffectStates(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetTriggerEffectExt(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadLastLotusButtonData(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadReadStateExt(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetExtensionReport(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetExtensionUnitInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnableExtensionPort(bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnableSpecificDeviceClass(std::int32_t deviceClass) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadOpenExt2(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDeviceOpen(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetDataInternal(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetUserColor(std::int32_t handle, std::uint32_t color) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableVibration(std::int32_t handle, bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetVibrationStrength(std::int32_t handle, std::uint32_t strength) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLightBarBaseBrightness(std::int32_t handle, std::uint32_t brightness) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLightBarBlinking(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

std::uint64_t APS5_VABI scePadGetIdleCount(std::int32_t userId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetControllerIdleCount(std::int32_t handle, std::uint64_t* count) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetPowerSavingState(std::int32_t handle, std::uint8_t state) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetAutoPowerOffCount(std::int32_t count) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnableUsbConnection(bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnableAutoDetect(bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnablePlayPauseEmulation(bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableTrackingLedBatterySaver(bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLoginUserNumber(std::int32_t number) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetCapability(std::int32_t deviceId, std::uint32_t* capability) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadShareOutputData(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadShareOutputDataWithFunctionType(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetVibrationForce(std::int32_t handle, const PadVibrationParam* param) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetVersionInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetControllerType(std::int32_t handle, std::int32_t* type) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableDataConversion(std::int32_t handle, bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetButtonRemappingInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetProcessPrivilegeOfButtonRemapping(bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetForceIntercepted(bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadIsLightBarBaseBrightnessControllable(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSwitchConnection(std::int32_t handle, std::uint32_t flags) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableTriggerEffect(std::int32_t handle, bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetTriggerEffectStrength(std::int32_t handle, std::uint32_t strength) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetTriggerEffectInternalState(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetUserNumber(std::int32_t handle, std::int32_t number) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableTransportButtons(bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLotusIrCodeSetStart(std::int32_t handle, std::uint8_t codeSet, std::uint16_t code, std::size_t dataSize) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLotusIrCodeSetData(std::int32_t handle, std::uint8_t packetIndex, const void* data, std::size_t dataSize) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLotusIrCodeSetEnd(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetLotusIrCodeSetResult(std::int32_t handle, std::int32_t* result, std::uint8_t* deviceResult) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLotusEnableIrKey(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLotusDisableIrKey(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetUsbDetectionState(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadUiReadState(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadUiReadStateForTuning(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetStickTriggerTuningMode(std::int32_t handle, std::uint32_t mode) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetStickTuningParam(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetTriggerTuningParam(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetProfileHunt(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetProfileHunt(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadResetHunt(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetProfileInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetActiveProfileInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableActiveProfileChange(std::int32_t handle, bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisableActiveProfileChangeByDeviceId(std::int32_t deviceId, bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetActiveProfileSlot(std::int32_t deviceId, std::uint32_t slot) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetSavedSettingInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetSavedSettingInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVrControllerGetPhysicalUsbConnectionStatus(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadParseUniqueDataLeonardo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetControllerTypeLeonardo(std::int32_t deviceId, std::int32_t* controllerType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetControllerTypeLeonardo(std::int32_t deviceId, std::int32_t controllerType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetProfileLeonardo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetProfileLeonardo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadResetLeonardo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetProfileEditMode(std::int32_t handle, bool enabled, std::uint32_t profile) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetProfileEditMode(std::int32_t handle, std::int32_t* mode) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetProfileEditModeLeonardo(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetSelectButtonCustomizeMode(std::int32_t handle, bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetSelectButtonCustomizeMode(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetToggleAllReleaseLeonardo(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnableControllerIndicator(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnableProfileIndicator(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetProfileIndicatorAnimation(std::int32_t handle, std::uint8_t animation) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadIsCoPilot(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetIsCoPilot(std::int32_t handle, bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadRegisterCollaboInputMgrProcess(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetTelemetryInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetActiveTime(std::int32_t handle, std::int32_t type, std::uint64_t* activeTime) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetDeviceId(std::int32_t handle, std::int32_t* deviceId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetInfoByPortType(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadResetLightBarAll(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadResetLightBarAllByPortType(std::uint32_t portType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetLightBarForTracker(std::int32_t handle, const PadLightBarParam* color) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadReadForTracker(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadReadBlasterForTracker(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadResetOrientationForTracker(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetMotionTimerUnit(std::int32_t handle, float* unit) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetVrTrackingMode(std::uint8_t mode) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetMotionSensorPosition(std::int32_t handle, PadMotionSensorPosition* position) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetSphereRadius(std::int32_t handle, float* radius) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

bool APS5_VABI scePadIsValidHandle(std::int32_t handle, std::int32_t portType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

bool APS5_VABI scePadIsDS4Connected(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

bool APS5_VABI scePadIsBlasterConnected(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

bool APS5_VABI scePadIsMoveConnected(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetBluetoothAddress(std::int32_t handle, std::uint64_t* address) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadIsMoveReproductionModel(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadReadAstonForTracker(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetAstonModel(std::int32_t handle) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadMbusInit(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadMbusTerm(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadConnectPort(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisconnectPort(std::int32_t userId, std::int32_t portType) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadDisconnectDevice(std::int32_t deviceId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetDeviceInfo(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetProcessFocus(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVertualDeviceAddDevice(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceAddDevice(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceDeleteDevice(std::int32_t deviceId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceInsertData(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceInsertTriggerInternalState(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceGetRemoteSetting(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceSetTriggerEffectSetting(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadVirtualDeviceDisableButtonRemapping(bool disabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetShareButtonMaskForRemotePlay(std::uint8_t mask) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetPS4BcVibrationMode(std::uint8_t mode) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetVirtualDeviceComponentInformation(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadEnablePs4CompatibleMode(std::int32_t handle, bool enabled) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetLicenseControllerInformation(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadReadHistory(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadStartRecording(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadStopRecording(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadRemoteControlInsertData(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadRemoteControlInsertDataForAston(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadGetFeatureReport2(std::int32_t handle, std::uint8_t reportId, void* data, std::size_t size) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetFeatureReport2(std::int32_t handle, std::uint8_t reportId, const void* data, std::size_t size) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadSetMuteLed(std::int32_t handle, std::uint32_t mode) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateInit(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateIsReady(std::int32_t deviceId, std::int32_t* ready) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateStart(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateGetStatus(std::int32_t deviceId, std::int32_t* status) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateSendData(std::int32_t deviceId, const PadUpdateData* data) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateResetBond(std::int32_t deviceId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateGetReconnectStatus(std::int32_t* status) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadBondUpdateTerm(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateInit(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateIsUpdateNeeded2(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateIsReady(std::int32_t deviceId, std::int32_t* ready) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateStart(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateGetStatus(std::int32_t deviceId, std::int32_t* status) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateSendData(std::int32_t deviceId, const PadUpdateData* data) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateResetController(std::int32_t deviceId) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateGetReconnectStatus(std::int32_t* status) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

int APS5_VABI scePadControllerUpdateTerm(void) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
