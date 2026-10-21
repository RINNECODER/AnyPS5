#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceUserService/UserService.hpp"
#include <cstring>
#include <mutex>

static std::mutex g_mutex;
static bool g_initialized = false;
static constexpr int ErrorInvalidArgument = static_cast<int>(0x80A00002);
static constexpr int ErrorAlreadyInitialized = static_cast<int>(0x80A00003);
static constexpr int ErrorNotInitialized = static_cast<int>(0x80A00004);

struct GameLiveStreamingProgramInfo;

struct GameLiveStreamingStatus2 {
    std::int32_t userId;
    bool isOnAir;
    std::uint8_t align[3];
    std::uint32_t spectatorCounts;
    std::uint32_t textMessageCounts;
    std::uint32_t commandMessageCounts;
    std::uint32_t broadcastVideoResolution;
    std::uint8_t reserved[48];
};
static_assert(sizeof(GameLiveStreamingStatus2) == 72);

extern "C" {

int APS5_VABI sceGameLiveStreamingInitialize(size_t heapSize) {
    if (heapSize != 0x4000) return ErrorInvalidArgument;
    std::lock_guard lock(g_mutex);
    if (g_initialized) return ErrorAlreadyInitialized;
    g_initialized = true;
    return 0;
}

int APS5_VABI sceGameLiveStreamingTerminate(void) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    g_initialized = false;
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetCurrentStatus2(GameLiveStreamingStatus2* status) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) return ErrorNotInitialized;
    if (status == nullptr) return ErrorInvalidArgument;
    std::memset(status, 0, sizeof(*status));
    status->userId = USER_SERVICE_USER_ID_INVALID;
    return 0;
}

int APS5_VABI sceGameLiveStreamingGetProgramInfo(GameLiveStreamingProgramInfo* info) {
    NotImplemented_nid_no_patch(__func__);
    return 0;
}

}
