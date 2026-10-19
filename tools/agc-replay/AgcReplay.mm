// anyps5_agc_replay: replays a frame captured with ANYPS5_AGC_CAPTURE through the Metal AGC driver
// (Execution/Metal/AgcFrameCapture.hpp has the capture and the file format).
//
//   anyps5_agc_replay <capture-dir> [--frames N] [--repeat K] [--dump-png out.png]
//                     [--expect-hash HEX] [--expect-png ref.png [--max-diff-pixels N]] [--metallib PATH] [--quiet]
//
// The captured guest ranges are mapped at their guest addresses (guest pointers are host pointers,
// as on the Rosetta direct path) and the driver is configured with them as the game's was. Each
// repeat restores the pre-frame memory and queue state, submits the recorded command buffers in
// order and waits for the GPU, then reads the scanout pixels of each frame's display buffer.
// Reported per frame: wall ms (first submit to idle), worker CPU ms (thread CPU time of the
// agc-queue-* workers, without the replay's own page writes), GPU busy ms (union of command
// buffer GPU intervals) and the image hash. Repeat 1 is a warm-up (pipelines compile there).
// The first repeat is a warm-up for timing and for the hash (cold pipelines and caches render a few
// hundred pixels 1-3 levels apart); the hash must be stable over the later repeats.
// --expect-hash checks the last frame's hash in every timed repeat (pixel-exact); --expect-png
// compares the last frame of every timed repeat against a PNG (e.g. one --dump-png wrote), allowing
// --max-diff-pixels in each (the warm-up is compared and reported, not judged).
// Exit status: 0, 1 on a mismatch or an unstable hash across timed repeats, 2 on errors.
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include "prx/libSceAgcDriver/Execution/Metal/AgcCaptureLogic.hpp"
#include "prx/libSceAgcDriver/Execution/Metal/AgcFrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/Metal/DirectDriver.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPipelineCache.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPipelineManifest.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPresentation.hpp"
#include <mach/mach.h>
#include <libproc.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef ANYPS5_AGC_REPLAY_METALLIB
#define ANYPS5_AGC_REPLAY_METALLIB ""
#endif

using namespace AgcDriver;
namespace Capture = AgcDriver::Metal::AgcCapture;

