#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace Cpu {

// Owner-thread API. Core Audio only reads host-owned PCM from a bounded ring.
class NativeAudioOutput final {
public:
    struct Config {
        std::uint32_t capacityFrames;
    };
    enum class SubmitStatus { Accepted, WouldBlock, Stopped, Fault };
    struct Snapshot {
        std::uint32_t capacityFrames;
        std::uint32_t queuedFrames;
        std::uint64_t acceptedFrames;
        // Real PCM pulled by Core Audio, not proof of physical/audible playback.
        std::uint64_t renderedFrames;
        std::uint64_t underrunFrames;
        std::uint64_t discardedFrames;
        std::int32_t nativeError;
        bool running;
    };

    static std::unique_ptr<NativeAudioOutput> Open(Config config);
    ~NativeAudioOutput();
    NativeAudioOutput(const NativeAudioOutput&) = delete;
    NativeAudioOutput& operator=(const NativeAudioOutput&) = delete;

    // Copies every frame or accepts none. Input contains interleaved L,R float32.
    SubmitStatus SubmitInterleavedStereo(std::span<const float> pcm);
    std::uint32_t AvailableFrames();
    Snapshot GetSnapshot();
    void Close();

private:
    struct Impl;
    explicit NativeAudioOutput(Config config);
    std::unique_ptr<Impl> impl;
};

}
