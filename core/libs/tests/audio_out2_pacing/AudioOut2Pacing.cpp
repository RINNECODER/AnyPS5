// Fork-only: AudioOut2 output pacing, with fake guests (no title audio).
//
// Fake guest mixers push grains of a constant signal using the reference title's context shape
// (512-sample grains, queue depth 4). SDL's disk driver plays the output in real time into a file,
// and every run of silence between the first and the last audible sample is a dropout the user
// would hear.
//
// - A mixer that pushes blocking in a loop, steadily or with stalls such as a guest thread gets on a
//   loaded host (Rosetta, the GPU worker, other processes).
// - An event-driven sink shaped like the reference title's: a renderer thread renders grains only
//   while GetQueueLevel reports room and hands each to a push thread, which pushes it blocking and
//   then wakes the renderer. Otherwise only a slow game frame loop wakes the renderer.
#include "SceTypes.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int APS5_VABI sceAudioOut2ContextResetParam(AudioOut2ContextParam*);
int APS5_VABI sceAudioOut2ContextCreate(const AudioOut2ContextParam*, void*, std::size_t, AudioOut2ContextHandle*);
int APS5_VABI sceAudioOut2ContextDestroy(AudioOut2ContextHandle);
int APS5_VABI sceAudioOut2ContextPush(AudioOut2ContextHandle, std::uint32_t);
int APS5_VABI sceAudioOut2ContextGetQueueLevel(AudioOut2ContextHandle, std::uint32_t*, std::uint32_t*);
int APS5_VABI sceAudioOut2PortCreate(AudioOut2ContextHandle, const AudioOut2PortParam*, AudioOut2PortHandle*);
int APS5_VABI sceAudioOut2PortDestroy(AudioOut2PortHandle);
int APS5_VABI sceAudioOut2PortSetAttributes(AudioOut2PortHandle, const AudioOut2Attribute*, std::uint32_t);
}

namespace {

constexpr std::uint32_t grain = 512;
constexpr std::uint32_t queueDepth = 4;
constexpr std::uint32_t frequency = 48000;
constexpr std::uint32_t formatStereoFloat = 2u << 8;
constexpr std::uint32_t attributeData = 0;
// A silent stretch shorter than this inside the signal is not counted (none should occur at all).
constexpr std::size_t minimumGapFrames = 32;

int failures = 0;

void Check(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "audio_out2_pacing: FAILED: %s\n", what);
    failures++;
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

struct Dropout {
    double at;
    double seconds;
};

// The silent stretches between the first and the last audible frame of the played file.
std::vector<Dropout> Dropouts(const std::vector<float>& played) {
    const std::size_t frames = played.size() / 2;
    const auto audible = [&](std::size_t frame) { return played[frame * 2] != 0.0f || played[frame * 2 + 1] != 0.0f; };
    std::size_t first = 0;
    while (first < frames && !audible(first)) first++;
    std::size_t last = frames;
    while (last > first && !audible(last - 1)) last--;
    std::vector<Dropout> dropouts;
    for (std::size_t frame = first; frame < last;) {
        if (audible(frame)) {
            frame++;
            continue;
        }
        const auto start = frame;
        while (frame < last && !audible(frame)) frame++;
        if (frame - start >= minimumGapFrames) {
            dropouts.push_back({static_cast<double>(start - first) / frequency, static_cast<double>(frame - start) / frequency});
        }
    }
    return dropouts;
}

// One context with one stereo port, played by SDL's disk driver into a file.
struct Session {
    const char* name;
    std::filesystem::path path;
    AudioOut2ContextHandle context = 0;
    AudioOut2PortHandle port = 0;
    // Buffers used in turn, as a guest mixer cycles its output buffers. All hold the same signal, so
    // the test does not depend on which one a push captures.
    std::vector<std::vector<float>> buffers = std::vector<std::vector<float>>(4, std::vector<float>(static_cast<std::size_t>(grain) * 2, 0.5f));

    explicit Session(const char* name) : name(name), path(std::filesystem::temp_directory_path() / (std::to_string(std::random_device{}()) + "-anyps5_audio_out2_pacing_" + name + ".raw")) {
        std::filesystem::remove(path);
        SetEnvironment("SDL_DISKAUDIOFILE", path.string());
        AudioOut2ContextParam params{};
        Check(sceAudioOut2ContextResetParam(&params) == 0, "ContextResetParam");
        params.num_grains = grain;
        params.queue_depth = queueDepth;
        Check(sceAudioOut2ContextCreate(&params, nullptr, 0, &context) == 0, "ContextCreate");
        AudioOut2PortParam portParams{};
        portParams.port_type = 0;
        portParams.data_format = formatStereoFloat;
        portParams.sampling_freq = frequency;
        Check(sceAudioOut2PortCreate(context, &portParams, &port) == 0, "PortCreate");
    }

    void Point(const void* data) {
        const AudioOut2Attribute attribute{attributeData, 0, &data, sizeof(data)};
        Check(sceAudioOut2PortSetAttributes(port, &attribute, 1) == 0, "PortSetAttributes");
    }