namespace {

struct Block {
    std::uint64_t guest;
    std::uint32_t raw;
    std::uint32_t stored;
    std::uint64_t previous;
    std::uint64_t hash;
    const std::byte* data;
};

struct Range {
    std::uint64_t guest;
    std::uint64_t bytes;
    bool writable;
    std::vector<const Block*> chunks;  // per 1 MiB chunk, null when the capture had none (zero)
    std::byte* host = nullptr;
    std::size_t activation = 0;  // global submission index it appeared at (0: part of the pre-frame snapshot)
    bool live = false;
    bool filled = false;
};

struct Submit {
    std::uint32_t queue;
    std::uint32_t registry;
    std::uint64_t address;
    const std::uint32_t* words;
    std::uint32_t count;
    std::uint32_t frame;
    std::size_t index;  // position in the capture's global execution order
    std::vector<Block> pages;
};

struct Display {
    DisplayBuffer buffer;
    std::uint64_t hash;
    std::uint32_t raw;
    std::uint32_t stored;
    const std::byte* data;
};

struct CaptureFile {
    std::vector<Range> ranges;
    std::vector<Block> chunks;
    std::vector<std::size_t> chunkRange;  // the range record each chunk belongs to
    std::map<std::uint32_t, std::shared_ptr<const DriverDetail::ShaderSnapshot>> shaders;
    std::map<std::uint32_t, std::shared_ptr<const DriverDetail::ShaderRegistry>> registries;
    std::map<std::uint32_t, QueueState> states;
    std::vector<Submit> submits;
    std::map<std::uint32_t, Display> displays;
    std::uint64_t fileBytes = 0;
};

[[noreturn]] void Fail(const std::string& message) {
    std::fprintf(stderr, "anyps5_agc_replay: %s\n", message.c_str());
    std::exit(2);
}

CaptureFile Load(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) Fail("cannot open " + path);
    const auto size = static_cast<std::size_t>(lseek(fd, 0, SEEK_END));
    auto* base = static_cast<const std::byte*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (base == MAP_FAILED || size < 16) Fail("cannot map " + path);
    if (std::memcmp(base, Capture::Magic, 8) != 0) Fail(path + " is not an AGC frame capture");
    std::uint32_t version = 0;
    std::memcpy(&version, base + 8, 4);
    if (version != Capture::Version) Fail(path + ": unsupported capture version " + std::to_string(version));
    CaptureFile file;
    file.fileBytes = size;
    std::size_t offset = 16;
    std::vector<std::pair<std::uint64_t, std::size_t>> chunkGuests;
    bool ended = false;
    while (offset + 16 <= size && !ended) {
        std::uint32_t tag = 0;
        std::uint64_t bytes = 0;
        std::memcpy(&tag, base + offset, 4);
        std::memcpy(&bytes, base + offset + 8, 8);
        offset += 16;
        if (bytes > size - offset) Fail("truncated capture record");
        Capture::Reader in(base + offset, bytes);
        switch (static_cast<Capture::Tag>(tag)) {
        case Capture::Tag::Range: {
            Range range{in.Get<std::uint64_t>(), in.Get<std::uint64_t>(), in.Get<std::uint32_t>() != 0, {}};
            range.chunks.assign((range.bytes + Capture::ChunkBytes - 1) / Capture::ChunkBytes, nullptr);
            // A range recorded while the frame ran appears just before the submission that introduced it.
            range.activation = file.submits.size();
            file.ranges.push_back(std::move(range));
            break;
        }
        case Capture::Tag::Chunk: {
            Block block{};
            block.guest = in.Get<std::uint64_t>();
            block.raw = in.Get<std::uint32_t>();
            block.stored = in.Get<std::uint32_t>();
            block.hash = in.Get<std::uint64_t>();
            block.data = in.Take(block.stored);
            // The chunk belongs to the latest range record that holds it (an address can be reused).
            std::size_t owner = file.ranges.size();
            bool found = false;
            while (owner > 0 && !found) {
                const auto& candidate = file.ranges[--owner];
                found = block.guest >= candidate.guest && block.guest < candidate.guest + candidate.bytes;
            }
            if (!found) Fail("chunk outside every range");
            file.chunkRange.push_back(owner);
            file.chunks.push_back(block);
            break;
        }
        case Capture::Tag::Shader: {
            const auto id = in.Get<std::uint32_t>();
            const auto type = static_cast<std::uint8_t>(in.Get<std::uint32_t>());
            const auto code = in.Get<std::uint64_t>();
            const auto header = in.Get<std::uint64_t>();
            const auto words = in.Get<std::uint32_t>();
            const auto headerBytes = in.Get<std::uint32_t>();
            DriverDetail::ShaderSnapshot snapshot{code, header, type, {}, {}};
            snapshot.code.resize(words);
            std::memcpy(snapshot.code.data(), in.Take(std::size_t{words} * 4), std::size_t{words} * 4);
            const auto* headerData = in.Take(headerBytes);
            snapshot.header.assign(headerData, headerData + headerBytes);
            file.shaders[id] = std::make_shared<const DriverDetail::ShaderSnapshot>(std::move(snapshot));
            break;
        }
        case Capture::Tag::Registry: {
            const auto id = in.Get<std::uint32_t>();
            const auto entries = in.Get<std::uint32_t>();
            auto registry = std::make_shared<DriverDetail::ShaderRegistry>();
            for (std::uint32_t i = 0; i < entries; ++i) {
                const auto key = in.Get<std::uint64_t>();
                const auto shader = file.shaders.find(in.Get<std::uint32_t>());
                if (shader == file.shaders.end()) Fail("registry names an unknown shader");
                (*registry)[key].push_back(shader->second);
            }
            file.registries[id] = std::move(registry);
            break;
        }
        case Capture::Tag::QueueState: {
            const auto queue = in.Get<std::uint32_t>();
            file.states[queue] = Capture::ReadQueueState(in);
            break;
        }
        case Capture::Tag::Submit: {
            Submit submit{};
            submit.queue = in.Get<std::uint32_t>();
            submit.registry = in.Get<std::uint32_t>();
            submit.address = in.Get<std::uint64_t>();
            submit.count = in.Get<std::uint32_t>();
            const auto pages = in.Get<std::uint32_t>();
            submit.frame = in.Get<std::uint32_t>();
            in.Get<std::uint32_t>();
            submit.words = reinterpret_cast<const std::uint32_t*>(in.Take(std::size_t{submit.count} * 4));
            for (std::uint32_t i = 0; i < pages; ++i) {
                Block page{};
                page.guest = in.Get<std::uint64_t>();
                page.raw = in.Get<std::uint32_t>();
                page.stored = in.Get<std::uint32_t>();
                page.previous = in.Get<std::uint64_t>();
                page.hash = in.Get<std::uint64_t>();
                page.data = in.Take(page.stored);
                submit.pages.push_back(page);
            }
            file.submits.push_back(std::move(submit));
            break;
        }
        case Capture::Tag::Display: {
            const auto frame = in.Get<std::uint32_t>();
            in.Get<std::uint32_t>();
            Display display{};
            // The DisplayBuffer gained its trailing alignment field without a capture version bump,
            // so a version 1 display record holds either the 48-byte or the 56-byte struct. A
            // candidate layout has to make the record's payload length add up and its fields pass
            // DisplayBufferSize, which rejects a struct read at the wrong offset.
            const auto* payload = base + offset;
            const auto decode = [&](std::size_t structBytes, DisplayBuffer& buffer) {
                if (8u + structBytes + 16u > bytes) return false;
                std::uint32_t stored = 0;
                std::memcpy(&stored, payload + 8u + structBytes + 12u, 4);
                if (8u + structBytes + 16u + stored != bytes) return false;
                const auto* fields = payload + 8u;
                std::memcpy(&buffer.address, fields, 8);
                std::memcpy(&buffer.pixelFormat, fields + 8, 8);
                std::memcpy(&buffer.width, fields + 16, 4);
                std::memcpy(&buffer.height, fields + 20, 4);
                std::memcpy(&buffer.tilingMode, fields + 24, 4);
                std::memcpy(&buffer.pitchInPixel, fields + 28, 4);
                std::memcpy(&buffer.dccAddress, fields + 32, 8);
                std::memcpy(&buffer.dccClearColor, fields + 40, 8);
                if (structBytes != sizeof(DisplayBuffer)) buffer.alignment = 65536;
                else std::memcpy(&buffer.alignment, fields + 48, 4);
                try {
                    static_cast<void>(DisplayBufferSize(buffer));
                } catch (const std::exception&) {
                    return false;
                }
                return true;
            };
            std::size_t layout = 0;
            if (decode(sizeof(DisplayBuffer), display.buffer)) layout = sizeof(DisplayBuffer);
            else if (decode(48u, display.buffer)) layout = 48u;
            else Fail("capture display record has an inconsistent payload length");
            static_cast<void>(in.Take(layout));
            display.hash = in.Get<std::uint64_t>();
            display.raw = in.Get<std::uint32_t>();
            display.stored = in.Get<std::uint32_t>();
            display.data = in.Take(display.stored);
            file.displays[frame] = display;
            break;
        }
        case Capture::Tag::End: ended = true; break;
        default: Fail("unknown capture record " + std::to_string(tag));
        }
        offset += bytes;
    }
    if (!ended) Fail("capture has no end record (the game exited before the frame was presented?)");
    for (std::size_t i = 0; i < file.chunks.size(); ++i) {
        auto& range = file.ranges[file.chunkRange[i]];
        range.chunks[(file.chunks[i].guest - range.guest) / Capture::ChunkBytes] = &file.chunks[i];
    }
    return file;
}

