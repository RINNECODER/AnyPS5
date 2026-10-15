#include "AgcFrameCapture.hpp"
#include "AgcCaptureLogic.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderCapture.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/MetalPresentation.hpp"
#include "prx/libSceAgcDriver/Graphics/Metal/GpuBusy.hpp"
#include <compression.h>
#include <dispatch/dispatch.h>
#include <sys/mman.h>
#include <unistd.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>

namespace AgcDriver::Metal::AgcCapture {

namespace {

constexpr compression_algorithm Algorithm = COMPRESSION_LZ4;

std::atomic<bool> active{false};

std::uint64_t ZeroHash(std::size_t bytes) {
    static const std::vector<std::byte> zeros(ChunkBytes);
    // Whole pages and chunks are the common sizes: hashed once, read without a lock.
    static const std::uint64_t page = Hash(zeros.data(), PageBytes);
    static const std::uint64_t chunk = Hash(zeros.data(), ChunkBytes);
    if (bytes == PageBytes) return page;
    if (bytes == ChunkBytes) return chunk;
    return Hash(zeros.data(), bytes);
}

void WriteRegisters(Writer& out, const Registers& registers) {
    out.Put(static_cast<std::uint32_t>(registers.size()));
    registers.ForEach([&](std::uint32_t offset, std::uint32_t value) {
        out.Put(offset);
        out.Put(value);
    });
}

Registers ReadRegisters(Reader& in) {
    Registers registers;
    const auto count = in.Get<std::uint32_t>();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto offset = in.Get<std::uint32_t>();
        registers.insert_or_assign(offset, in.Get<std::uint32_t>());
    }
    return registers;
}

// Residency per host page of [begin, begin + bytes): nonzero when the page may hold data. Any flag
// counts, including MINCORE_PAGED_OUT, which xnu reports for pages held by the compressor or the
// swap pager; only a page the process never populated reads as 0, and its contents are zero.
// ANYPS5_AGC_CAPTURE_READ_ALL=1 reads every page instead.
std::vector<char> Residency(const std::byte* begin, std::size_t bytes, std::uintptr_t& base, std::size_t& page) {
    page = static_cast<std::size_t>(getpagesize());
    base = reinterpret_cast<std::uintptr_t>(begin) & ~(static_cast<std::uintptr_t>(page) - 1);
    const auto end = reinterpret_cast<std::uintptr_t>(begin) + bytes;
    std::vector<char> vec((end - base + page - 1) / page, 1);
    static const bool readAll = [] {
        const char* value = std::getenv("ANYPS5_AGC_CAPTURE_READ_ALL");
        return value != nullptr && *value == '1';
    }();
    if (readAll) return vec;
    // The kernel walks every page of the range (about 1 s per 13 GiB on one thread): split it.
    constexpr std::size_t SlicePages = 16384;
    const auto pages = vec.size();
    auto* out = vec.data();
    const auto pageBytes = page;
    __block bool failed = false;
    dispatch_apply((pages + SlicePages - 1) / SlicePages, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(std::size_t slice) {
        const auto first = slice * SlicePages;
        const auto count = std::min(SlicePages, pages - first);
        const auto from = base + first * pageBytes;
        const auto bytes = std::min<std::uintptr_t>(count * pageBytes, end - from);
        if (mincore(reinterpret_cast<void*>(from), bytes, out + first) != 0) failed = true;
    });
    if (failed) std::fill(vec.begin(), vec.end(), 1);
    return vec;
}

bool AnyResident(const std::vector<char>& vec, std::uintptr_t base, std::size_t page, const std::byte* begin, std::size_t bytes) {
    const auto first = (reinterpret_cast<std::uintptr_t>(begin) - base) / page;
    const auto last = (reinterpret_cast<std::uintptr_t>(begin) + bytes - 1 - base) / page;
    for (auto i = first; i <= last && i < vec.size(); ++i)
        if (vec[i] != 0) return true;
    return false;
}

}

