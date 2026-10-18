#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif
#include "prx/libc/include/Shutdown.hpp"
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libkernel/KernelErrors.hpp"
#include "prx/libSceSystemService/SystemService.hpp"

namespace {

int ConsoleLanguage() {
    const char* configured = std::getenv("ANYPS5_LANGUAGE");
    if (configured == nullptr || configured[0] == '\0') return SYSTEM_SERVICE_PARAM_LANG_ENGLISH_US;
    const std::string_view text(configured);
    int language = -1;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), language);
    if (error != std::errc{} || end != text.data() + text.size() || language < SYSTEM_SERVICE_PARAM_LANG_JAPANESE || language > SYSTEM_SERVICE_PARAM_LANG_UKRAINIAN) {
        throw std::runtime_error("sceSystemServiceParamGetInt: ANYPS5_LANGUAGE must be a console language number from " +
            std::to_string(SYSTEM_SERVICE_PARAM_LANG_JAPANESE) + " to " + std::to_string(SYSTEM_SERVICE_PARAM_LANG_UKRAINIAN) +
            ", not \"" + std::string(text) + "\"");
    }
    return language;
}

struct ConsoleTimeZone {
    int minutes;
    bool summer;
};

ConsoleTimeZone HostTimeZone() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    const bool converted = localtime_s(&local, &now) == 0;
#else
    const bool converted = localtime_r(&now, &local) != nullptr;
#endif
    if (!converted) throw std::runtime_error("sceSystemServiceParamGetInt: the host local time is unavailable");
    const std::chrono::sys_days date = std::chrono::year{local.tm_year + 1900} / std::chrono::month{static_cast<unsigned>(local.tm_mon + 1)} / std::chrono::day{static_cast<unsigned>(local.tm_mday)};
    const auto localSeconds = date.time_since_epoch() + std::chrono::hours{local.tm_hour} + std::chrono::minutes{local.tm_min} + std::chrono::seconds{local.tm_sec};
    const auto offset = std::chrono::duration_cast<std::chrono::minutes>(localSeconds - std::chrono::seconds{now});
    const bool summer = local.tm_isdst > 0;
#if defined(_WIN32)
    long daylightBias = 0;
    _get_dstbias(&daylightBias);
    const int summerMinutes = summer ? static_cast<int>(-daylightBias / 60) : 0;
#else
    const int summerMinutes = summer ? 60 : 0;
#endif
    return {static_cast<int>(offset.count()) - summerMinutes, summer};
}

constexpr char OwnExecutable[] = "/app0/eboot.bin";