// Per-thread counters by CPU performance level (xnu's PROC_PIDTHREADCOUNTS; not in the public SDK).
struct ThreadCountsData {
    std::uint64_t instructions;
    std::uint64_t cycles;
    std::uint64_t userTimeMach;
    std::uint64_t systemTimeMach;
    std::uint64_t energyNj;
};
struct ThreadCounts {
    std::uint16_t length;
    std::uint16_t reserved0;
    std::uint32_t reserved1;
    ThreadCountsData counts[4];
};
constexpr int ProcPidThreadCounts = 34;

struct WorkerCounters {
    std::uint64_t cpuNs = 0;
    std::uint64_t systemNs = 0;
    std::uint64_t instructions = 0;  // 0 when the kernel does not report them
};

// Thread CPU time and retired instructions of the driver's queue workers (threads named agc-queue-*).
WorkerCounters Workers() {
    WorkerCounters result;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return result;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        thread_extended_info_data_t info{};
        mach_msg_type_number_t infoCount = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &infoCount) == KERN_SUCCESS &&
            std::strncmp(info.pth_name, "agc-queue-", 10) == 0) {
            result.cpuNs += info.pth_user_time + info.pth_system_time;
            result.systemNs += info.pth_system_time;
            thread_identifier_info_data_t identifier{};
            mach_msg_type_number_t identifierCount = THREAD_IDENTIFIER_INFO_COUNT;
            ThreadCounts counts{};
            if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identifier), &identifierCount) == KERN_SUCCESS &&
                proc_pidinfo(getpid(), ProcPidThreadCounts, identifier.thread_id, &counts, sizeof counts) > 0) {
                const auto levels = std::min<std::size_t>(4, (counts.length - 8) / sizeof(ThreadCountsData));
                for (std::size_t level = 0; level < levels; ++level) result.instructions += counts.counts[level].instructions;
            }
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    return result;
}

std::uint64_t ThreadCpuNs() { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }

// Whether pending GPU work may store to guest [guest, guest + bytes): `ranges` are sorted, merged
// and disjoint.
bool PendingWriteOver(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, std::uint64_t guest, std::uint64_t bytes) {
    const auto at = std::upper_bound(ranges.begin(), ranges.end(), guest,
        [](std::uint64_t value, const auto& range) { return value < range.first; });
    if (at != ranges.begin() && guest - (at - 1)->first < (at - 1)->second) return true;
    return at != ranges.end() && at->first < guest + bytes;
}

std::uint64_t ThreadInstructions() {
    ThreadCounts counts{};
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    if (proc_pidinfo(getpid(), ProcPidThreadCounts, id, &counts, sizeof counts) <= 0) return 0;
    std::uint64_t total = 0;
    for (std::size_t level = 0; level < std::min<std::size_t>(4, (counts.length - 8) / sizeof(ThreadCountsData)); ++level)
        total += counts.counts[level].instructions;
    return total;
}

class ReplayFlip final : public IFlipRequest {
public:
    void GpuReady(const std::shared_ptr<FrameTiming>&) override {}
    void Fail(std::exception_ptr) noexcept override {}
};
class ReplayWait final : public IRenderingWait {
public:
    void Wait() override {}
};
class ReplayOutput final : public IVideoOutput {
public:
    std::shared_ptr<IFlipRequest> Reserve(const FlipInfo&) override { return std::make_shared<ReplayFlip>(); }
    std::shared_ptr<IRenderingWait> CaptureRenderingWait(std::uint32_t) override { return std::make_shared<ReplayWait>(); }
    void Fail(std::exception_ptr) noexcept override {}
};

struct DeltaStats {
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> kept{0};
    // Writes ordered against pending GPU work (the wait the restore used to take for every
    // submission) and how long those waits took.
    std::atomic<std::uint64_t> ordered{0};
    std::atomic<std::uint64_t> waitNs{0};
    std::atomic<std::uint64_t> cpuNs{0};
    std::atomic<std::uint64_t> instructions{0};
};

