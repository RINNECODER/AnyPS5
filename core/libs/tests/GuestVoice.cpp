#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdlib>
#include <stdexcept>

extern "C" {
int APS5_VABI sceVoiceInit(VoiceInitParam*, std::int32_t);
int APS5_VABI sceVoiceStart(const VoiceStartParam*);
int APS5_VABI sceVoiceEnd_nid_postfix();
int APS5_VABI sceVoiceCreatePort(std::uint32_t*, const VoicePortParam*);
int APS5_VABI sceVoiceDeletePort(std::uint32_t);
int APS5_VABI sceVoiceConnectIPortToOPort(std::uint32_t, std::uint32_t);
int APS5_VABI sceVoiceDisconnectIPortFromOPort(std::uint32_t, std::uint32_t);
int APS5_VABI sceVoiceGetVolume(std::uint32_t, float*);
int APS5_VABI sceVoiceSetMuteFlag(std::uint32_t, bool);
int APS5_VABI sceVoiceGetMuteFlag(std::uint32_t, bool*);
int APS5_VABI sceVoiceGetPortInfo(std::uint32_t, VoicePortInfo*);
int APS5_VABI sceVoiceReadFromOPort(std::uint32_t, void*, std::uint32_t*);
int APS5_VABI sceVoiceResetPort(std::uint32_t);
int APS5_VABI sceVoiceGetResourceInfo(VoiceResourceInfo*);
int APS5_VABI sceVoiceEnableChat(void);
int APS5_VABI sceVoiceDisableChat(void);
int APS5_VABI sceVoiceInitHQ(const VoiceInitParam*, std::int32_t);
int APS5_VABI sceVoiceVADSetVersion(std::int32_t);
}

namespace {

void Require(bool value) { if (!value) std::abort(); }

template <typename TFunction>
void RequireThrows(TFunction function) {
    try { function(); } catch (const std::logic_error&) { return; }
    std::abort();
}

std::uint32_t CreatePort(std::int32_t type, float volume) {
    VoicePortParam param{};
    param.port_type = type;
    param.volume = volume;
    if (type == 2 || type == 4) param.voice.bitrate = 16000;
    std::uint32_t id = 0;
    Require(sceVoiceCreatePort(&id, &param) == 0);
    return id;
}

} // namespace

