#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

extern "C" {
int APS5_VABI sceAudioOut2Initialize();
int APS5_VABI sceAudioOut2LoInit();
int APS5_VABI sceAudioOut2LoTerminate();
int APS5_VABI sceAudioOut2Set3DLatency(int, std::uint32_t);
int APS5_VABI sceAudioOut2MasteringInit(std::uint32_t);
int APS5_VABI sceAudioOut2MasteringTerm();
int APS5_VABI sceAudioOut2MasteringSetParam(const void*, std::uint32_t, std::uint32_t);
int APS5_VABI sceAudioOut2EnableChat();
int APS5_VABI sceAudioOut2DisableChat();
}

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

template<typename TFunction>
static bool ThrowsRuntimeError(TFunction function) {
    try {
        function();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

namespace {

constexpr int systemUser = 0xFF;
constexpr int user = 0x10000000;
constexpr int errorInvalidParam = static_cast<int>(0x80268001);

void TestSet3DLatency() {
    Require(sceAudioOut2Set3DLatency(systemUser, 0) == errorInvalidParam, "output 0 for selector 255 must be refused");
    for (std::uint32_t output = 0; output <= 2; ++output) {
        Require(sceAudioOut2Set3DLatency(0, output) == 0, "selector 0 must accept each supported output");
    }
    Require(sceAudioOut2Set3DLatency(systemUser, 1) == 0, "output 1 for the system user must be accepted");
    Require(sceAudioOut2Set3DLatency(systemUser, 2) == 0, "output 2 for the system user must be accepted");
    Require(sceAudioOut2Set3DLatency(systemUser, 2) == 0, "output 2 must be accepted again");
    Require(sceAudioOut2Set3DLatency(user, 1) == errorInvalidParam, "a user id is not a valid latency selector");
    Require(sceAudioOut2Set3DLatency(systemUser, 3) == errorInvalidParam, "output 3 must be refused");
    Require(sceAudioOut2Set3DLatency(systemUser, 0xFFFFFFFFu) == errorInvalidParam, "a large output must be refused");
}

void TestMasteringInit() {
    Require(sceAudioOut2MasteringInit(0) == 0, "flags 0 must be accepted");
    Require(ThrowsRuntimeError([] { sceAudioOut2MasteringInit(1); }), "flags 1 must throw");
}

void TestMasteringTerm() {
    Require(sceAudioOut2MasteringInit(0) == 0, "a second initialization must be accepted");
    Require(sceAudioOut2MasteringTerm() == 0, "termination must be accepted");
    Require(sceAudioOut2MasteringTerm() == 0, "termination must be accepted for each initialization");
}

void TestMasteringSetParam() {
    const std::uint32_t params[4] = {1u, 0u, 0u, 0u};
    Require(sceAudioOut2MasteringSetParam(params, 0, 0) == 0, "mastering parameters must be accepted");
    Require(ThrowsRuntimeError([] { sceAudioOut2MasteringSetParam(nullptr, 0, 0); }), "null mastering parameters must throw");
}

void TestEnableChat() {
    Require(sceAudioOut2EnableChat() == 0, "enabling chat must be accepted");
    Require(sceAudioOut2EnableChat() == 0, "enabling chat again must be accepted");
    Require(sceAudioOut2DisableChat() == 0, "disabling chat must be accepted");
    Require(sceAudioOut2DisableChat() == 0, "disabling chat again must be accepted");
    Require(sceAudioOut2EnableChat() == 0, "enabling chat after disabling it must be accepted");
}

void TestLowLatencyService() {
    Require(sceAudioOut2LoInit() == 0, "starting the low-latency service must be accepted");
    Require(sceAudioOut2LoInit() == 0, "starting the low-latency service again must be accepted");
    Require(ThrowsRuntimeError([] { sceAudioOut2LoTerminate(); }), "stopping the low-latency service must throw");
}

}

int main() {
    Require(sceAudioOut2Initialize() == 0, "initialization must succeed");
    TestSet3DLatency();
    TestMasteringInit();
    TestMasteringTerm();
    TestMasteringSetParam();
    TestEnableChat();
    TestLowLatencyService();
    return 0;
}
