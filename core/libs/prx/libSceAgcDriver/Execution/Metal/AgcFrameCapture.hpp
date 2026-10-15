#pragma once

// Frame capture and replay for the Metal AGC driver (fork-only; tools/agc-replay).
//
// Capture (off unless ANYPS5_AGC_CAPTURE=<dir> is set): after the queue workers have executed
// flip ANYPS5_AGC_CAPTURE_FLIP - 1 (default 600; ANYPS5_AGC_CAPTURE_AFTER_S=<seconds> captures the
// first flip executed after that long since the driver loaded instead), the worker that ran that flip drains the GPU and
// writes <dir>/frame.agc:
//   - every borrowed guest range the driver sees (GPU-visible guest memory plus its staging range)
//     and the contents of its resident, non-zero 1 MiB chunks, compressed: the pre-frame state;
//   - the queue state (registers, constant RAM, index and indirect bases, predication) of each
//     queue at its first recorded submission;
//   - every submission the workers execute until flip N + ANYPS5_AGC_CAPTURE_FRAMES - 1, in
//     execution order: queue, command address and words, the shader registry it was submitted
//     with, and the 64 KiB guest pages that changed since the previous submission (taken just
//     before it executes, with the GPU drained), each with the hash of its previous contents;
//   - for each captured flip, the display buffer the next present scanned out and its pixels
//     (the frame the game showed), also written as <dir>/game-<frame>.png.
// The hooks sit at the submission boundary (queue worker loop) and in Present; they never touch
// the per-draw path. Capturing stalls the game for the snapshot (seconds) once.
//
// Replay (anyps5_agc_replay) maps the ranges at their guest addresses, submits the recorded
// command buffers through the same driver, and applies each recorded page delta from the worker
// just before that submission executes: a page is written only while the replay's copy still
// equals the game's previous contents, so pages the replayed GPU work wrote keep the replay's
// bytes (the delta carries CPU writes the guest made between submissions).
//
// File format (little-endian, version 1): "AGCFRAME", u32 version, u32 compression algorithm
// (libcompression constant), then records {u32 tag, u32 reserved, u64 payload bytes, payload}.

#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/Submission.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace AgcDriver::Metal::AgcCapture {

inline constexpr char Magic[8] = {'A', 'G', 'C', 'F', 'R', 'A', 'M', 'E'};
inline constexpr std::uint32_t Version = 1;
inline constexpr std::uint64_t ChunkBytes = std::uint64_t{1} << 20;
inline constexpr std::uint64_t PageBytes = std::uint64_t{64} << 10;

enum class Tag : std::uint32_t {
    Range = 1,      // u64 guest, u64 bytes, u32 writable, u32 0
    Chunk = 2,      // u64 guest, u32 raw bytes, u32 stored bytes (== raw: uncompressed), u64 hash, data
    Shader = 3,     // u32 id, u32 type, u64 code address, u64 header address, u32 code words, u32 header bytes, code, header
    Registry = 4,   // u32 id, u32 entries, entries x {u64 key, u32 shader id}
    QueueState = 5, // u32 queue, serialized QueueState
    Submit = 6,     // u32 queue, u32 registry id, u64 command address, u32 words, u32 pages, u32 frame, u32 0,
                    // words x u32, pages x {u64 guest, u32 raw, u32 stored, u64 previous hash, u64 hash, data}
    Display = 7,    // u32 frame, u32 0, DisplayBuffer fields, u64 pixel hash, u32 raw, u32 stored, data
    End = 8,        // u32 frames, u32 submissions
};

// FNV-style 4-lane hash; the same function hashes chunks, pages and display pixels.
inline std::uint64_t Hash(const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t lanes[4] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full, 0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    std::size_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        for (int lane = 0; lane < 4; ++lane) {
            std::uint64_t word;
            std::memcpy(&word, p + i + 8 * lane, 8);
            lanes[lane] = (lanes[lane] ^ word) * 0x100000001b3ull;
        }
    }
    std::uint64_t h = lanes[0] ^ (lanes[1] << 1) ^ (lanes[2] << 2) ^ (lanes[3] << 3) ^ bytes;
    for (; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return h;
}

class Writer {
public:
    std::vector<std::byte> bytes;
    template <class T> void Put(const T& value) {
        const auto* p = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    void PutBytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
};

class Reader {
public:
    Reader(const std::byte* data, std::size_t size) : data(data), size(size) {}
    template <class T> T Get() {
        T value;
        std::memcpy(&value, Take(sizeof(T)), sizeof(T));
        return value;
    }
    const std::byte* Take(std::size_t count) {
        if (count > size - offset) throw std::runtime_error("agc capture: truncated record");
        const auto* p = data + offset;
        offset += count;
        return p;
    }
    [[nodiscard]] bool Done() const { return offset == size; }

private:
    const std::byte* data;
    std::size_t size;
    std::size_t offset = 0;
};

void WriteQueueState(Writer& out, const QueueState& state);
QueueState ReadQueueState(Reader& in);

// Compresses with the capture's algorithm; returns raw bytes when compression does not help.
std::vector<std::byte> Compress(const void* data, std::size_t bytes);
// Decompresses `stored` bytes into exactly `raw` bytes at `target` (a copy when stored == raw).
void Decompress(const std::byte* stored, std::size_t storedBytes, void* target, std::size_t raw);

// Writes width x height BGRA8 pixels (scanout layout) as a PNG.
bool WriteBgraPng(const std::filesystem::path& path, const std::vector<std::byte>& pixels, std::uint32_t width, std::uint32_t height);

// What a hook needs from the driver: the worker's ranges, the GPU lock and the drain
// (CompletePriorGpuWorkAndCopyBack, to call with the GPU lock held).
struct Context {
    std::span<const NativeGuestMemory::BorrowedRange> ranges;
    std::recursive_mutex& gpuMutex;
    std::function<void()> drain;
};

// Whether a capture or a replay is installed (cheap; the hooks return at once otherwise).
bool Active();
// Queue worker, before and after ExecuteSubmission (not holding the GPU lock).
void BeforeExecute(const DriverDetail::Submission& submission, const QueueState& state, const Context& context);
void AfterExecute(const DriverDetail::Submission& submission, const Context& context);
// MetalDriver::Present with the GPU lock held; `pixels` are the decoded display pixels when read.
void Presented(const DisplayBuffer* buffer, const std::vector<std::byte>* pixels, const Context& context);

// Replay: called on the worker before each submission of `queue` executes, in queue order.
using ReplayHook = std::function<void(std::uint32_t queue, const Context& context)>;
void InstallReplay(ReplayHook hook);

// GPU busy time (union of command buffer GPU intervals) while enabled; MetalDevice records it.
void EnableGpuBusy(bool enabled);
bool GpuBusyEnabled();
void RecordGpuBusy(double gpuStart, double gpuEnd);
double TakeGpuBusyMs();

}
