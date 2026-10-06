#include <cpu/NativeAudioOutput.hpp>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Cpu {
namespace {
constexpr std::uint32_t MaximumCapacity = 1u << 20;
constexpr std::uint32_t MaximumRenderFrames = 4096;
constexpr double SampleRate = 48000.0;
constexpr AudioFormatFlags ClientFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked) |
                                               static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::int32_t>::is_always_lock_free);

void Check(OSStatus status, const char* operation) {
    if (status != noErr) throw std::runtime_error(std::string(operation) + ": OSStatus=" + std::to_string(status));
}
}

struct NativeAudioOutput::Impl {
    const std::uint32_t capacity;
    std::unique_ptr<float[]> ring;
    alignas(16) std::array<std::array<float, MaximumRenderFrames>, 2> scratch{};
    std::atomic<std::uint64_t> written{0}, read{0}, underrun{0};
    std::atomic<std::int32_t> failure{0};
    std::atomic<bool> closing{false};
    AudioUnit unit = nullptr;
    bool initialized = false;
    bool started = false;
    bool closed = false;
    std::uint64_t discarded = 0;

    explicit Impl(std::uint32_t frames) : capacity(frames), ring(std::make_unique<float[]>(std::size_t{frames} * 2)) {}

    void Fail(OSStatus status) noexcept {
        if (status == noErr) return;
        std::int32_t expected = 0;
        failure.compare_exchange_strong(expected, status, std::memory_order_relaxed);
    }

