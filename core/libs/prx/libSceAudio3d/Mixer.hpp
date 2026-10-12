#ifndef CORE_LIBS_PRX_LIBSCEAUDIO3D_MIXER_HPP
#define CORE_LIBS_PRX_LIBSCEAUDIO3D_MIXER_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace Audio3dInternal {

struct Source {
    const void* samples;
    std::uint32_t num_samples;
    std::uint32_t channels;
    bool is_float;
    float gain;
    float pan;
};

inline float ReadSample(const Source& source, std::uint32_t frame, std::uint32_t channel) {
    const std::size_t index = static_cast<std::size_t>(frame) * source.channels + channel;
    if (source.is_float) return static_cast<const float*>(source.samples)[index];
    return static_cast<float>(static_cast<const std::int16_t*>(source.samples)[index]) / 32768.0f;
}

inline void MixFrame(const Source* sources, std::size_t count, std::uint32_t granularity, std::vector<std::int16_t>& out) {
    std::vector<float> mix(static_cast<std::size_t>(granularity) * 2, 0.0f);
    for (std::size_t source_index = 0; source_index < count; ++source_index) {
        const Source& source = sources[source_index];
        const double angle = (static_cast<double>(source.pan) + 1.0) * 0.25 * std::numbers::pi;
        const float left_gain = static_cast<float>(std::cos(angle)) * source.gain;
        const float right_gain = static_cast<float>(std::sin(angle)) * source.gain;
        for (std::uint32_t frame = 0; frame < granularity && frame < source.num_samples; ++frame) {
            const std::size_t left_index = static_cast<std::size_t>(frame) * 2;
            if (source.channels == 1) {
                const float sample = ReadSample(source, frame, 0);
                mix[left_index] += sample * left_gain;
                mix[left_index + 1] += sample * right_gain;
            } else {
                mix[left_index] += ReadSample(source, frame, 0) * source.gain;
                mix[left_index + 1] += ReadSample(source, frame, 1) * source.gain;
            }
        }
    }
    out.resize(static_cast<std::size_t>(granularity) * 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::int16_t>(std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));
    }
}

}

#endif
