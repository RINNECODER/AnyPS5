#pragma once
// LOCAL PROFILING ONLY (frame-profile-2026-10-15). ANYPS5_FRAME_TRACE=<file> records command
// buffers, CPU waits, and every encoder (with GPU stage-boundary timestamps) into memory and
// writes them to <file> at exit.
#import <Metal/Metal.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

namespace AgcDriver::Metal::FrameTrace {

inline const char* Path() {
    static const char* path = std::getenv("ANYPS5_FRAME_TRACE");
    return path != nullptr && *path != '\0' ? path : nullptr;
}
inline bool Enabled() { return Path() != nullptr; }
inline bool CountersOff() {
    static const bool off = std::getenv("ANYPS5_FRAME_TRACE_NOCOUNTERS") != nullptr;
    return off;
}

inline double Now() {
    static const double scale = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return static_cast<double>(info.numer) / static_cast<double>(info.denom) / 1e9;
    }();
    return static_cast<double>(mach_absolute_time()) * scale;
}

inline std::uint64_t Tid() {
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
}

struct State {
    std::mutex mutex;
    std::vector<std::string> lines;
    std::vector<id<MTLCounterSampleBuffer>> buffers;
    NSUInteger used = 0;  // samples used in buffers.back()
    std::uint64_t seq = 0;
    id<MTLDevice> device = nil;
    std::vector<std::pair<std::uint64_t, double>> clock;  // gpu ts, host s
    bool countersOk = true;
};

void Dump();

inline State& Shared() {
    static State* state = [] {
        auto* s = new State;
        std::atexit([] { Dump(); });
        return s;
    }();
    return *state;
}

inline void Line(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    auto& s = Shared();
    std::lock_guard lock(s.mutex);
    s.lines.emplace_back(text);
}

inline void Mark(const char* what) {
    if (Enabled()) Line("M %.9f %llu %s", Now(), static_cast<unsigned long long>(Tid()), what);
}

inline void SampleClock(id<MTLDevice> device) {
    MTLTimestamp cpu = 0, gpu = 0;
    [device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    Shared().clock.push_back({gpu, Now()});
}

// Two sample indices in a shared counter buffer (caller holds no lock). Returns false if unsupported.
inline bool Reserve(id<MTLDevice> device, id<MTLCounterSampleBuffer>* buffer, NSUInteger* index, std::uint64_t* seq) {
    auto& s = Shared();
    std::lock_guard lock(s.mutex);
    *seq = ++s.seq;
    if (CountersOff() || !s.countersOk) return false;
    if (s.device == nil) {
        s.device = device;
        if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
            s.countersOk = false;
            std::fprintf(stderr, "[frame-trace] no stage-boundary counter sampling\n");
            return false;
        }
        SampleClock(device);
    }
    constexpr NSUInteger Samples = 4096;
    if (s.buffers.empty() || s.used + 2 > Samples) {
        if (s.buffers.size() >= 256) { s.countersOk = false; std::fprintf(stderr, "[frame-trace] counter buffers exhausted\n"); return false; }
        id<MTLCounterSet> timestamps = nil;
        for (id<MTLCounterSet> set in device.counterSets) {
            if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps = set;
        }
        if (timestamps == nil) {
            s.countersOk = false;
            return false;
        }
        auto descriptor = [[MTLCounterSampleBufferDescriptor alloc] init];
        descriptor.counterSet = timestamps;
        descriptor.sampleCount = Samples;
        descriptor.storageMode = MTLStorageModeShared;
        NSError* error = nil;
        id<MTLCounterSampleBuffer> made = [device newCounterSampleBufferWithDescriptor:descriptor error:&error];
        if (made == nil) {
            s.countersOk = false;
            std::fprintf(stderr, "[frame-trace] counter buffer failed: %s\n", error.localizedDescription.UTF8String);
            return false;
        }
        s.buffers.push_back(made);
        s.used = 0;
    }
    *buffer = s.buffers.back();
    *index = s.used;
    s.used += 2;
    s.lines.push_back("X " + std::to_string(*seq) + " " + std::to_string(s.buffers.size() - 1) + " " + std::to_string(*index));
    return true;
}