void RelaunchHostExecutable() {
    std::fflush(nullptr);
#if defined(_WIN32)
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size()) {
        throw std::runtime_error("sceSystemServiceLoadExec: GetModuleFileNameW failed with error " + std::to_string(GetLastError()));
    }
    executable.resize(length);
    std::wstring commandLine = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("sceSystemServiceLoadExec: CreateProcessW failed with error " + std::to_string(GetLastError()));
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
#elif defined(__linux__)
    std::ifstream file("/proc/self/cmdline", std::ios::binary);
    const std::string commandLine((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<char*> argv;
    std::size_t start = 0;
    while (start < commandLine.size()) {
        argv.push_back(const_cast<char*>(commandLine.c_str() + start));
        const std::size_t end = commandLine.find('\0', start);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (argv.empty()) throw std::runtime_error("sceSystemServiceLoadExec: empty /proc/self/cmdline");
    argv.push_back(nullptr);
    ::execv("/proc/self/exe", argv.data());
    throw std::runtime_error("sceSystemServiceLoadExec: execv failed: " + std::string(std::strerror(errno)));
#else
    NotImplemented_nid_no_patch("sceSystemServiceLoadExec: relaunching on this host");
#endif
}

}

extern "C" {

int APS5_VABI sceSystemServiceLoadExec(const char* path, const char* const* arguments) {
    if (!path || !*path) return SYSTEM_SERVICE_ERROR_PARAMETER;
    if (std::strcmp(path, "exit") == 0) LibcExitWithoutHandlers_nid_no_patch(0);
    if (std::strcmp(path, OwnExecutable) != 0) {
        NotImplemented_nid_no_patch("sceSystemServiceLoadExec: executable replacement");
    }
    if (arguments != nullptr && arguments[0] != nullptr) {
        NotImplemented_nid_no_patch("sceSystemServiceLoadExec: arguments for the relaunched executable");
    }
    LibcRunShutdown_nid_postfix();
    RelaunchHostExecutable();
    std::exit(0);
}

int APS5_VABI sceSystemServiceDisableNoticeScreenSkipFlagAutoSet(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetDisplaySafeAreaInfo(SystemServiceDisplaySafeAreaInfo* info) {
 if (info == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 *info = SystemServiceDisplaySafeAreaInfo{};
 info->ratio = 1.0f;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceShowDisplaySafeAreaSettings(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetHdrToneMapLuminance(SystemServiceHdrToneMapLuminance* luminance) {
 if (luminance == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 constexpr float SdrReferenceWhiteNits = 100.0f;
 luminance->max_full_frame_tone_map_luminance = SdrReferenceWhiteNits;
 luminance->max_tone_map_luminance = SdrReferenceWhiteNits;
 luminance->min_tone_map_luminance = 0.0f;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetNoticeScreenSkipFlag(bool* value) {
 if (value == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 *value = false;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetStatus(SystemServiceStatus* status) {
 if (status == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 *status = SystemServiceStatus{};
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceHideSplashScreen(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceParamGetInt(int paramId, int* value) {
 if (value == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 switch (paramId) {
  case SYSTEM_SERVICE_PARAM_ID_LANG:
  case SYSTEM_SERVICE_PARAM_ID_400: *value = ConsoleLanguage(); break;
  case SYSTEM_SERVICE_PARAM_ID_DATE_FORMAT: *value = SYSTEM_SERVICE_PARAM_DATE_FORMAT_DDMMYYYY; break;
  case SYSTEM_SERVICE_PARAM_ID_TIME_FORMAT: *value = SYSTEM_SERVICE_PARAM_TIME_FORMAT_24HOUR; break;
  case SYSTEM_SERVICE_PARAM_ID_TIME_ZONE: *value = HostTimeZone().minutes; break;
  case SYSTEM_SERVICE_PARAM_ID_SUMMERTIME: *value = HostTimeZone().summer ? 1 : 0; break;
  case SYSTEM_SERVICE_PARAM_ID_GAME_PARENTAL_LEVEL: *value = SYSTEM_SERVICE_PARAM_GAME_PARENTAL_OFF; break;
  case SYSTEM_SERVICE_PARAM_ID_ENTER_BUTTON_ASSIGN: *value = SYSTEM_SERVICE_PARAM_ENTER_BUTTON_CROSS; break;
  default: *value = 0; break;
 }
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceParamGetString(int paramId, char* buf, size_t bufSize) {
 if (buf == nullptr || bufSize == 0) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 if (paramId != SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME) {
  NotImplemented_nid_no_patch("sceSystemServiceParamGetString: parameter other than the system name");
 }
 if (bufSize < SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH) {
  NotImplemented_nid_no_patch("sceSystemServiceParamGetString: buffer shorter than 65 bytes");
 }
 constexpr char SystemName[] = "PS5";
 std::memcpy(buf, SystemName, sizeof(SystemName));
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServicePowerTick(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceReceiveEvent(SystemServiceEvent* event) {
 if (event == nullptr) {
  return SYSTEM_SERVICE_ERROR_PARAMETER;
 }
 return SYSTEM_SERVICE_ERROR_NO_EVENT;
}

int APS5_VABI sceSystemServiceReportAbnormalTermination(const void* info) {
 (void)info;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceSetNoticeScreenSkipFlag(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceInitializePlayerDialogParam(void* param) {
 if (param == nullptr) return SYSTEM_SERVICE_ERROR_PARAMETER;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceDisableMediaPlay() {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceReenableMediaPlay() {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceLaunchPlayerDialog(const void* param) {
 if (param == nullptr) return SYSTEM_SERVICE_ERROR_PARAMETER;
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceLaunchWebBrowser(const char* uri, void* param) {
 (void)uri;
 (void)param;
 return SCE_KERNEL_ERROR_EOPNOTSUPP;
}

int APS5_VABI sceSystemServiceLaunchApp(const char* titleId, const char* const* arguments, const void* param) {
 (void)titleId;
 (void)arguments;
 (void)param;
 NotImplemented_nid_no_patch("sceSystemServiceLaunchApp: launching another application");
 return 0;
}

int APS5_VABI sceSystemServiceDisableMusicPlayer(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceOpenChallengeActivity(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceOpenTournamentOccurrence(void) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSystemServiceAddLocalProcessForPsmKit(const char*, const char* const*, const void*) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSystemServiceKillLocalProcessForPsmKit(int) {
 NotImplemented_nid_no_patch(__func__);
 return 0;
}

int APS5_VABI sceSystemServiceReenableMusicPlayer(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceShowControllerSettings(void) {
 return SYSTEM_SERVICE_OK;
}

int APS5_VABI sceSystemServiceGetAppIdOfRunningBigApp(void) {
 return SYSTEM_SERVICE_RUNNING_APP_ID;
}

int APS5_VABI sceSystemServiceKillApp(int appId, int how, int reason, int coreDump) {
 if (appId != SYSTEM_SERVICE_RUNNING_APP_ID) {
  NotImplemented_nid_no_patch("sceSystemServiceKillApp: application other than the running title");
 }
 if (how != -1 || reason != 0 || coreDump != 0) {
  NotImplemented_nid_no_patch("sceSystemServiceKillApp: arguments other than -1, 0 and 0");
 }
 LibcExit_nid_no_patch(0);
}

}