bool WriteBgraPng(const std::filesystem::path& path, const std::vector<std::byte>& pixels, std::uint32_t width, std::uint32_t height) {
    if (pixels.size() != std::size_t{width} * height * 4) return false;
    CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(pixels.data()), static_cast<CFIndex>(pixels.size()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    // Scanout pixels are BGRA8 in memory (little-endian 32-bit ARGB, alpha ignored).
    CGImageRef image = CGImageCreate(width, height, 8, 32, std::size_t{width} * 4, space,
        static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst), provider, nullptr, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
        static_cast<CFIndex>(path.string().size()), false);
    CGImageDestinationRef destination = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    bool ok = false;
    if (destination != nullptr && image != nullptr) {
        CGImageDestinationAddImage(destination, image, nullptr);
        ok = CGImageDestinationFinalize(destination);
    }
    if (destination != nullptr) CFRelease(destination);
    CFRelease(url);
    if (image != nullptr) CGImageRelease(image);
    CGDataProviderRelease(provider);
    CFRelease(data);
    CGColorSpaceRelease(space);
    return ok;
}

namespace {

class Capture {
public:
    static Capture* Create() {
        const char* dir = std::getenv("ANYPS5_AGC_CAPTURE");
        if (dir == nullptr || *dir == '\0') return nullptr;
        auto* capture = new Capture();
        capture->dir = dir;
        if (const char* flip = std::getenv("ANYPS5_AGC_CAPTURE_FLIP"); flip != nullptr && *flip != '\0')
            capture->target = std::max<std::uint64_t>(2, std::strtoull(flip, nullptr, 10));
        if (const char* after = std::getenv("ANYPS5_AGC_CAPTURE_AFTER_S"); after != nullptr && *after != '\0')
            capture->afterSeconds = std::strtod(after, nullptr);
        if (const char* frames = std::getenv("ANYPS5_AGC_CAPTURE_FRAMES"); frames != nullptr && *frames != '\0')
            capture->frames = std::max<std::uint64_t>(1, std::strtoull(frames, nullptr, 10));
        std::fprintf(stderr, "[agc-capture] armed: flip %llu (or the first after %.0f s), %llu frame(s), into %s\n",
            static_cast<unsigned long long>(capture->target), capture->afterSeconds, static_cast<unsigned long long>(capture->frames), dir);
        return capture;
    }

    void Before(const DriverDetail::Submission& submission, const QueueState& state, const Context& context) {
        if (!recording.load(std::memory_order_acquire)) return;
        std::lock_guard gpu(context.gpuMutex);
        std::lock_guard lock(mutex);
        if (phase != Phase::Recording || submission.suspend) return;
        context.drain();
        const auto started = std::chrono::steady_clock::now();
        if (queuesWithState.insert(submission.queue).second) {
            Writer record;
            record.Put(submission.queue);
            WriteQueueState(record, state);
            Emit(Tag::QueueState, record);
        }
        const auto registry = RegistryId(submission.shaders);
        Writer record;
        record.Put(submission.queue);
        record.Put(registry);
        record.Put(submission.commandAddress);
        record.Put(static_cast<std::uint32_t>(submission.commands.size()));
        const auto pagesAt = record.bytes.size();
        record.Put(std::uint32_t{0});
        record.Put(static_cast<std::uint32_t>(FrameOf()));
        record.Put(std::uint32_t{0});
        record.PutBytes(submission.commands.data(), submission.commands.size() * sizeof(std::uint32_t));
        const auto pages = Deltas(context, record);
        std::memcpy(record.bytes.data() + pagesAt, &pages, sizeof pages);
        Emit(Tag::Submit, record);
        ++submissions;
        deltaPages += pages;
        const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        stallMs += ms;
        std::fprintf(stderr, "[agc-capture] submission %llu: queue %u, %zu words, %u changed pages, %.0f ms\n",
            static_cast<unsigned long long>(submissions), submission.queue, submission.commands.size(), pages, ms);
    }