inline const char* LoadName(MTLLoadAction a) { return a == MTLLoadActionLoad ? "L" : a == MTLLoadActionClear ? "C" : "D"; }
inline const char* StoreName(MTLStoreAction a) {
    switch (a) {
    case MTLStoreActionStore: return "S";
    case MTLStoreActionDontCare: return "D";
    case MTLStoreActionMultisampleResolve: return "R";
    case MTLStoreActionStoreAndMultisampleResolve: return "SR";
    default: return "U";
    }
}
inline std::string Attachment(const char* slot, MTLRenderPassAttachmentDescriptor* a) {
    if (a.texture == nil) return {};
    id<MTLTexture> t = a.texture;
    id<MTLTexture> root = t.parentTexture != nil ? t.parentTexture : t;
    char text[256];
    std::snprintf(text, sizeof text, " %s:%llx:%llx:%lux%lu:f%lu:s%lu:m%lu:l%lu:%s%s:st%lu", slot,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>((__bridge void*)t)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>((__bridge void*)root)),
        static_cast<unsigned long>(t.width >> a.level), static_cast<unsigned long>(t.height >> a.level),
        static_cast<unsigned long>(t.pixelFormat), static_cast<unsigned long>(t.sampleCount),
        static_cast<unsigned long>(a.level), static_cast<unsigned long>(a.slice + a.depthPlane), LoadName(a.loadAction),
        StoreName(a.storeAction), static_cast<unsigned long>(t.storageMode));
    return text;
}

inline unsigned long long Ptr(id object) { return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>((__bridge void*)object)); }

// Log-only encoder record in the format analyze.py expects: P seq time tid cb kind site.
inline void Encoder(id<MTLCommandBuffer> commands, const char* kind, const char* site) {
    if (!Enabled()) return;
    auto& s = Shared();
    std::lock_guard lock(s.mutex);
    const std::uint64_t seq = ++s.seq;
    char text[160];
    std::snprintf(text, sizeof text, "P %llu %.9f %llu %llx %s %s", static_cast<unsigned long long>(seq), Now(),
        static_cast<unsigned long long>(Tid()), Ptr(commands), kind, site);
    s.lines.emplace_back(text);
}

inline void PrepareRender(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor* pass, const char* site) {
    if (!Enabled()) return;
    id<MTLCounterSampleBuffer> buffer = nil;
    NSUInteger index = 0;
    std::uint64_t seq = 0;
    if (Reserve(commands.device, &buffer, &index, &seq)) {
        auto sample = pass.sampleBufferAttachments[0];
        sample.sampleBuffer = buffer;
        sample.startOfVertexSampleIndex = index;
        sample.endOfVertexSampleIndex = MTLCounterDontSample;
        sample.startOfFragmentSampleIndex = MTLCounterDontSample;
        sample.endOfFragmentSampleIndex = index + 1;
    }
    std::string text;
    for (NSUInteger i = 0; i < 8; ++i) {
        char slot[8];
        std::snprintf(slot, sizeof slot, "c%lu", static_cast<unsigned long>(i));
        auto a = pass.colorAttachments[i];
        text += Attachment(slot, a);
        if (a.resolveTexture != nil) text += std::string(" r") + std::to_string(i) + ":" + std::to_string(a.resolveTexture.width) + "x" + std::to_string(a.resolveTexture.height);
    }
    text += Attachment("d", pass.depthAttachment);
    text += Attachment("s", pass.stencilAttachment);
    Line("P %llu %.9f %llu %llx render %s rt%lux%lu a%lu vis%d%s", static_cast<unsigned long long>(seq), Now(),
        static_cast<unsigned long long>(Tid()), Ptr(commands), site, static_cast<unsigned long>(pass.renderTargetWidth),
        static_cast<unsigned long>(pass.renderTargetHeight), static_cast<unsigned long>(pass.renderTargetArrayLength),
        pass.visibilityResultBuffer != nil ? 1 : 0, text.c_str());
}

inline id<MTLRenderCommandEncoder> Render(id<MTLCommandBuffer> commands, MTLRenderPassDescriptor* pass, const char* site) {
    // Measurement hack (wrong pixels): ANYPS5_FP_LS=l|s|ls turns Load / Store into DontCare.
    static const char* hack = std::getenv("ANYPS5_FP_LS");
    if (hack != nullptr) {
        const bool l = std::strchr(hack, 'l') != nullptr, st = std::strchr(hack, 's') != nullptr;
        auto fix = [&](MTLRenderPassAttachmentDescriptor* a) {
            if (a.texture == nil) return;
            if (l && a.loadAction == MTLLoadActionLoad) a.loadAction = MTLLoadActionDontCare;
            if (st && a.storeAction == MTLStoreActionStore) a.storeAction = MTLStoreActionDontCare;
            if (st && a.storeAction == MTLStoreActionStoreAndMultisampleResolve) a.storeAction = MTLStoreActionMultisampleResolve;
        };
        for (NSUInteger i = 0; i < 8; ++i) fix(pass.colorAttachments[i]);
        fix(pass.depthAttachment);
        fix(pass.stencilAttachment);
    }
    if (!Enabled()) return [commands renderCommandEncoderWithDescriptor:pass];
    PrepareRender(commands, pass, site);
    return [commands renderCommandEncoderWithDescriptor:pass];
}