// Pixels that differ between two scanouts (colour channels and alpha separately), the largest colour delta.
struct PixelDiff {
    std::size_t colour = 0;
    std::size_t alpha = 0;
    int maxDelta = 0;
    std::uint32_t x0 = ~0u, x1 = 0, y0 = ~0u, y1 = 0;
};
PixelDiff Compare(const std::vector<std::byte>& a, const std::vector<std::byte>& b, std::uint32_t width) {
    PixelDiff diff;
    for (std::size_t i = 0; i + 3 < std::min(a.size(), b.size()); i += 4) {
        int delta = 0;
        for (int c = 0; c < 3; ++c) delta = std::max(delta, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
        if (delta != 0) {
            ++diff.colour;
            const auto x = static_cast<std::uint32_t>(i / 4 % width);
            const auto y = static_cast<std::uint32_t>(i / 4 / width);
            diff.x0 = std::min(diff.x0, x);
            diff.x1 = std::max(diff.x1, x);
            diff.y0 = std::min(diff.y0, y);
            diff.y1 = std::max(diff.y1, y);
        }
        if (a[i + 3] != b[i + 3]) ++diff.alpha;
        diff.maxDelta = std::max(diff.maxDelta, delta);
    }
    return diff;
}

// A PNG decoded to BGRA8 (scanout layout), empty when it cannot be read.
std::vector<std::byte> ReadPng(const std::string& path, std::uint32_t& width, std::uint32_t& height) {
    std::vector<std::byte> pixels;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), false);
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
    CFRelease(url);
    if (source == nullptr) return pixels;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (image == nullptr) return pixels;
    width = static_cast<std::uint32_t>(CGImageGetWidth(image));
    height = static_cast<std::uint32_t>(CGImageGetHeight(image));
    pixels.resize(std::size_t{width} * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, std::size_t{width} * 4, space,
        static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst));
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGColorSpaceRelease(space);
    CGImageRelease(image);
    return pixels;
}

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0 : values[values.size() / 2];
}

}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <capture-dir> [--frames N] [--repeat K] [--dump-png out.png] [--expect-hash HEX]\n"
                             "       [--expect-png ref.png [--max-diff-pixels N]] [--metallib PATH] [--quiet] [--pso-stats]\n", argv[0]);
        return 2;
    }
    std::string dir = argv[1];
    std::size_t frames = 0;
    std::size_t repeat = 5;
    std::string dumpPng;
    std::string expect;
    std::string expectPng;
    std::size_t maxDiffPixels = 0;
    std::string metallib = ANYPS5_AGC_REPLAY_METALLIB;
    if (const char* value = std::getenv("ANYPS5_METAL_UTILITY_LIBRARY"); value != nullptr && *value != '\0') metallib = value;
    bool quiet = false;
    bool psoStats = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) Fail(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "--frames") frames = std::stoul(next());
        else if (arg == "--repeat") repeat = std::max<std::size_t>(1, std::stoul(next()));
        else if (arg == "--dump-png") dumpPng = next();
        else if (arg == "--expect-hash") expect = next();
        else if (arg == "--expect-png") expectPng = next();
        else if (arg == "--max-diff-pixels") maxDiffPixels = std::stoul(next());
        else if (arg == "--metallib") metallib = next();
        else if (arg == "--quiet") quiet = true;
        else if (arg == "--pso-stats") psoStats = true;
        else Fail("unknown argument " + arg);
    }
    // The queue workers are created from this thread and inherit its QoS: user-interactive keeps
    // them on performance cores, so thread CPU time does not jump with core type.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    // Compiled shaders persist next to the capture, never in the user's shared cache.
    setenv("ANYPS5_SHADER_CACHE_DIR", (dir + "/shader_cache").c_str(), 0);
    const auto loadStart = std::chrono::steady_clock::now();
    auto capture = Load(dir + "/frame.agc");
    std::size_t captured = 0;
    for (const auto& submit : capture.submits) captured = std::max<std::size_t>(captured, submit.frame + 1);
    if (frames == 0 || frames > captured) frames = captured;
    if (frames == 0) Fail("capture holds no submissions");
    std::vector<const Submit*> submits;
    for (std::size_t i = 0; i < capture.submits.size(); ++i) {
        capture.submits[i].index = i;
        if (capture.submits[i].frame < frames) submits.push_back(&capture.submits[i]);
    }

    // Guest memory at its guest addresses.
    const auto pageSize = static_cast<std::uint64_t>(getpagesize());
    std::vector<NativeGuestMemory::BorrowedRange> ranges;
    // Ranges the title mapped during the frame exist only from the submission that introduced them.
    Capture::RangeTimeline timeline;
    for (const auto& range : capture.ranges) timeline.Add(range.guest, range.bytes, range.activation);
    const auto setLive = [&](std::size_t submission) {
        for (auto& range : capture.ranges) range.live = false;
        ranges.clear();
        for (const auto index : timeline.LiveAt(submission)) {
            auto& range = capture.ranges[index];
            range.live = true;
            ranges.push_back({range.guest, {range.host, range.bytes}, range.writable});
        }
        std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.guestAddress < b.guestAddress; });
    };
    // A range the replay process already uses at that address (the driver's staging buffer is a host
    // mapping, which can land where this process has its own) is mapped elsewhere: the driver
    // translates guest addresses through the ranges, so only identity-mapped guest pointers need
    // the guest address itself, and those point into the title's GPU allocations.
    std::vector<std::pair<std::uint64_t, std::int64_t>> moved;
    for (auto& range : capture.ranges) {
        const auto begin = range.guest & ~(pageSize - 1);
        const auto end = (range.guest + range.bytes + pageSize - 1) & ~(pageSize - 1);
        void* mapped = mmap(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mapped == MAP_FAILED) Fail("cannot map guest memory");
        if (mapped != reinterpret_cast<void*>(begin)) {
            std::fprintf(stderr, "note: guest range 0x%llx (+0x%llx) is in use in this process; mapped at %p\n",
                static_cast<unsigned long long>(range.guest), static_cast<unsigned long long>(range.bytes), mapped);
            range.host = static_cast<std::byte*>(mapped) + (range.guest - begin);
        } else {
            range.host = reinterpret_cast<std::byte*>(range.guest);
        }
    }
    // Only live ranges are looked up, newest first (an address reused by a later range wins).
    const auto host = [&](std::uint64_t guest) {
        for (auto it = capture.ranges.rbegin(); it != capture.ranges.rend(); ++it)
            if (it->live && guest >= it->guest && guest - it->guest < it->bytes) return it->host + (guest - it->guest);
        Fail("guest address outside the captured ranges");
    };
    setLive(0);
    std::uint64_t residentBytes = 0;
    for (std::size_t i = 0; i < capture.chunks.size(); ++i) {
        auto& range = capture.ranges[capture.chunkRange[i]];
        if (range.activation != 0) continue;  // filled when its submission is reached
        const auto& chunk = capture.chunks[i];
        Capture::Decompress(chunk.data, chunk.stored, range.host + (chunk.guest - range.guest), chunk.raw);
        residentBytes += chunk.raw;
        range.filled = true;
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) Fail("no Metal device");
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:metallib.c_str()]] error:&error];
    if (library == nil) Fail("cannot load the utility metallib " + metallib + " (--metallib)");
    Metal::ApplyDirectWorkingSet();
    auto& driver = Metal::MetalDriver::Get();
    driver.Configure((__bridge void*)device, (__bridge void*)library, ranges, {}, 1);
    AgcDriver::Metal::WaitForMetalPipelineManifestReplay(120000);
    driver.SetSkipInvalidCommandBuffers(true);
    std::set<std::uint32_t> handles;
    for (const auto* submit : submits)
        for (std::uint32_t i = 0; i + 1 < submit->count; ++i)
            if (submit->words[i] == FlipPacketHeader || submit->words[i] == RenderingWaitPacketHeader) handles.insert(submit->words[i + 1]);
    std::vector<std::shared_ptr<IVideoOutput>> outputs;
    for (const auto handle : handles) {
        outputs.push_back(std::make_shared<ReplayOutput>());
        driver.RegisterVideoOutput(handle, outputs.back());
    }

    // Each queue's submissions in order; the hook applies a submission's page delta just before it runs.
    std::map<std::uint32_t, std::vector<const Submit*>> byQueue;
    for (const auto* submit : submits) byQueue[submit->queue].push_back(submit);
    std::map<std::uint32_t, std::size_t> cursor;
    std::mutex cursorMutex;
    // The capture recorded submissions in one global order across queues, and each page delta's
    // previous hash follows it: a worker waits until its submission is the next one in that order.
    std::condition_variable turn;
    std::size_t globalNext = 0;
    std::atomic<std::uint64_t> outOfOrder{0};
    DeltaStats deltas;
    Capture::InstallReplay([&](std::uint32_t queue, const Capture::Context& context) {
        const Submit* submit = nullptr;
        {
            std::lock_guard lock(cursorMutex);
            auto& list = byQueue[queue];
            auto& next = cursor[queue];
            if (next < list.size()) submit = list[next++];
        }
        // Workers run at the QoS the game's guest threads give them only by inheritance; pin them to
        // user-interactive so thread CPU time does not jump between core types.
        thread_local bool raised = false;
        if (!raised) {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            raised = true;
        }
        if (submit == nullptr) return;
        const auto advance = [&] {
            {
                std::lock_guard lock(cursorMutex);
                globalNext = std::max(globalNext, submit->index + 1);
            }
            turn.notify_all();
        };
        {
            std::unique_lock lock(cursorMutex);
            // A wait the capture's order cannot satisfy (a queue blocked on this one) gives up after 5 s.
            if (!turn.wait_for(lock, std::chrono::seconds(5), [&] { return globalNext >= submit->index; })) ++outOfOrder;
        }
        if (submit->pages.empty()) return advance();
        const auto started = ThreadCpuNs();
        const auto startedInstructions = ThreadInstructions();
        std::lock_guard gpu(context.gpuMutex);
        // Restore the pages whose captured start state this submission needs: a page that already
        // holds it (its captured hash) or newer bytes the replayed GPU wrote is left alone, and so
        // is a page committed work may still store to (that store is the newer content, below).
        // Waiting for the GPU at every submission is what used to stop the worker from encoding
        // ahead of it, and most pages need no wait at all.
        std::vector<std::size_t> restore;
        for (std::size_t i = 0; i < submit->pages.size(); ++i) {
            const auto& page = submit->pages[i];
            const auto current = Capture::Hash(host(page.guest), page.raw);
            if (current == page.hash) continue;
            if (current != page.previous) {
                ++deltas.kept;
                continue;
            }
            restore.push_back(i);
        }
        if (!restore.empty()) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> overwritten;
            overwritten.reserve(restore.size());
            for (const auto index : restore) overwritten.emplace_back(submit->pages[index].guest, submit->pages[index].raw);
            std::sort(overwritten.begin(), overwritten.end());
            const auto pending = driver.PendingGuestWrites();
            if (pending.unknown) {
                // Work whose stores cannot be named (BDA pointers) is pending, so which page it may
                // write is unknown: wait as before, then re-read what landed.
                const auto waitStart = std::chrono::steady_clock::now();
                if (driver.WaitBeforeHostWrites(overwritten)) {
                    ++deltas.ordered;
                    deltas.waitNs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - waitStart).count());
                    std::erase_if(restore, [&](std::size_t index) {
                        const auto& page = submit->pages[index];
                        if (Capture::Hash(host(page.guest), page.raw) == page.previous) return false;
                        ++deltas.kept;
                        return true;
                    });
                }
            } else {
                // A page committed work may still store to is left to the GPU: that store is the
                // newer content the capture recorded (waiting for it here would stop the worker at
                // every submission).
                std::erase_if(restore, [&](std::size_t index) {
                    const auto& page = submit->pages[index];
                    if (!PendingWriteOver(pending.ranges, page.guest, page.raw)) return false;
                    ++deltas.kept;
                    return true;
                });
            }
            for (const auto index : restore) {
                const auto& page = submit->pages[index];
                Capture::Decompress(page.data, page.stored, host(page.guest), page.raw);
                ++deltas.written;
            }
        }
        deltas.cpuNs += ThreadCpuNs() - started;
        deltas.instructions += ThreadInstructions() - startedInstructions;
        advance();
    });
    Capture::EnableGpuBusy(true);
    const auto loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
    std::fprintf(stderr, "capture: %.1f MiB file, %zu ranges, %.1f MiB resident, %zu submissions in %zu frame(s), %zu shaders; loaded in %.0f ms\n",
        static_cast<double>(capture.fileBytes) / (1 << 20), capture.ranges.size(), static_cast<double>(residentBytes) / (1 << 20),
        submits.size(), frames, capture.shaders.size(), loadMs);

    struct Sample {
        double wallMs;
        double cpuMs;
        double gpuMs;
        double minstr;
        double systemMs;
        std::uint64_t hash;
    };
    std::vector<std::vector<Sample>> samples(frames);
    std::vector<std::byte> lastPixels;
    // --expect-png, read once: every repeat's last frame is compared against it.
    std::uint32_t referenceWidth = 0, referenceHeight = 0;
    const auto reference = expectPng.empty() ? std::vector<std::byte>{} : ReadPng(expectPng, referenceWidth, referenceHeight);
    std::size_t pngMismatches = 0;
    std::vector<std::vector<std::byte>> previousPixels(frames);
    const auto zeroChunk = std::vector<std::byte>(Capture::ChunkBytes);
    std::size_t liveAt = 0;
    std::uint64_t generation = 1;
    for (std::size_t rep = 0; rep < repeat; ++rep) {
        if (rep != 0) {
            // Back to the pre-frame set of ranges; ranges the frame introduced do not exist yet and read as zero.
            if (liveAt != 0 || std::any_of(capture.ranges.begin(), capture.ranges.end(), [](const Range& r) { return r.activation != 0 && r.filled; })) {
                liveAt = 0;
                setLive(0);
                driver.ReplaceBorrowedRanges(ranges, ++generation);
            }
            // Pre-frame memory: chunks that differ from the capture are written again.
            for (auto& range : capture.ranges) {
                if (range.activation != 0) range.filled = false;
                for (std::size_t c = 0; c < range.chunks.size(); ++c) {
                    const auto offset = c * Capture::ChunkBytes;
                    auto* data = range.host + offset;
                    const auto bytes = static_cast<std::size_t>(std::min<std::uint64_t>(Capture::ChunkBytes, range.bytes - offset));
                    if (const auto* chunk = range.activation == 0 ? range.chunks[c] : nullptr) {
                        if (Capture::Hash(data, bytes) != chunk->hash) Capture::Decompress(chunk->data, chunk->stored, data, chunk->raw);
                        continue;
                    }
                    std::vector<char> resident((bytes + pageSize - 1) / pageSize, 1);
                    mincore(data, bytes, resident.data());
                    if (std::none_of(resident.begin(), resident.end(), [](char r) { return r != 0; })) continue;
                    if (std::memcmp(data, zeroChunk.data(), bytes) != 0) std::memset(data, 0, bytes);
                }
            }
        }
        for (const auto& [queue, state] : capture.states) driver.ReplaceQueueStateForReplay(queue, state);
        {
            std::lock_guard lock(cursorMutex);
            cursor.clear();
            globalNext = submits.empty() ? 0 : submits.front()->index;
        }
        std::uint32_t registry = 0xffffffffu;
        std::size_t next = 0;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            Capture::TakeGpuBusyMs();
            const auto deltaCpuBefore = deltas.cpuNs.load();
            const auto deltaInstrBefore = deltas.instructions.load();
            const auto workersBefore = Workers();
            const auto wallBefore = std::chrono::steady_clock::now();
            for (; next < submits.size() && submits[next]->frame == frame; ++next) {
                const auto* submit = submits[next];
                if (submit->index >= timeline.NextChange(liveAt)) {
                    // A range the capture recorded just before this submission: it exists from here on,
                    // with its contents at this point, and not for the submissions before.
                    driver.WaitIdle();
                    liveAt = submit->index;
                    setLive(liveAt);
                    for (std::size_t r = 0; r < capture.ranges.size(); ++r) {
                        auto& range = capture.ranges[r];
                        if (!range.live || range.activation == 0 || range.filled) continue;
                        for (const auto* chunk : range.chunks)
                            if (chunk != nullptr) Capture::Decompress(chunk->data, chunk->stored, range.host + (chunk->guest - range.guest), chunk->raw);
                        range.filled = true;
                    }
                    driver.ReplaceBorrowedRanges(ranges, ++generation);
                    registry = 0xffffffffu;
                }
                if (submit->registry != registry && capture.registries.count(submit->registry) != 0) {
                    driver.ReplaceShaderRegistryForReplay(capture.registries.at(submit->registry));
                    registry = submit->registry;
                }
                if (submit->count != 0) std::memcpy(host(submit->address), submit->words, std::size_t{submit->count} * 4);
                driver.SubmitCommandBuffer(submit->address, submit->count, 0, submit->queue);
            }
            driver.WaitIdle();
            const auto wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wallBefore).count();
            const auto workersAfter = Workers();
            const auto cpuMs = static_cast<double>(workersAfter.cpuNs - workersBefore.cpuNs - (deltas.cpuNs.load() - deltaCpuBefore)) / 1e6;
            const auto gpuMs = Capture::TakeGpuBusyMs();
            const auto minstr = static_cast<double>(workersAfter.instructions - workersBefore.instructions - (deltas.instructions.load() - deltaInstrBefore)) / 1e6;
            std::uint64_t hash = 0;
            // Timing runs read no pixels: the readback is not part of the frame the replay measures.
            if (std::getenv("ANYPS5_REPLAY_NO_PIXELS") == nullptr) {
                if (const auto display = capture.displays.find(static_cast<std::uint32_t>(frame)); display != capture.displays.end()) {
                    NativeGuestMemory::BorrowedRangesScope scope(ranges);
                    lastPixels = Metal::MetalPresentation::ReadPixels(display->second.buffer);
                    hash = Capture::Hash(lastPixels.data(), lastPixels.size());
                }
            }
            const auto systemMs = static_cast<double>(workersAfter.systemNs - workersBefore.systemNs) / 1e6;
            samples[frame].push_back({wallMs, cpuMs, gpuMs, minstr, systemMs, hash});
            if (rep != 0 && hash != samples[frame][rep - 1].hash) {
                const auto display = capture.displays.at(static_cast<std::uint32_t>(frame));
                const auto diff = Compare(previousPixels[frame], lastPixels, display.buffer.width);
                std::printf("  repeat %zu differs from repeat %zu: %zu pixels in colour (max delta %d) in x %u..%u y %u..%u, %zu in alpha\n", rep + 1,
                    rep, diff.colour, diff.maxDelta, diff.x0, diff.x1, diff.y0, diff.y1, diff.alpha);
            }
            previousPixels[frame] = lastPixels;
            if (!reference.empty() && frame + 1 == frames && reference.size() == lastPixels.size()) {
                const auto diff = Compare(reference, lastPixels, referenceWidth);
                const bool ok = diff.colour <= maxDiffPixels;
                if (rep != 0 && !ok) ++pngMismatches;
                std::printf("  repeat %zu vs expect-png: %zu pixels differ (max delta %d)%s\n", rep + 1, diff.colour, diff.maxDelta,
                    rep == 0 ? " (warm-up, not judged)" : ok ? "" : ": MISMATCH");
            }
            if (!quiet)
                std::printf("repeat %zu frame %zu: wall %.2f ms  worker cpu %.2f ms (%.2f system)  gpu busy %.2f ms  worker %.1f M instructions  hash %016" PRIx64 "\n",
                    rep + 1, frame, wallMs, cpuMs, systemMs, gpuMs, minstr, hash);
        }
    }

    int status = 0;
    if (outOfOrder.load() != 0)
        std::printf("warning: %llu submissions ran out of the captured cross-queue order\n", static_cast<unsigned long long>(outOfOrder.load()));
    std::printf("page deltas: %llu written, %llu kept (replayed GPU work wrote them), %llu of the writes ordered against pending GPU work (%.1f ms), %.1f ms CPU (excluded)\n",
        static_cast<unsigned long long>(deltas.written.load()), static_cast<unsigned long long>(deltas.kept.load()),
        static_cast<unsigned long long>(deltas.ordered.load()), static_cast<double>(deltas.waitNs.load()) / 1e6 / static_cast<double>(repeat),
        static_cast<double>(deltas.cpuNs.load()) / 1e6 / static_cast<double>(repeat));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto& all = samples[frame];
        std::vector<Sample> timed(all.begin() + (all.size() > 1 ? 1 : 0), all.end());
        std::vector<double> wall, cpu, gpu, instr;
        for (const auto& sample : timed) {
            wall.push_back(sample.wallMs);
            cpu.push_back(sample.cpuMs);
            gpu.push_back(sample.gpuMs);
            instr.push_back(sample.minstr);
        }
        const auto spread = [](const std::vector<double>& v) {
            const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
            const auto median = Median(v);
            return median > 0 ? 100.0 * (*hi - *lo) / median : 0.0;
        };
        // Interquartile range over the median: the spread of the middle half of the repeats.
        const auto iqr = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            const auto median = v[v.size() / 2];
            return median > 0 ? 100.0 * (v[(v.size() * 3) / 4] - v[v.size() / 4]) / median : 0.0;
        };
        const auto range = [](const std::vector<double>& v) {
            const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
            return std::make_pair(*lo, *hi);
        };
        const auto [wallLo, wallHi] = range(wall);
        const auto [cpuLo, cpuHi] = range(cpu);
        const auto [gpuLo, gpuHi] = range(gpu);
        std::printf("frame %zu over %zu timed repeats (first is warm-up, %.1f ms wall):\n", frame, timed.size(), all.front().wallMs);
        const auto [instrLo, instrHi] = range(instr);
        std::printf("  worker cpu ms  median %.2f  min %.2f  max %.2f  spread %.1f%%  iqr %.1f%%\n", Median(cpu), cpuLo, cpuHi, spread(cpu), iqr(cpu));
        std::printf("  wall ms        median %.2f  min %.2f  max %.2f  spread %.1f%%  iqr %.1f%%\n", Median(wall), wallLo, wallHi, spread(wall), iqr(wall));
        std::printf("  gpu busy ms    median %.2f  min %.2f  max %.2f  spread %.1f%%  iqr %.1f%%\n", Median(gpu), gpuLo, gpuHi, spread(gpu), iqr(gpu));
        std::printf("  worker M instr median %.1f  min %.1f  max %.1f  spread %.1f%%  iqr %.1f%%\n", Median(instr), instrLo, instrHi,
            spread(instr), iqr(instr));
        std::set<std::uint64_t> hashes;
        for (const auto& sample : timed) hashes.insert(sample.hash);
        const auto replayed = all.back().hash;
        std::printf("  hash %016" PRIx64 " %s\n", replayed, hashes.size() == 1 ? "(stable over the timed repeats)" : "(UNSTABLE across timed repeats)");
        if (hashes.size() != 1) status = 1;
        if (const auto display = capture.displays.find(static_cast<std::uint32_t>(frame)); display != capture.displays.end()) {
            const auto& shown = display->second;
            std::printf("  game showed %016" PRIx64 ": %s\n", shown.hash, shown.hash == replayed ? "replay matches the game" : "replay differs from the game");
            if (shown.hash != replayed && frame + 1 == frames && lastPixels.size() == shown.raw) {
                std::vector<std::byte> game(shown.raw);
                Capture::Decompress(shown.data, shown.stored, game.data(), shown.raw);
                const auto diff = Compare(game, lastPixels, shown.buffer.width);
                std::printf("  vs game: %zu pixels differ in colour (%.4f%%, max delta %d) in x %u..%u y %u..%u, %zu in alpha\n", diff.colour,
                    100.0 * static_cast<double>(diff.colour) / static_cast<double>(game.size() / 4), diff.maxDelta, diff.x0, diff.x1, diff.y0, diff.y1,
                    diff.alpha);
            }
        }
    }
    if (!expect.empty()) {
        const auto wanted = std::strtoull(expect.c_str(), nullptr, 16);
        bool all = true;
        const auto& last = samples[frames - 1];
        for (std::size_t i = last.size() > 1 ? 1 : 0; i < last.size(); ++i) all = all && last[i].hash == wanted;
        std::printf("expect-hash %016llx: %s\n", static_cast<unsigned long long>(wanted), all ? "match" : "MISMATCH");
        if (!all) status = 1;
    }
    if (!expectPng.empty()) {
        const auto width = referenceWidth, height = referenceHeight;
        const auto display = capture.displays.find(static_cast<std::uint32_t>(frames - 1));
        if (reference.empty() || display == capture.displays.end() || width != display->second.buffer.width || height != display->second.buffer.height ||
            reference.size() != lastPixels.size()) {
            std::printf("expect-png %s: cannot compare (unreadable or a different size)\n", expectPng.c_str());
            status = 1;
        } else {
            const auto diff = Compare(reference, lastPixels, width);
            // With one repeat there is only the warm-up, which is reported but not judged.
            const bool ok = (repeat < 2 || diff.colour <= maxDiffPixels) && pngMismatches == 0;
            std::printf("expect-png %s: last repeat %zu pixels differ (max delta %d), allowed %zu; %zu timed repeats over: %s\n", expectPng.c_str(),
                diff.colour, diff.maxDelta, maxDiffPixels, pngMismatches, ok ? "match" : "MISMATCH");
            if (!ok) status = 1;
        }
    }
    if (!dumpPng.empty()) {
        const auto display = capture.displays.find(static_cast<std::uint32_t>(frames - 1));
        if (display == capture.displays.end() || !Capture::WriteBgraPng(dumpPng, lastPixels, display->second.buffer.width, display->second.buffer.height))
            std::fprintf(stderr, "anyps5_agc_replay: could not write %s\n", dumpPng.c_str());
        else std::printf("wrote %s\n", dumpPng.c_str());
    }
    std::fflush(stdout);
    AgcDriver::Metal::FlushMetalPipelineArchives();
    AgcDriver::Metal::FlushMetalPipelineManifest();
    if (psoStats) {
        const auto cache = AgcDriver::Metal::GetMetalPipelineCacheStatistics();
        const auto manifest = AgcDriver::Metal::GetMetalPipelineManifestStatistics();
        const auto ms = [](std::uint64_t micros) { return static_cast<double>(micros) / 1000.0; };
        std::printf("pso: library compiles %llu (%.1f ms), pipeline creations %llu (%.1f ms)\n",
            static_cast<unsigned long long>(cache.libraryCompiles), ms(cache.libraryCompileMicros),
            static_cast<unsigned long long>(cache.pipelineCreations), ms(cache.pipelineCreationMicros));
        std::printf("pso: draw-thread library compiles %llu (%.1f ms), draw-thread pipeline creations %llu (%.1f ms)\n",
            static_cast<unsigned long long>(cache.drawThreadLibraryCompiles), ms(cache.drawThreadLibraryCompileMicros),
            static_cast<unsigned long long>(cache.drawThreadCreations), ms(cache.drawThreadCreationMicros));
        std::printf("pso: render misses %llu hits %llu, compute misses %llu hits %llu\n",
            static_cast<unsigned long long>(cache.renderPipelines.misses), static_cast<unsigned long long>(cache.renderPipelines.hits),
            static_cast<unsigned long long>(cache.computePipelines.misses), static_cast<unsigned long long>(cache.computePipelines.hits));
        std::printf("pso: manifest entries %llu loaded %llu replayed %llu failed %llu (%.1f ms)\n",
            static_cast<unsigned long long>(manifest.entries), static_cast<unsigned long long>(manifest.loaded),
            static_cast<unsigned long long>(manifest.replayed), static_cast<unsigned long long>(manifest.failed), ms(manifest.micros));
        std::fflush(stdout);
    }
    // The driver's workers and Metal objects are not torn down: exit directly.
    _exit(status);
}