    void After(const DriverDetail::Submission& submission, const Context& context) {
        if (submission.flips.empty()) return;
        std::lock_guard gpu(context.gpuMutex);
        std::lock_guard lock(mutex);
        flips += submission.flips.size();
        if (phase == Phase::Waiting && afterSeconds > 0) {
            // Time trigger: the next flip after the delay is the captured frame.
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - loaded).count();
            if (elapsed < afterSeconds) return;
            target = flips + 1;
            afterSeconds = 0;
        }
        if (phase == Phase::Waiting && flips >= target - 1) {
            context.drain();
            Begin(context);
        } else if (phase == Phase::Recording && flips >= target - 1 + frames) {
            phase = Phase::AwaitPresent;
            recording.store(false, std::memory_order_release);
            std::fprintf(stderr, "[agc-capture] recorded %llu submissions (%llu delta pages, %.0f ms stalled); waiting for the present\n",
                static_cast<unsigned long long>(submissions), static_cast<unsigned long long>(deltaPages), stallMs);
            Finish();
        }
    }

    void Present(const DisplayBuffer* buffer, const std::vector<std::byte>* pixels, const Context& context) {
        std::lock_guard lock(mutex);
        // A present in the window without a buffer (a blanked output) keeps its slot and counts
        // towards completion; it only has no Display record.
        const auto step = presentTracker.Observe(target, frames, phase != Phase::Done && phase != Phase::Waiting, buffer != nullptr);
        if (!step.inWindow) return;
        const auto presents = presentTracker.Presents();
        if (!step.recordDisplay) {
            std::fprintf(stderr, "[agc-capture] present %llu showed no buffer: frame %u has no display\n",
                static_cast<unsigned long long>(presents), step.frame);
            return Complete(step);
        }
        std::vector<std::byte> read;
        if (pixels == nullptr || pixels->empty()) {
            NativeGuestMemory::BorrowedRangesScope scope(context.ranges);
            read = MetalPresentation::ReadPixels(*buffer);
            pixels = &read;
        }
        const auto frame = step.frame;
        const auto hash = Hash(pixels->data(), pixels->size());
        Writer record;
        record.Put(frame);
        record.Put(std::uint32_t{0});
        record.Put(*buffer);
        record.Put(hash);
        const auto stored = Compress(pixels->data(), pixels->size());
        record.Put(static_cast<std::uint32_t>(pixels->size()));
        record.Put(static_cast<std::uint32_t>(stored.size()));
        record.PutBytes(stored.data(), stored.size());
        Emit(Tag::Display, record);
        const auto png = dir / ("game-" + std::to_string(frame) + ".png");
        const bool wrote = WriteBgraPng(png, *pixels, buffer->width, buffer->height);
        std::fprintf(stderr, "[agc-capture] frame %u shown by present %llu: %ux%u format 0x%llx tiling %u hash %016llx%s\n", frame,
            static_cast<unsigned long long>(presents), buffer->width, buffer->height, static_cast<unsigned long long>(buffer->pixelFormat),
            buffer->tilingMode, static_cast<unsigned long long>(hash), wrote ? "" : " (png not written)");
        Complete(step);
    }

private:
    void Complete(const AgcCapture::PresentTracker::Step& step) {
        if (!step.complete) return;
        phase = Phase::Done;
        Writer end;
        end.Put(static_cast<std::uint32_t>(frames));
        end.Put(static_cast<std::uint32_t>(submissions));
        Emit(Tag::End, end);
        std::fclose(file);
        file = nullptr;
        std::fprintf(stderr, "[agc-capture] done: %s (%.1f MiB)\n", (dir / "frame.agc").c_str(), static_cast<double>(written) / (1 << 20));
        active.store(false);
    }

    enum class Phase { Waiting, Recording, AwaitPresent, Done };
    struct Table {
        std::uint64_t guest;
        std::byte* host;
        std::size_t bytes;
        std::vector<std::uint64_t> pages;
    };

    std::uint64_t FrameOf() const { return flips - (target - 1); }