    static OSStatus Render(void* context, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*,
                           UInt32, UInt32 frames, AudioBufferList* buffers) noexcept {
        auto& self = *static_cast<Impl*>(context);
        if (frames > MaximumRenderFrames || buffers == nullptr || buffers->mNumberBuffers != 2) {
            self.Fail(kAudio_ParamError);
            return kAudio_ParamError;
        }
        const UInt32 bytes = frames * sizeof(float);
        for (std::uint32_t channel = 0; channel < 2; ++channel) {
            auto& buffer = buffers->mBuffers[channel];
            if (buffer.mNumberChannels != 1 || (buffer.mData != nullptr && buffer.mDataByteSize < bytes)) {
                self.Fail(kAudio_ParamError);
                return kAudio_ParamError;
            }
            if (buffer.mData == nullptr) buffer.mData = self.scratch[channel].data();
            buffer.mDataByteSize = bytes;
        }
        auto* left = static_cast<float*>(buffers->mBuffers[0].mData);
        auto* right = static_cast<float*>(buffers->mBuffers[1].mData);
        const auto begin = self.read.load(std::memory_order_relaxed);
        const auto end = self.written.load(std::memory_order_acquire);
        const auto available = end - begin;
        if (available > self.capacity) {
            self.Fail(kAudio_ParamError);
            return kAudio_ParamError;
        }
        const auto count = self.closing.load(std::memory_order_acquire) || self.failure.load(std::memory_order_relaxed) != 0 ?
            0u : static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, available));
        for (std::uint32_t frame = 0; frame < count; ++frame) {
            const auto index = static_cast<std::size_t>((begin + frame) % self.capacity) * 2;
            left[frame] = self.ring[index];
            right[frame] = self.ring[index + 1];
        }
        std::fill_n(left + count, frames - count, 0.0f);
        std::fill_n(right + count, frames - count, 0.0f);
        if (flags != nullptr) {
            if (count == 0) *flags |= kAudioUnitRenderAction_OutputIsSilence;
            else *flags &= ~static_cast<AudioUnitRenderActionFlags>(kAudioUnitRenderAction_OutputIsSilence);
        }
        self.read.store(begin + count, std::memory_order_release);
        self.underrun.fetch_add(frames - count, std::memory_order_relaxed);
        return noErr;
    }

    void Start() {
        AudioComponentDescription component{};
        component.componentType = kAudioUnitType_Output;
        component.componentSubType = kAudioUnitSubType_DefaultOutput;
        component.componentManufacturer = kAudioUnitManufacturer_Apple;
        auto native = AudioComponentFindNext(nullptr, &component);
        if (native == nullptr) throw std::runtime_error("Native default audio output component is unavailable");
        Check(AudioComponentInstanceNew(native, &unit), "AudioComponentInstanceNew");
        AudioStreamBasicDescription format{};
        format.mSampleRate = SampleRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = ClientFormatFlags;
        format.mBytesPerPacket = sizeof(float);
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = sizeof(float);
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        Check(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)),
              "AudioUnit stereo 48 kHz format");
        UInt32 maximum = MaximumRenderFrames;
        Check(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximum, sizeof(maximum)),
              "AudioUnit maximum render frames");
        AURenderCallbackStruct callback{Render, this};
        Check(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)),
              "AudioUnit render callback");
        Check(AudioUnitInitialize(unit), "AudioUnitInitialize");
        initialized = true;
        UInt32 size = sizeof(format);
        Check(AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, &size),
              "AudioUnit accepted format");
        if (size != sizeof(format) || format.mSampleRate != SampleRate || format.mFormatID != kAudioFormatLinearPCM ||
            format.mFormatFlags != ClientFormatFlags ||
            format.mChannelsPerFrame != 2 || format.mBytesPerFrame != sizeof(float) || format.mFramesPerPacket != 1 ||
            format.mBytesPerPacket != sizeof(float) || format.mBitsPerChannel != 32)
            throw std::runtime_error("Native audio output did not retain the requested stereo float32 format");
        size = sizeof(maximum);
        Check(AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maximum, &size),
              "AudioUnit accepted render bound");
        if (size != sizeof(maximum) || maximum == 0 || maximum > MaximumRenderFrames)
            throw std::runtime_error("Native audio output render bound exceeds preallocated storage");
        Check(AudioOutputUnitStart(unit), "AudioOutputUnitStart");
        started = true;
        Inspect();
        Check(failure.load(std::memory_order_relaxed), "Native audio output availability");
    }

    // Native inspection occurs only on the producer/owner thread.
    bool Inspect() noexcept {
        if (closed || unit == nullptr || !started) return false;
        if (failure.load(std::memory_order_relaxed) != 0) return false;
        AudioDeviceID device = kAudioObjectUnknown;
        UInt32 size = sizeof(device);
        auto status = AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, &size);
        if (status != noErr || size != sizeof(device) || device == kAudioObjectUnknown) {
            Fail(status == noErr ? kAudioHardwareBadDeviceError : status);
            return false;
        }
        UInt32 alive = 0;
        size = sizeof(alive);
        const AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain};
        status = AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &alive);
        if (status != noErr || size != sizeof(alive) || alive == 0) {
            Fail(status == noErr ? kAudioHardwareBadDeviceError : status);
            return false;
        }
        OSStatus renderError = noErr;
        size = sizeof(renderError);
        status = AudioUnitGetProperty(unit, kAudioUnitProperty_LastRenderError, kAudioUnitScope_Global, 0, &renderError, &size);
        if (status != noErr || size != sizeof(renderError) || renderError != noErr) {
            Fail(status != noErr ? status : size != sizeof(renderError) ? kAudio_ParamError : renderError);
            return false;
        }
        UInt32 running = 0;
        size = sizeof(running);
        status = AudioUnitGetProperty(unit, kAudioOutputUnitProperty_IsRunning, kAudioUnitScope_Global, 0, &running, &size);
        if (status != noErr || size != sizeof(running) || running == 0) {
            Fail(status == noErr ? kAudioUnitErr_Uninitialized : status);
            return false;
        }
        return true;
    }

    OSStatus Stop() noexcept {
        if (closed) return noErr;
        closing.store(true, std::memory_order_release);
        OSStatus first = noErr;
        const auto record = [&](OSStatus status) {
            if (status != noErr && first == noErr) first = status;
            Fail(status);
        };
        if (unit != nullptr) {
            if (started) record(AudioOutputUnitStop(unit));
            if (initialized) record(AudioUnitUninitialize(unit));
            const auto disposed = AudioComponentInstanceDispose(unit);
            record(disposed);
            if (disposed != noErr) return first;
            unit = nullptr;
        }
        // Successful native disposal is the callback lifetime barrier.
        discarded = written.load(std::memory_order_relaxed) - read.load(std::memory_order_acquire);
        started = false;
        initialized = false;
        closed = true;
        return first;
    }
};