    // Plays out what is queued, closes the context and returns the dropouts in what the device played.
    std::vector<Dropout> Finish() {
        Point(nullptr);
        for (std::uint32_t push = 0; push < 64; push++) Check(sceAudioOut2ContextPush(context, 1) == 0, "blocking Push of silence");
        Check(sceAudioOut2PortDestroy(port) == 0, "PortDestroy");
        Check(sceAudioOut2ContextDestroy(context) == 0, "ContextDestroy");
        std::ifstream file(path, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        file.close();
        std::filesystem::remove(path);
        const auto* samples = reinterpret_cast<const float*>(bytes.data());
        const std::vector<float> played(samples, samples + bytes.size() / sizeof(float));
        Check(!played.empty(), "the device played nothing");
        auto dropouts = Dropouts(played);
        std::printf("audio_out2_pacing: %s: %zu dropouts", name, dropouts.size());
        for (const auto& dropout : dropouts) std::printf(" [%.2f s: %.0f ms]", dropout.at, dropout.seconds * 1000.0);
        std::printf("\n");
        return dropouts;
    }
};

// Runs a blocking-push mixer for `grains` grains, sleeping `stall` before every `stallEvery`-th push
// (0: never), and returns the dropouts in what the device played.
std::vector<Dropout> Play(const char* name, std::uint32_t grains, std::uint32_t stallEvery, std::chrono::milliseconds stall) {
    Session session(name);
    for (std::uint32_t push = 0; push < grains; push++) {
        if (stallEvery != 0 && push != 0 && push % stallEvery == 0) std::this_thread::sleep_for(stall);
        session.Point(session.buffers[push % session.buffers.size()].data());
        Check(sceAudioOut2ContextPush(session.context, 1) == 0, "blocking Push");
    }
    return session.Finish();
}

// A guest that keeps up is not cut. The test threads are host threads on a shared, often loaded
// machine, so one cut from the host descheduling them is tolerated (the jitter buffer then grows).
constexpr std::size_t hostJitterDropouts = 1;

void TestSteadyGuest() {
    const auto dropouts = Play("steady", 280, 0, std::chrono::milliseconds(0));
    Check(dropouts.size() <= hostJitterDropouts, "a guest pushing at the port rate heard dropouts");
}

// A guest whose mixer stalls for 120 ms every ~0.64 s (longer than the queue it is allowed to fill
// ahead) still produces audio in real time on average. The output may cut out while it adapts, but
// not over and over: at most 3 times, and not in the second half of the 8 s.
void TestStallingGuest() {
    const auto dropouts = Play("stalling", 750, 60, std::chrono::milliseconds(120));
    std::size_t late = 0;
    for (const auto& dropout : dropouts) late += dropout.at > 4.0 ? 1 : 0;
    Check(late == 0, "a guest with periodic 120 ms stalls kept cutting out after 4 s");
    Check(dropouts.size() <= 3, "a guest with periodic 120 ms stalls cut out more than 3 times");
}


// The reference title's sink (see the file comment). The game frame loop wakes the renderer twice a
// second, as a slow frame rate on a loaded host does.
void TestEventDrivenSink() {
    constexpr std::uint32_t grains = 280;
    constexpr auto frameInterval = std::chrono::milliseconds(500);
    Session session("event_driven_sink");
    std::mutex lock;
    std::condition_variable changed;
    bool wake = true;
    bool stop = false;
    std::uint32_t handedOver = 0;
    std::atomic<std::uint32_t> pushed{0};
    const auto start = std::chrono::steady_clock::now();
    std::thread pusher([&] {
        for (std::uint32_t next = 0; next < grains; next++) {
            {
                std::unique_lock guard(lock);
                changed.wait(guard, [&] { return handedOver > next || stop; });
                if (stop) return;
            }
            Check(sceAudioOut2ContextPush(session.context, 1) == 0, "sink blocking Push");
            pushed++;
            std::lock_guard guard(lock);
            wake = true;
            changed.notify_all();
        }
    });
    std::thread frames([&] {
        std::unique_lock guard(lock);
        while (!stop) {
            changed.wait_for(guard, frameInterval, [&] { return stop; });
            wake = true;
            changed.notify_all();
        }
    });
    std::uint32_t rendered = 0;
    while (rendered < grains) {
        std::unique_lock guard(lock);
        changed.wait(guard, [&] { return wake; });
        wake = false;
        guard.unlock();
        // Render while the hardware queue has room beyond the grains already handed to the pusher.
        for (;;) {
            std::uint32_t level = 0;
            std::uint32_t available = 0;
            Check(sceAudioOut2ContextGetQueueLevel(session.context, &level, &available) == 0, "GetQueueLevel");
            std::lock_guard handOver(lock);
            if (rendered == grains || available <= handedOver - pushed.load()) break;
            session.Point(session.buffers[rendered % session.buffers.size()].data());
            rendered++;
            handedOver++;
            changed.notify_all();
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(30)) break;
    }
    if (rendered < grains) {
        std::lock_guard guard(lock);
        stop = true;
        changed.notify_all();
    }
    pusher.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    {
        std::lock_guard guard(lock);
        stop = true;
        changed.notify_all();
    }
    frames.join();
    const double audio = static_cast<double>(grains) * grain / frequency;
    std::printf("audio_out2_pacing: event_driven_sink: %u grains (%.2f s of audio) handed over in %.2f s\n", grains, audio, seconds);
    const auto dropouts = session.Finish();
    Check(seconds < audio * 1.5, "an event-driven sink did not keep up with the output (blocking pushes must return at the output rate)");
    Check(dropouts.size() <= hostJitterDropouts, "an event-driven sink heard dropouts");
}

}

int main() {
    SetEnvironment("SDL_AUDIODRIVER", "disk");
    TestSteadyGuest();
    TestStallingGuest();
    TestEventDrivenSink();
    if (failures != 0) return 1;
    std::puts("audio_out2_pacing: ok");
    return 0;
}