    void Emit(Tag tag, const Writer& record) {
        if (file == nullptr) return;
        const auto value = static_cast<std::uint32_t>(tag);
        const std::uint32_t reserved = 0;
        const std::uint64_t bytes = record.bytes.size();
        std::fwrite(&value, sizeof value, 1, file);
        std::fwrite(&reserved, sizeof reserved, 1, file);
        std::fwrite(&bytes, sizeof bytes, 1, file);
        if (bytes != 0) std::fwrite(record.bytes.data(), 1, bytes, file);
        written += 16 + bytes;
    }

    void Begin(const Context& context) {
        const auto started = std::chrono::steady_clock::now();
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        file = std::fopen((dir / "frame.agc").c_str(), "wb");
        if (file == nullptr) {
            std::fprintf(stderr, "[agc-capture] cannot create %s; capture off\n", (dir / "frame.agc").c_str());
            phase = Phase::Done;
            active.store(false);
            return;
        }
        std::fwrite(Magic, 1, sizeof Magic, file);
        const auto version = Version;
        const auto algorithm = static_cast<std::uint32_t>(Algorithm);
        std::fwrite(&version, sizeof version, 1, file);
        std::fwrite(&algorithm, sizeof algorithm, 1, file);
        written = 16;
        std::uint64_t residentBytes = 0;
        std::uint64_t storedChunks = 0;
        for (const auto& range : context.ranges) SnapshotRange(range, residentBytes, storedChunks);
        phase = Phase::Recording;
        recording.store(true, std::memory_order_release);
        std::fprintf(stderr, "[agc-capture] flip %llu: snapshot of %zu ranges, %llu MiB in %llu chunks, %.1f MiB written in %.0f ms\n",
            static_cast<unsigned long long>(flips), context.ranges.size(), static_cast<unsigned long long>(residentBytes >> 20),
            static_cast<unsigned long long>(storedChunks), static_cast<double>(written) / (1 << 20),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    }

    // Records `range` and its resident, non-zero chunks, and starts its page hashes.
    void SnapshotRange(const NativeGuestMemory::BorrowedRange& range, std::uint64_t& residentBytes, std::uint64_t& storedChunks) {
        Writer record;
        record.Put(range.guestAddress);
        record.Put(static_cast<std::uint64_t>(range.host.size()));
        record.Put(static_cast<std::uint32_t>(range.writable));
        record.Put(std::uint32_t{0});
        Emit(Tag::Range, record);
        Table table{range.guestAddress, range.host.data(), range.host.size(), {}};
        table.pages.resize((range.host.size() + PageBytes - 1) / PageBytes);
        std::uintptr_t base = 0;
        std::size_t page = 0;
        const auto resident = Residency(range.host.data(), range.host.size(), base, page);
        const auto chunks = (range.host.size() + ChunkBytes - 1) / ChunkBytes;
        // Batches keep at most 256 compressed chunks in memory at once.
        constexpr std::size_t Batch = 256;
        for (std::size_t first = 0; first < chunks; first += Batch) {
            const auto count = std::min<std::size_t>(Batch, chunks - first);
            std::vector<std::vector<std::byte>> stored(count);
            std::vector<std::uint64_t> hashes(count);
            std::vector<char> present(count, 0);
            auto* storedOut = stored.data();
            auto* hashesOut = hashes.data();
            auto* presentOut = present.data();
            auto* pagesOut = table.pages.data();
            const auto* residentIn = &resident;
            const auto* host = range.host.data();
            const auto hostBytes = range.host.size();
            dispatch_apply(count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(std::size_t i) {
                const auto offset = (first + i) * ChunkBytes;
                const auto bytes = std::min<std::size_t>(ChunkBytes, hostBytes - offset);
                const auto* data = host + offset;
                for (std::size_t p = 0; p < bytes; p += PageBytes) {
                    const auto pageBytes = std::min<std::size_t>(PageBytes, bytes - p);
                    pagesOut[(offset + p) / PageBytes] = AnyResident(*residentIn, base, page, data + p, pageBytes)
                        ? Hash(data + p, pageBytes) : ZeroHash(pageBytes);
                }
                if (!AnyResident(*residentIn, base, page, data, bytes)) return;
                hashesOut[i] = Hash(data, bytes);
                if (hashesOut[i] == ZeroHash(bytes) && std::all_of(data, data + bytes, [](std::byte b) { return b == std::byte{0}; })) return;
                presentOut[i] = 1;
                storedOut[i] = Compress(data, bytes);
            });
            for (std::size_t i = 0; i < count; ++i) {
                if (!present[i]) continue;
                const auto offset = (first + i) * ChunkBytes;
                const auto bytes = std::min<std::size_t>(ChunkBytes, range.host.size() - offset);
                Writer chunk;
                chunk.Put(range.guestAddress + offset);
                chunk.Put(static_cast<std::uint32_t>(bytes));
                chunk.Put(static_cast<std::uint32_t>(stored[i].size()));
                chunk.Put(hashes[i]);
                chunk.PutBytes(stored[i].data(), stored[i].size());
                Emit(Tag::Chunk, chunk);
                residentBytes += bytes;
                ++storedChunks;
            }
        }
        tables.push_back(std::move(table));
    }

    // Appends the pages that changed since the previous checkpoint to `record`; returns their count.
    std::uint32_t Deltas(const Context& context, Writer& record) {
        std::uint32_t count = 0;
        // A range the title mapped after the snapshot: its contents now are its initial state.
        for (const auto& range : context.ranges) {
            const bool known = std::any_of(tables.begin(), tables.end(), [&](const Table& table) {
                return table.guest == range.guestAddress && table.host == range.host.data() && table.bytes == range.host.size();
            });
            if (known) continue;
            std::uint64_t residentBytes = 0;
            std::uint64_t storedChunks = 0;
            SnapshotRange(range, residentBytes, storedChunks);
            std::fprintf(stderr, "[agc-capture] range 0x%llx (+0x%zx) mapped during the frame: %llu MiB recorded\n",
                static_cast<unsigned long long>(range.guestAddress), range.host.size(), static_cast<unsigned long long>(residentBytes >> 20));
        }
        for (auto& table : tables) {
            // A range that is no longer borrowed is not read by this submission.
            const auto found = std::find_if(context.ranges.begin(), context.ranges.end(), [&](const auto& range) {
                return range.guestAddress == table.guest && range.host.data() == table.host && range.host.size() == table.bytes;
            });
            if (found == context.ranges.end()) continue;
            std::uintptr_t base = 0;
            std::size_t page = 0;
            const auto resident = Residency(table.host, table.bytes, base, page);
            const auto pages = table.pages.size();
            std::vector<std::uint64_t> hashes(pages);
            auto* hashOut = hashes.data();
            auto* tableRef = &table;
            const auto* residentRef = &resident;
            dispatch_apply((pages + 63) / 64, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(std::size_t group) {
                for (std::size_t p = group * 64; p < std::min(pages, group * 64 + 64); ++p) {
                    const auto offset = p * PageBytes;
                    const auto bytes = std::min<std::size_t>(PageBytes, tableRef->bytes - offset);
                    const auto* data = tableRef->host + offset;
                    hashOut[p] = AnyResident(*residentRef, base, page, data, bytes) ? Hash(data, bytes) : ZeroHash(bytes);
                }
            });
            for (std::size_t p = 0; p < pages; ++p) {
                if (hashes[p] == table.pages[p]) continue;
                const auto offset = p * PageBytes;
                const auto bytes = std::min<std::size_t>(PageBytes, table.bytes - offset);
                const auto stored = Compress(table.host + offset, bytes);
                record.Put(table.guest + offset);
                record.Put(static_cast<std::uint32_t>(bytes));
                record.Put(static_cast<std::uint32_t>(stored.size()));
                record.Put(table.pages[p]);
                record.Put(hashes[p]);
                record.PutBytes(stored.data(), stored.size());
                table.pages[p] = hashes[p];
                ++count;
            }
        }
        return count;
    }

    std::uint32_t RegistryId(const std::shared_ptr<const DriverDetail::ShaderRegistry>& registry) {
        if (registry == nullptr) return 0xffffffffu;
        if (const auto found = registryIds.find(registry.get()); found != registryIds.end()) return found->second;
        static const auto nullPixel = DriverDetail::NullPixelProgramAddress();
        Writer record;
        const auto id = static_cast<std::uint32_t>(registryIds.size());
        std::vector<std::pair<std::uint64_t, std::uint32_t>> entries;
        for (const auto& [key, snapshots] : *registry) {
            if (key == nullPixel) continue;
            for (const auto& snapshot : snapshots) entries.emplace_back(key, ShaderId(snapshot));
        }
        record.Put(id);
        record.Put(static_cast<std::uint32_t>(entries.size()));
        for (const auto& [key, shader] : entries) {
            record.Put(key);
            record.Put(shader);
        }
        Emit(Tag::Registry, record);
        registryIds.emplace(registry.get(), id);
        keepRegistries.push_back(registry);
        return id;
    }

    std::uint32_t ShaderId(const std::shared_ptr<const DriverDetail::ShaderSnapshot>& snapshot) {
        if (const auto found = shaderIds.find(snapshot.get()); found != shaderIds.end()) return found->second;
        const auto id = static_cast<std::uint32_t>(shaderIds.size());
        Writer record;
        record.Put(id);
        record.Put(static_cast<std::uint32_t>(snapshot->type));
        record.Put(snapshot->codeAddress);
        record.Put(snapshot->headerAddress);
        record.Put(static_cast<std::uint32_t>(snapshot->code.size()));
        record.Put(static_cast<std::uint32_t>(snapshot->header.size()));
        record.PutBytes(snapshot->code.data(), snapshot->code.size() * sizeof(std::uint32_t));
        record.PutBytes(snapshot->header.data(), snapshot->header.size());
        Emit(Tag::Shader, record);
        shaderIds.emplace(snapshot.get(), id);
        keepShaders.push_back(snapshot);
        return id;
    }

    void Finish() { std::fflush(file); }

    std::mutex mutex;
    std::filesystem::path dir;
    std::FILE* file = nullptr;
    std::uint64_t target = 600;
    std::uint64_t frames = 1;
    std::uint64_t flips = 0;
    PresentTracker presentTracker;
    std::uint64_t submissions = 0;
    std::uint64_t deltaPages = 0;
    std::uint64_t written = 0;
    double stallMs = 0;
    double afterSeconds = 0;
    std::chrono::steady_clock::time_point loaded = std::chrono::steady_clock::now();
    Phase phase = Phase::Waiting;
    std::atomic<bool> recording{false};
    std::vector<Table> tables;
    std::set<std::uint32_t> queuesWithState;
    std::map<const void*, std::uint32_t> registryIds;
    std::map<const void*, std::uint32_t> shaderIds;
    std::vector<std::shared_ptr<const DriverDetail::ShaderRegistry>> keepRegistries;
    std::vector<std::shared_ptr<const DriverDetail::ShaderSnapshot>> keepShaders;
};

Capture* SharedCapture() {
    static Capture* capture = [] {
        auto* created = Capture::Create();
        if (created != nullptr) active.store(true);
        return created;
    }();
    return capture;
}

std::mutex replayMutex;
ReplayHook replayHook;

// Arms the capture at load time, so Active() is accurate from the first submission.
__attribute__((constructor)) void ArmCapture() { static_cast<void>(SharedCapture()); }

}