NativeAudioOutput::NativeAudioOutput(Config config) {
    if (config.capacityFrames == 0 || config.capacityFrames > MaximumCapacity)
        throw std::invalid_argument("Native audio queue capacity must be within 1..1048576 stereo frames");
    impl = std::make_unique<Impl>(config.capacityFrames);
}

std::unique_ptr<NativeAudioOutput> NativeAudioOutput::Open(Config config) {
    auto output = std::unique_ptr<NativeAudioOutput>(new NativeAudioOutput(config));
    output->impl->Start();
    return output;
}

NativeAudioOutput::~NativeAudioOutput() {
    if (!impl) return;
    impl->Stop();
    // If native disposal fails, its raw callback context may still be referenced.
    // Retain that state instead of freeing memory under an uncertain callback.
    if (impl->unit != nullptr) static_cast<void>(impl.release());
}

NativeAudioOutput::SubmitStatus NativeAudioOutput::SubmitInterleavedStereo(std::span<const float> pcm) {
    if (pcm.empty() || pcm.size() % 2 != 0 || pcm.size() / 2 > impl->capacity)
        throw std::invalid_argument("Native audio submission must contain a bounded complete stereo frame block");
    for (const auto sample : pcm) if (!std::isfinite(sample))
        throw std::invalid_argument("Native audio submission contains nonfinite PCM");
    if (impl->closed || impl->closing.load(std::memory_order_acquire)) return SubmitStatus::Stopped;
    if (!impl->Inspect()) return SubmitStatus::Fault;
    const auto begin = impl->written.load(std::memory_order_relaxed);
    const auto consumed = impl->read.load(std::memory_order_acquire);
    const auto frames = pcm.size() / 2;
    if (begin - consumed > impl->capacity) throw std::runtime_error("Native audio queue accounting is invalid");
    if (frames > impl->capacity - (begin - consumed)) return SubmitStatus::WouldBlock;
    if (begin > std::numeric_limits<std::uint64_t>::max() - frames)
        throw std::overflow_error("Native audio frame counter overflow");
    const auto offset = static_cast<std::size_t>(begin % impl->capacity);
    const auto first = std::min<std::size_t>(frames, impl->capacity - offset);
    std::memcpy(impl->ring.get() + offset * 2, pcm.data(), first * 2 * sizeof(float));
    if (frames != first) std::memcpy(impl->ring.get(), pcm.data() + first * 2, (frames - first) * 2 * sizeof(float));
    if (!impl->Inspect()) return SubmitStatus::Fault;
    impl->written.store(begin + frames, std::memory_order_release);
    return SubmitStatus::Accepted;
}

NativeAudioOutput::Snapshot NativeAudioOutput::GetSnapshot() {
    const bool running = impl->Inspect();
    const auto accepted = impl->written.load(std::memory_order_relaxed);
    const auto rendered = impl->read.load(std::memory_order_acquire);
    const auto queued = impl->closed ? 0u : static_cast<std::uint32_t>(accepted - rendered);
    return {impl->capacity, queued, accepted, rendered, impl->underrun.load(std::memory_order_relaxed),
            impl->discarded, impl->failure.load(std::memory_order_relaxed), running};
}

std::uint32_t NativeAudioOutput::AvailableFrames() {
    const auto state = GetSnapshot();
    return state.running && state.nativeError == 0 ? state.capacityFrames - state.queuedFrames : 0;
}

void NativeAudioOutput::Close() { Check(impl->Stop(), "Native audio output close"); }

}