int main() {
    RequireThrows([] { sceVoiceResetPort(0); });
    RequireThrows([] { VoiceResourceInfo info{}; sceVoiceGetResourceInfo(&info); });
    RequireThrows([] { sceVoiceEnableChat(); });
    RequireThrows([] { sceVoiceDisableChat(); });
    VoiceInitParam init{};
    VoiceStartParam start{};
    Require(sceVoiceInit(&init, 0) == 0);
    Require(sceVoiceStart(&start) == 0);
    RequireThrows([] { sceVoiceGetResourceInfo(nullptr); });
    VoiceResourceInfo resourceInfo{};
    Require(sceVoiceGetResourceInfo(&resourceInfo) == 0);
    Require(resourceInfo.max_voice_input_ports == 7);
    Require(resourceInfo.max_voice_output_ports == 7);
    Require(resourceInfo.max_device_input_ports == 1);
    Require(resourceInfo.max_device_output_ports == 1);
    Require(resourceInfo.max_ports == 64);
    Require(sceVoiceEnableChat() == 0);
    Require(sceVoiceDisableChat() == 0);
    const auto input = CreatePort(2, 0.5f);
    const auto output = CreatePort(3, 0.25f);
    Require(sceVoiceConnectIPortToOPort(input, output) == 0);
    Require(sceVoiceSetMuteFlag(output, true) == 0);
    VoicePortInfo before{};
    Require(sceVoiceGetPortInfo(input, &before) == 0);

    Require(sceVoiceResetPort(input) == 0);
    Require(sceVoiceResetPort(output) == 0);
    RequireThrows([&] { sceVoiceResetPort(output + 1); });

    VoicePortInfo after{};
    Require(sceVoiceGetPortInfo(input, &after) == 0);
    Require(after.port_type == before.port_type && after.state == before.state && after.frame_size == before.frame_size);
    float volume = 0;
    bool muted = false;
    Require(sceVoiceGetVolume(output, &volume) == 0 && volume == 0.25f);
    Require(sceVoiceGetMuteFlag(output, &muted) == 0 && muted);
    std::uint8_t data[64]{};
    std::uint32_t size = sizeof(data);
    Require(sceVoiceReadFromOPort(output, data, &size) == 0 && size == 0);
    Require(sceVoiceDisconnectIPortFromOPort(input, output) == 0);

    Require(sceVoiceGetMuteFlag(input, &muted) == 0 && !muted);
    Require(sceVoiceSetMuteFlagAll(true) == 0);
    Require(sceVoiceGetMuteFlag(input, &muted) == 0 && muted);
    Require(sceVoiceSetMuteFlagAll(false) == 0);
    Require(sceVoiceGetMuteFlag(input, &muted) == 0 && !muted);
    Require(sceVoiceGetMuteFlag(output, &muted) == 0 && muted);

    std::uint32_t portBitrate = 0;
    Require(sceVoiceSetBitRate(input, 7300) == 0);
    Require(sceVoiceGetBitRate(input, &portBitrate) == 0 && portBitrate == 7300);
    VoicePortInfo rateInfo{};
    Require(sceVoiceGetPortInfo(input, &rateInfo) == 0 && rateInfo.frame_size == 19);
    Require(sceVoiceSetBitRate(output + 1, 7300) == portNotFound);
    Require(sceVoiceSetBitRate(128, 7300) == invalidPort);
    RequireThrows([&] { sceVoiceSetBitRate(output, 7300); });
    RequireThrows([&] { sceVoiceSetBitRate(input, 7301); });
    RequireThrows([&] { sceVoiceSetBitRate(input, 0x80000000u); });
    Require(sceVoiceGetBitRate(input, &portBitrate) == 0 && portBitrate == 7300);

    VoicePortParam update{};
    update.port_type = 2;
    update.mute = 1;
    update.volume = 0.75f;
    update.voice.bitrate = 14400;
    Require(sceVoiceUpdatePort(input, &update) == 0);
    Require(sceVoiceGetMuteFlag(input, &muted) == 0 && muted);
    Require(sceVoiceGetVolume(input, &volume) == 0 && volume == 0.75f);
    Require(sceVoiceGetBitRate(input, &portBitrate) == 0 && portBitrate == 14400);
    Require(sceVoiceGetPortInfo(input, &rateInfo) == 0 && rateInfo.frame_size == 27);
    update.voice.bitrate = 1;
    RequireThrows([&] { sceVoiceUpdatePort(input, &update); });
    Require(sceVoiceGetBitRate(input, &portBitrate) == 0 && portBitrate == 14400);
    update.voice.bitrate = 14400;
    update.port_type = 3;
    RequireThrows([&] { sceVoiceUpdatePort(input, &update); });
    RequireThrows([&] { sceVoiceUpdatePort(input, nullptr); });
    Require(sceVoiceUpdatePort(output + 1, &update) == portNotFound);
    Require(sceVoiceUpdatePort(128, &update) == invalidPort);
    VoicePortParam pcm{};
    pcm.port_type = 3;
    pcm.volume = std::numeric_limits<float>::quiet_NaN();
    Require(sceVoiceUpdatePort(output, &pcm) == 0);
    Require(sceVoiceGetMuteFlag(output, &muted) == 0 && !muted);
    Require(sceVoiceGetVolume(output, &volume) == 0 && volume == 0.0f);
    pcm.volume = 0.25f;
    pcm.mute = 1;
    Require(sceVoiceUpdatePort(output, &pcm) == 0);

    Require(sceVoiceSetMuteFlagAll(true) == 0);

    Require(sceVoiceDeletePort(input) == 0);
    RequireThrows([&] { sceVoiceResetPort(input); });
    Require(sceVoiceResetPort(output) == 0);
    Require(sceVoiceEnd_nid_postfix() == 0);
    TestNotInitialized();
    Require(sceVoiceResetPort(output) == notInitialized);
    Require(sceVoiceResumePort(output) == notInitialized);
    Require(sceVoiceGetResourceInfo(nullptr) == notInitialized);
    Require(sceVoiceEnableChat() == notInitialized);
    Require(sceVoiceDisableChat() == notInitialized);
    for (const auto mode : {4, 0x08000000, 0x10000000, 0x20000000}) {
        init.app_type = mode;
        Require(sceVoiceInit(&init, 100) == 0);
        const std::uint32_t memorySize = mode == 4 || mode == 0x08000000 ? 0xc0000 : 0x40000;
        start.mem_size = memorySize - 1;
        Require(sceVoiceStart(&start) == static_cast<std::int32_t>(0x804e0806u));
        start.mem_size = memorySize + 1;
        Require(sceVoiceStart(&start) == static_cast<std::int32_t>(0x804e0806u));
        start.mem_size = memorySize;
        Require(sceVoiceStart(&start) == 0);
        Require(sceVoiceStop() == 0);
        Require(sceVoiceStop() == 0);
        Require(sceVoiceSetThreadsParams(&threads) == 0);
        VoicePortParam portParam{};
        portParam.port_type = 0;
        std::uint32_t port = UINT32_MAX;
        Require(sceVoiceCreatePort(&port, &portParam) == 0 && port == 0);
        Require(sceVoiceGetResourceInfo(&resourceOutput.info) == 0);
        Require(resourceOutput.info.maxVoiceInputPorts == (mode == 4 ? 0 : mode == 0x20000000 ? 7 : 15));
        Require(resourceOutput.info.maxVoiceOutputPorts == (mode == 4 ? 0 : 4));
        Require(resourceOutput.info.maxDeviceInputPorts == 4);
        Require(resourceOutput.info.maxDeviceOutputPorts == (mode == 4 ? 5 : 4));
        Require(resourceOutput.guard == 0xa5a5a5a5u);
        Require(sceVoiceEnd_nid_postfix() == 0);
    }

    constexpr auto resourceFull = static_cast<std::int32_t>(0x804e0808u);
    std::uint32_t portId = 123;
    VoicePortParam param{};
    Require(sceVoiceCreatePort(nullptr, nullptr) == notInitialized);
    Require(sceVoiceCreatePort(&portId, &param) == notInitialized && portId == 123);
    for (const auto mode : {4, 0x08000000, 0x10000000, 0x20000000}) {
        init.app_type = mode;
        Require(sceVoiceInit(&init, 100) == 0);
        Require(sceVoiceCreatePort(&portId, nullptr) == invalidArgument && portId == 123);
        Require(sceVoiceCreatePort(nullptr, &param) == invalidArgument);
        for (const auto type : {-1, 6, INT32_MAX}) {
            param = {};
            param.port_type = type;
            Require(sceVoiceCreatePort(&portId, &param) == invalidArgument && portId == 255);
        }
        for (const auto type : {0, 2, 4, 5}) {
            param = {};
            param.port_type = type;
            if (type == 2 || type == 4) param.voice.bitrate = 16000;
            if (type == 5) param.device.type = 14;
            const auto limit = type == 0 ? 4 : type == 5 ? (mode == 4 ? 5 : 4) : mode == 4 ? 0 : type == 4 ? 4 : mode == 0x20000000 ? 7 : 15;
            std::vector<std::uint32_t> ports;
            for (int i = 0; i < limit; ++i) {
                Require(sceVoiceCreatePort(&portId, &param) == 0 && portId < 128);
                ports.push_back(portId);
            }
            Require(sceVoiceCreatePort(&portId, &param) == resourceFull && portId == 255);
            if (type == 0 || type == 5) {
                param.device.type = 1;
                Require(sceVoiceCreatePort(&portId, &param) == resourceFull);
                param.device.type = type == 0 ? 0 : 2;
            }
            for (auto id : ports) Require(sceVoiceDeletePort(id) == 0);
            if (limit != 0) {
                Require(sceVoiceCreatePort(&portId, &param) == 0);
                Require(sceVoiceDeletePort(portId) == 0);
            }
        }
        param = {};
        param.port_type = 3;
        param.pcmaudio.sample_rate = 16000;
        param.pcmaudio.buffer_size = 1024;
        const auto limit = mode == 4 ? 128 : mode == 0x08000000 ? 30 : mode == 0x10000000 ? 10 : 14;
        std::vector<std::uint32_t> ports;
        for (int i = 0; i < limit; ++i) {
            Require(sceVoiceCreatePort(&portId, &param) == 0 && portId < 128);
            for (auto id : ports) Require(id != portId);
            ports.push_back(portId);
        }
        Require(sceVoiceCreatePort(&portId, &param) == resourceFull && portId == 255);
        const auto released = ports.front();
        Require(sceVoiceDeletePort(released) == 0);
        Require(sceVoiceCreatePort(&portId, &param) == 0);
        if (mode == 4) Require(portId == released);
        Require(sceVoiceEnd_nid_postfix() == 0);
        portId = 123;
    }
    init.app_type = 0x08000000;
    Require(sceVoiceInit(&init, 100) == 0);
    for (const auto type : {0, 5}) {
        param = {};
        param.port_type = type;
        for (const auto deviceType : {-1, 0, 1, 2, 3, 13, 14, 15, INT32_MAX}) {
            param.device.type = deviceType;
            const bool valid = type == 0 ? deviceType == 0 : deviceType == 2 || deviceType == 14;
            Require(sceVoiceCreatePort(&portId, &param) == (valid ? 0 : invalidArgument));
            if (valid) Require(sceVoiceDeletePort(portId) == 0);
            else Require(portId == 255);
        }
    }
    for (const auto type : {1, 3}) {
        param = {};
        param.port_type = type;
        param.pcmaudio.buffer_size = 1024;
        Require(sceVoiceCreatePort(&portId, &param) == invalidArgument);
        param.pcmaudio.sample_rate = 16000;
        param.pcmaudio.data_type = 1;
        Require(sceVoiceCreatePort(&portId, &param) == invalidArgument);
        param.pcmaudio.data_type = 0;
        for (const std::uint32_t bytes : {0u, 511u, 512u, 1023u, 1024u, 1535u, 64512u, 65023u, 65024u, 65535u, 65536u, 66560u, UINT32_MAX}) {
            param.pcmaudio.buffer_size = bytes;
            const auto rounded = static_cast<std::uint16_t>(bytes - bytes % 512);
            const bool valid = rounded >= 1024 && rounded <= 64512;
            Require(sceVoiceCreatePort(&portId, &param) == (valid ? 0 : invalidArgument));
            if (valid) Require(sceVoiceDeletePort(portId) == 0);
        }
        param.pcmaudio.buffer_size = 1024;
        param.threshold = 33;
        Require(sceVoiceCreatePort(&portId, &param) == (type == 1 ? resourceFull : 0));
        if (type == 3) Require(sceVoiceDeletePort(portId) == 0);
        param.threshold = 32;
        Require(sceVoiceCreatePort(&portId, &param) == 0);
        Require(sceVoiceDeletePort(portId) == 0);
    }
    for (const auto type : {2, 4}) {
        param = {};
        param.port_type = type;
        for (const auto bitrate : {-1, 0, 3849, 3850, 4650, 5700, 7300, 11800, 12200, 14400, 16000, 16001, 22533, 22534}) {
            param.voice.bitrate = bitrate;
            const bool valid = bitrate == 3850 || bitrate == 4650 || bitrate == 5700 || bitrate == 7300 || bitrate == 11800 || bitrate == 12200 || bitrate == 14400 || bitrate == 16000 || bitrate == 22533;
            Require(sceVoiceCreatePort(&portId, &param) == (valid ? 0 : invalidArgument));
            if (valid) Require(sceVoiceDeletePort(portId) == 0);
        }
        param.voice.bitrate = 16000;
        param.threshold = 1021;
        Require(sceVoiceCreatePort(&portId, &param) == (type == 2 ? resourceFull : 0));
        if (type == 4) Require(sceVoiceDeletePort(portId) == 0);
        param.threshold = 1020;
        param.mute = 1;
        Require(sceVoiceCreatePort(&portId, &param) == 0);
        Require(sceVoiceGetMuteFlag(portId, &muted) == 0 && muted);
        Require(sceVoiceDeletePort(portId) == 0);
    }
    Require(sceVoiceGetVolume(128, nullptr) == invalidPort);
    Require(sceVoiceGetBitRate(128, nullptr) == invalidPort);
    Require(sceVoiceGetMuteFlag(128, nullptr) == invalidPort);
    Require(sceVoiceSetVolume(128, 1.0f) == invalidPort);
    Require(sceVoiceSetMuteFlag(128, 1) == invalidPort);
    Require(sceVoiceDeletePort(128) == invalidPort);
    Require(sceVoiceGetVolume(127, nullptr) == invalidArgument);
    Require(sceVoiceGetBitRate(127, nullptr) == invalidArgument);
    Require(sceVoiceGetMuteFlag(127, nullptr) == invalidArgument);
    volume = 123.0f;
    std::uint32_t bitrate = 123;
    muted = true;
    Require(sceVoiceGetVolume(127, &volume) == portNotFound && volume == 123.0f);
    Require(sceVoiceGetBitRate(127, &bitrate) == portNotFound && bitrate == 123);
    Require(sceVoiceGetMuteFlag(127, &muted) == portNotFound && muted);
    Require(sceVoiceSetVolume(127, 1.0f) == portNotFound);
    Require(sceVoiceSetMuteFlag(127, 1) == portNotFound);
    Require(sceVoiceDeletePort(127) == portNotFound);
    for (const auto type : {0, 1, 2, 3, 4, 5}) {
        const auto id = CreatePort(type, -2.0f);
        Require(sceVoiceGetBitRate(id, &bitrate) == (type == 2 || type == 4 ? 0 : portNotFound));
        if (type == 2 || type == 4) Require(bitrate == 16000);
        for (const float value : {-2.0f, -0.0f, 0.25f, 2.0f, std::numeric_limits<float>::max(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            Require(sceVoiceSetVolume(id, value) == 0);
            Require(sceVoiceGetVolume(id, &volume) == 0);
            Require(volume == (std::isfinite(value) ? value : 0.0f));
            if (value == 0.0f) Require(std::signbit(volume) == std::signbit(value));
        }
        for (const std::uint8_t value : {0, 1, 2, 128, 255}) {
            Require(sceVoiceSetMuteFlag(id, value) == 0);
            Require(sceVoiceGetMuteFlag(id, &muted) == 0 && muted == (value != 0));
        }
        Require(sceVoiceDeletePort(id) == 0);
    }
    Require(sceVoiceEnd_nid_postfix() == 0);

    Require(sceVoiceVADSetVersion(0) == 0 && sceVoiceVADSetVersion(1) == 0);
    Require(sceVoiceVADSetVersion(2) == invalidArgument && sceVoiceVADSetVersion(-1) == invalidArgument);
    init.app_type = 0x10000000;
    start.mem_size = 0x40000;
    Require(sceVoiceInitHQ(nullptr, 100) == invalidArgument && sceVoiceInitHQ(&init, 0) == invalidArgument);
    Require(sceVoiceInitHQ(&init, 100) == 0);
    Require(sceVoiceInitHQ(&init, 100) == AlreadyInitialized);
    Require(sceVoiceInit(&init, 100) == AlreadyInitialized);
    Require(sceVoiceStart(&start) == 0);
    Require(sceVoiceEnd_nid_postfix() == 0);
    Require(sceVoiceInit(&init, 100) == 0);
    Require(sceVoiceInitHQ(&init, 100) == AlreadyInitialized);
    Require(sceVoiceEnd_nid_postfix() == 0);
}