void WriteQueueState(Writer& out, const QueueState& state) {
    WriteRegisters(out, state.shader);
    WriteRegisters(out, state.context);
    WriteRegisters(out, state.userConfig);
    out.Put(static_cast<std::uint32_t>(state.savedContext.has_value()));
    if (state.savedContext) WriteRegisters(out, *state.savedContext);
    out.Put(static_cast<std::uint32_t>(state.constantRam.size()));
    out.PutBytes(state.constantRam.data(), state.constantRam.size() * sizeof(std::uint32_t));
    out.Put(state.indexBase);
    out.Put(state.drawIndirectBase);
    out.Put(state.dispatchIndirectBase);
    out.Put(state.indexBufferSize);
    out.Put(state.indexType);
    out.Put(state.instanceCount);
    out.Put(state.predication.address);
    out.Put(state.predication.operation);
    out.Put(static_cast<std::uint32_t>(state.predication.executeWhenSet));
}

QueueState ReadQueueState(Reader& in) {
    QueueState state;
    state.shader = ReadRegisters(in);
    state.context = ReadRegisters(in);
    state.userConfig = ReadRegisters(in);
    if (in.Get<std::uint32_t>() != 0) state.savedContext = ReadRegisters(in);
    state.constantRam.resize(in.Get<std::uint32_t>());
    std::memcpy(state.constantRam.data(), in.Take(state.constantRam.size() * sizeof(std::uint32_t)), state.constantRam.size() * sizeof(std::uint32_t));
    state.indexBase = in.Get<std::uint64_t>();
    state.drawIndirectBase = in.Get<std::uint64_t>();
    state.dispatchIndirectBase = in.Get<std::uint64_t>();
    state.indexBufferSize = in.Get<std::uint32_t>();
    state.indexType = in.Get<std::uint32_t>();
    state.instanceCount = in.Get<std::uint32_t>();
    state.predication.address = in.Get<std::uint64_t>();
    state.predication.operation = in.Get<std::uint32_t>();
    state.predication.executeWhenSet = in.Get<std::uint32_t>() != 0;
    return state;
}

