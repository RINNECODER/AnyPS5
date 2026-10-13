#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace AgcDriver::Metal::GuestBytesFingerprint {

// Fingerprints of guest bytes, for GuestImageCache to tell whether a surface's bytes changed
// since a texture was made from them (or copied back into them).

// Every byte: an XXH3-style accumulation over 64-byte stripes (32-bit multiplies of the data
// mixed with a per-stripe key), about 30 GB/s on one core under Rosetta.
inline std::uint64_t Hash(const std::byte* bytes, std::size_t size) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    std::uint64_t result = 0x27D4EB2F165667C5ull ^ (size * 0x9E3779B97F4A7C15ull);
    std::size_t at = 0;
#if defined(__SSE2__)
    __m128i acc[4] = {_mm_set1_epi64x(static_cast<long long>(0x9E3779B185EBCA87ull)),
        _mm_set1_epi64x(static_cast<long long>(0xC2B2AE3D27D4EB4Full)), _mm_set1_epi64x(static_cast<long long>(0x165667B19E3779F9ull)),
        _mm_set1_epi64x(static_cast<long long>(0x85EBCA77C2B2AE63ull))};
    __m128i key = _mm_set_epi64x(static_cast<long long>(0x27D4EB2F165667C5ull), static_cast<long long>(0x9E3779B97F4A7C15ull));
    const __m128i step = _mm_set_epi64x(static_cast<long long>(0x94D049BB133111EBull), static_cast<long long>(0xBF58476D1CE4E5B9ull));
    for (; at + 64 <= size; at += 64) {
        for (int lane = 0; lane < 4; ++lane) {
            const __m128i value = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + at + lane * 16));
            const __m128i mixed = _mm_xor_si128(value, key);
            const __m128i product = _mm_mul_epu32(mixed, _mm_shuffle_epi32(mixed, 0x31));
            acc[lane] = _mm_add_epi64(acc[lane], _mm_add_epi64(product, _mm_shuffle_epi32(value, 0x4e)));
        }
        key = _mm_add_epi64(key, step);
    }
    std::uint64_t lanes[8];
    for (int lane = 0; lane < 4; ++lane) _mm_storeu_si128(reinterpret_cast<__m128i*>(lanes + 2 * lane), acc[lane]);
    for (const auto lane : lanes) {
        result ^= lane;
        result *= 0x9E3779B97F4A7C15ull;
        result ^= result >> 29u;
    }
#else
    std::uint64_t lanes[4] = {0x9E3779B185EBCA87ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x85EBCA77C2B2AE63ull};
    for (; at + 32 <= size; at += 32) {
        for (int lane = 0; lane < 4; ++lane) {
            std::uint64_t value;
            std::memcpy(&value, data + at + lane * 8, 8);
            lanes[lane] = (lanes[lane] ^ (value * 0xC2B2AE3D27D4EB4Full)) * 0x9E3779B97F4A7C15ull;
            lanes[lane] ^= lanes[lane] >> 31u;
        }
    }
    for (const auto lane : lanes) {
        result ^= lane;
        result *= 0x9E3779B97F4A7C15ull;
        result ^= result >> 29u;
    }
#endif
    for (; at < size; ++at) {
        result ^= data[at];
        result *= 0x100000001B3ull;
    }
    return result;
}

// A sample: 512 eight-byte words spread over the bytes (and their first and last words). It reads
// about 4 KiB whatever the size, so it is cheap enough to take for every surface the GPU writes,
// but it only notices a change that touches a sampled word: a whole-surface rewrite or clear by
// the CPU, not a small patch.
inline std::uint64_t Sample(const std::byte* bytes, std::size_t size) {
    constexpr std::size_t Samples = 512;
    std::uint64_t result = 0x165667B19E3779F9ull ^ size;
    const auto mix = [&](std::uint64_t value) {
        result ^= value + 0x9E3779B97F4A7C15ull + (result << 6u) + (result >> 2u);
    };
    if (size <= Samples * 8) {
        mix(Hash(bytes, size));
        return result;
    }
    const auto words = size / 8;
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < Samples; ++i) {
        // Evenly spread, each offset jittered within its stride by a fixed sequence.
        const auto stride = words / Samples;
        const auto jitter = static_cast<std::size_t>((i * 0x9E3779B97F4A7C15ull) >> 40u) % stride;
        std::memcpy(&value, bytes + (i * stride + jitter) * 8, 8);
        mix(value);
    }
    std::memcpy(&value, bytes, 8);
    mix(value);
    std::memcpy(&value, bytes + size - 8, 8);
    mix(value);
    return result;
}

}