inline id<MTLComputeCommandEncoder> Compute(id<MTLCommandBuffer> commands, const char* site) {
    if (!Enabled()) return [commands computeCommandEncoder];
    id<MTLCounterSampleBuffer> buffer = nil;
    NSUInteger index = 0;
    std::uint64_t seq = 0;
    auto pass = [MTLComputePassDescriptor computePassDescriptor];
    if (Reserve(commands.device, &buffer, &index, &seq)) {
        auto sample = pass.sampleBufferAttachments[0];
        sample.sampleBuffer = buffer;
        sample.startOfEncoderSampleIndex = index;
        sample.endOfEncoderSampleIndex = index + 1;
    }
    Line("P %llu %.9f %llu %llx compute %s", static_cast<unsigned long long>(seq), Now(), static_cast<unsigned long long>(Tid()), Ptr(commands), site);
    return [commands computeCommandEncoderWithDescriptor:pass];
}

inline id<MTLBlitCommandEncoder> Blit(id<MTLCommandBuffer> commands, const char* site) {
    if (!Enabled()) return [commands blitCommandEncoder];
    id<MTLCounterSampleBuffer> buffer = nil;
    NSUInteger index = 0;
    std::uint64_t seq = 0;
    auto pass = [MTLBlitPassDescriptor blitPassDescriptor];
    if (Reserve(commands.device, &buffer, &index, &seq)) {
        auto sample = pass.sampleBufferAttachments[0];
        sample.sampleBuffer = buffer;
        sample.startOfEncoderSampleIndex = index;
        sample.endOfEncoderSampleIndex = index + 1;
    }
    Line("P %llu %.9f %llu %llx blit %s", static_cast<unsigned long long>(seq), Now(), static_cast<unsigned long long>(Tid()), Ptr(commands), site);
    return [commands blitCommandEncoderWithDescriptor:pass];
}

// A new command buffer: log creation and, at completion, its scheduling and GPU times.
inline void Created(id<MTLCommandBuffer> commands) {
    if (!Enabled()) return;
    Line("C %llx %.9f %llu", Ptr(commands), Now(), static_cast<unsigned long long>(Tid()));
    const auto ptr = Ptr(commands);
    [commands addScheduledHandler:^(id<MTLCommandBuffer>) { Line("Q %llx %.9f", ptr, Now()); }];
    [commands addCompletedHandler:^(id<MTLCommandBuffer> done) {
        Line("D %llx %.9f %.9f %.9f %.9f %.9f %ld", ptr, done.kernelStartTime, done.kernelEndTime, done.GPUStartTime, done.GPUEndTime, Now(),
            static_cast<long>(done.status));
    }];
}

inline void Committing(id<MTLCommandBuffer> commands, const char* site) {
    if (Enabled()) Line("K %llx %.9f %llu %s", Ptr(commands), Now(), static_cast<unsigned long long>(Tid()), site);
}

// A CPU wait on the GPU. `caller` is a return address, symbolized offline.
inline void Waited(std::uint32_t cause, double start, double end, const void* caller = nullptr) {
    if (!Enabled()) return;
    Dl_info info{};
    unsigned long long offset = 0;
    const char* file = "?";
    if (caller != nullptr && dladdr(caller, &info) != 0) {
        offset = reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
        file = info.dli_fname;
    }
    Line("W %x %.9f %.9f %llu %s+0x%llx", cause, start, end, static_cast<unsigned long long>(Tid()), file, offset);
}

inline void Dump() {
    const char* path = Path();
    if (path == nullptr) return;
    auto& s = Shared();
    std::lock_guard lock(s.mutex);
    if (s.device != nil && s.countersOk) SampleClock(s.device);
    FILE* out = std::fopen(path, "w");
    if (out == nullptr) return;
    for (const auto& [gpu, host] : s.clock) std::fprintf(out, "T %llu %.9f\n", static_cast<unsigned long long>(gpu), host);
    for (std::size_t b = 0; b < s.buffers.size(); ++b) {
        const NSUInteger count = b + 1 == s.buffers.size() ? s.used : 4096;
        if (count == 0) continue;
        NSData* data = [s.buffers[b] resolveCounterRange:NSMakeRange(0, count)];
        if (data == nil) {
            std::fprintf(out, "E resolve failed %zu\n", b);
            continue;
        }
        const auto* values = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
        const std::size_t n = data.length / sizeof(MTLCounterResultTimestamp);
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            std::fprintf(out, "G %zu %zu %llu %llu\n", b, i, static_cast<unsigned long long>(values[i].timestamp),
                static_cast<unsigned long long>(values[i + 1].timestamp));
        }
    }
    for (const auto& line : s.lines) {
        std::fputs(line.c_str(), out);
        std::fputc('\n', out);
    }
    std::fclose(out);
}

}