std::vector<std::byte> Compress(const void* data, std::size_t bytes) {
    std::vector<std::byte> out(bytes);
    const auto size = bytes == 0 ? 0 : compression_encode_buffer(reinterpret_cast<std::uint8_t*>(out.data()), out.size(),
        static_cast<const std::uint8_t*>(data), bytes, nullptr, Algorithm);
    if (size == 0 || size >= bytes) {
        std::memcpy(out.data(), data, bytes);
        return out;
    }
    out.resize(size);
    return out;
}

void Decompress(const std::byte* stored, std::size_t storedBytes, void* target, std::size_t raw) {
    if (storedBytes == raw) {
        std::memcpy(target, stored, raw);
        return;
    }
    const auto size = compression_decode_buffer(static_cast<std::uint8_t*>(target), raw, reinterpret_cast<const std::uint8_t*>(stored),
        storedBytes, nullptr, Algorithm);
    if (size != raw) throw std::runtime_error("agc capture: a compressed block does not decode to its size");
}

bool Active() { return active.load(std::memory_order_relaxed); }

void BeforeExecute(const DriverDetail::Submission& submission, const QueueState& state, const Context& context) {
    if (!Active()) return;
    if (auto* capture = SharedCapture()) return capture->Before(submission, state, context);
    ReplayHook hook;
    {
        std::lock_guard lock(replayMutex);
        hook = replayHook;
    }
    if (hook && !submission.suspend) hook(submission.queue, context);
}

void AfterExecute(const DriverDetail::Submission& submission, const Context& context) {
    if (!Active()) return;
    if (auto* capture = SharedCapture()) capture->After(submission, context);
}

void Presented(const DisplayBuffer* buffer, const std::vector<std::byte>* pixels, const Context& context) {
    if (!Active()) return;
    if (auto* capture = SharedCapture()) capture->Present(buffer, pixels, context);
}

void InstallReplay(ReplayHook hook) {
    if (SharedCapture() != nullptr) throw std::logic_error("agc replay: a capture is armed in this process");
    std::lock_guard lock(replayMutex);
    replayHook = std::move(hook);
    active.store(static_cast<bool>(replayHook));
}

void EnableGpuBusy(bool enabled) { GpuBusy::Shared().enabled.store(enabled); }
bool GpuBusyEnabled() { return GpuBusy::Enabled(); }
void RecordGpuBusy(double gpuStart, double gpuEnd) { GpuBusy::Record(gpuStart, gpuEnd); }
double TakeGpuBusyMs() { return GpuBusy::Take(); }

}
