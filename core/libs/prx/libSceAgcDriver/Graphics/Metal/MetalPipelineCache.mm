#include "MetalPipelineCache.hpp"
#include "MetalPipelineManifest.hpp"
#include "MetalWorkerPool.hpp"
#include "MetalBackend/BoundedCache.hpp"
#include "MetalBackend/MetalShaderBridge.hpp"
#include "ShaderCacheDirectory.hpp"
#include <unordered_map>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <pthread.h>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace AgcDriver::Metal {

bool FastMathEnabled() {
    const char* value = std::getenv("ANYPS5_METAL_FAST_MATH");
    if (value == nullptr) return true;
    if (*value == '0' || *value == '\0') return false;
    if (std::strcmp(value, "false") == 0 || std::strcmp(value, "FALSE") == 0 ||
        std::strcmp(value, "off") == 0 || std::strcmp(value, "OFF") == 0 ||
        std::strcmp(value, "no") == 0 || std::strcmp(value, "NO") == 0) return false;
    return true;
}

bool Force1xMsaaEnabled() {
    const char* value = std::getenv("ANYPS5_METAL_FORCE_1X_MSAA");
    if (value == nullptr) return false;
    if (*value == '1' && *(value + 1) == '\0') return true;
    if (std::strcmp(value, "true") == 0 || std::strcmp(value, "TRUE") == 0 ||
        std::strcmp(value, "on") == 0 || std::strcmp(value, "ON") == 0 ||
        std::strcmp(value, "yes") == 0 || std::strcmp(value, "YES") == 0) return true;
    return false;
}

namespace {

using ShaderRecompiler::MetalBackend::BoundedCache;
using ShaderRecompiler::MetalBackend::CacheKey;
using ShaderRecompiler::MetalBackend::CacheKeyProbe;
using ShaderRecompiler::MetalBackend::CacheKeyWriter;

constexpr NSUInteger BufferSlots = 31;
constexpr NSUInteger ColorAttachments = 8;
// Converted pipelines are batched into the archive file once this many are new, and at exit.
constexpr std::size_t ArchiveWriteBatch = 32;
constexpr std::size_t ObjectBytes = 512;

struct FunctionEntry {
    id<MTLFunction> function;
};

struct ComputeEntry {
    id<MTLComputePipelineState> state;
    MTLComputePipelineReflection* reflection;
    MTLComputePipelineDescriptor* descriptor;  // retains the keyed functions so their addresses stay unique
};

struct RenderEntry {
    id<MTLRenderPipelineState> state;
    MTLRenderPipelineReflection* reflection;
    id descriptor;
};

BoundedCache<FunctionEntry>& Functions() {
    static BoundedCache<FunctionEntry> cache(2048, std::size_t{256} << 20);
    return cache;
}

// In front of Functions(), by the identity of a converted shader's shared source text. The entry
// holds the text, so no other text can take its address while it is cached.
struct SharedFunctionEntry {
    std::shared_ptr<const std::string> source;
    id<MTLFunction> function;
};

BoundedCache<SharedFunctionEntry>& SharedFunctions() {
    static BoundedCache<SharedFunctionEntry> cache(4096, std::size_t{64} << 20);
    return cache;
}

BoundedCache<ComputeEntry>& ComputePipelines() {
    static BoundedCache<ComputeEntry> cache(2048, std::size_t{64} << 20);
    return cache;
}

BoundedCache<RenderEntry>& RenderPipelines() {
    static BoundedCache<RenderEntry> cache(4096, std::size_t{64} << 20);
    return cache;
}

BoundedCache<id<MTLDepthStencilState>>& DepthStencilStates() {
    static BoundedCache<id<MTLDepthStencilState>> cache(1024, std::size_t{16} << 20);
    return cache;
}

// Kept well under the device's limit on live argument-buffer samplers.
BoundedCache<id<MTLSamplerState>>& SamplerStates() {
    static BoundedCache<id<MTLSamplerState>> cache(512, std::size_t{16} << 20);
    return cache;
}

void Object(CacheKeyWriter& key, id object) {
    key.Value(reinterpret_cast<std::uintptr_t>((__bridge void*)object));
}

void Device(CacheKeyWriter& key, id<MTLDevice> device, char kind) {
    key.Value(kind);
    Object(key, device);
}

void Buffers(CacheKeyWriter& key, MTLPipelineBufferDescriptorArray* buffers) {
    for (NSUInteger i = 0; i < BufferSlots; ++i) key.Value(buffers[i].mutability);
}

void Colors(CacheKeyWriter& key, MTLRenderPipelineColorAttachmentDescriptorArray* colors) {
    for (NSUInteger i = 0; i < ColorAttachments; ++i) {
        const auto color = colors[i];
        key.Value(color.pixelFormat);
        key.Bool(color.blendingEnabled);
        key.Value(color.sourceRGBBlendFactor);
        key.Value(color.destinationRGBBlendFactor);
        key.Value(color.rgbBlendOperation);
        key.Value(color.sourceAlphaBlendFactor);
        key.Value(color.destinationAlphaBlendFactor);
        key.Value(color.alphaBlendOperation);
        key.Value(color.writeMask);
    }
}

void VertexLayout(CacheKeyWriter& key, MTLVertexDescriptor* vertex) {
    key.Bool(vertex != nil);
    if (vertex == nil) return;
    for (NSUInteger i = 0; i < BufferSlots; ++i) {
        const auto attribute = vertex.attributes[i];
        key.Value(attribute.format);
        key.Value(attribute.offset);
        key.Value(attribute.bufferIndex);
        const auto layout = vertex.layouts[i];
        key.Value(layout.stride);
        key.Value(layout.stepFunction);
        key.Value(layout.stepRate);
    }
}

void StageInputs(CacheKeyWriter& key, MTLStageInputOutputDescriptor* inputs) {
    key.Bool(inputs != nil);
    if (inputs == nil) return;
    for (NSUInteger i = 0; i < BufferSlots; ++i) {
        const auto attribute = inputs.attributes[i];
        key.Value(attribute.format);
        key.Value(attribute.offset);
        key.Value(attribute.bufferIndex);
        const auto layout = inputs.layouts[i];
        key.Value(layout.stride);
        key.Value(layout.stepFunction);
        key.Value(layout.stepRate);
    }
    key.Value(inputs.indexType);
    key.Value(inputs.indexBufferIndex);
}

void Stencil(CacheKeyWriter& key, MTLStencilDescriptor* stencil) {
    key.Bool(stencil != nil);
    if (stencil == nil) return;
    key.Value(stencil.stencilCompareFunction);
    key.Value(stencil.stencilFailureOperation);
    key.Value(stencil.depthFailureOperation);
    key.Value(stencil.depthStencilPassOperation);
    key.Value(stencil.readMask);
    key.Value(stencil.writeMask);
}

// Optional properties the converted path never sets but a newer SDK may expose: read by name so a
// future caller setting them still changes the key.
void OptionalNumber(CacheKeyWriter& key, id object, NSString* name) {
    const SEL selector = NSSelectorFromString(name);
    if (![object respondsToSelector:selector]) {
        key.Bool(false);
        return;
    }
    NSNumber* value = [object valueForKey:name];
    key.Bool(value != nil);
    key.Value(value == nil ? 0.0 : value.doubleValue);
}

// Pipelines that link extra functions or libraries are created uncached: their identity is not captured
// by the key below.
bool Linked(NSArray* functions) {
    return functions != nil && functions.count != 0;
}

CacheKey ComputeKey(id<MTLDevice> device, MTLComputePipelineDescriptor* descriptor) {
    CacheKeyWriter key;
    Device(key, device, 'C');
    key.Bool(FastMathEnabled());
    Object(key, descriptor.computeFunction);
    StageInputs(key, descriptor.stageInputDescriptor);
    key.Bool(descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth);
    key.Value(descriptor.maxTotalThreadsPerThreadgroup);
    key.Bool(descriptor.supportIndirectCommandBuffers);
    key.Value(descriptor.maxCallStackDepth);
    key.Bool(descriptor.supportAddingBinaryFunctions);
    Buffers(key, descriptor.buffers);
    return key.TakeKey();
}

CacheKey RenderKey(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor, id<MTLFunction> vertexFunction,
                   id<MTLFunction> fragmentFunction) {
    CacheKeyWriter key;
    Device(key, device, 'R');
    key.Bool(FastMathEnabled());
    Object(key, vertexFunction);
    Object(key, fragmentFunction);
    Colors(key, descriptor.colorAttachments);
    key.Value(descriptor.depthAttachmentPixelFormat);
    key.Value(descriptor.stencilAttachmentPixelFormat);
    key.Value(descriptor.rasterSampleCount);
    key.Bool(descriptor.alphaToCoverageEnabled);
    key.Bool(descriptor.alphaToOneEnabled);
    key.Bool(descriptor.rasterizationEnabled);
    key.Value(descriptor.inputPrimitiveTopology);
    key.Value(descriptor.tessellationPartitionMode);
    key.Value(descriptor.maxTessellationFactor);
    key.Bool(descriptor.tessellationFactorScaleEnabled);
    key.Value(descriptor.tessellationFactorFormat);
    key.Value(descriptor.tessellationControlPointIndexType);
    key.Value(descriptor.tessellationFactorStepFunction);
    key.Value(descriptor.tessellationOutputWindingOrder);
    key.Bool(descriptor.supportIndirectCommandBuffers);
    key.Value(descriptor.maxVertexAmplificationCount);
    key.Value(descriptor.maxVertexCallStackDepth);
    key.Value(descriptor.maxFragmentCallStackDepth);
    key.Bool(descriptor.supportAddingVertexBinaryFunctions);
    key.Bool(descriptor.supportAddingFragmentBinaryFunctions);
    VertexLayout(key, descriptor.vertexDescriptor);
    Buffers(key, descriptor.vertexBuffers);
    Buffers(key, descriptor.fragmentBuffers);
    return key.TakeKey();
}

CacheKey MeshKey(id<MTLDevice> device, MTLMeshRenderPipelineDescriptor* descriptor) {
    CacheKeyWriter key;
    Device(key, device, 'M');
    key.Bool(FastMathEnabled());
    Object(key, descriptor.objectFunction);
    Object(key, descriptor.meshFunction);
    Object(key, descriptor.fragmentFunction);
    Colors(key, descriptor.colorAttachments);
    key.Value(descriptor.depthAttachmentPixelFormat);
    key.Value(descriptor.stencilAttachmentPixelFormat);
    key.Value(descriptor.rasterSampleCount);
    key.Bool(descriptor.alphaToCoverageEnabled);
    key.Bool(descriptor.alphaToOneEnabled);
    key.Bool(descriptor.rasterizationEnabled);
    key.Value(descriptor.maxTotalThreadsPerObjectThreadgroup);
    key.Value(descriptor.maxTotalThreadsPerMeshThreadgroup);
    key.Bool(descriptor.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth);
    key.Bool(descriptor.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth);
    key.Value(descriptor.payloadMemoryLength);
    key.Value(descriptor.maxTotalThreadgroupsPerMeshGrid);
    key.Value(descriptor.maxVertexAmplificationCount);
    key.Bool(descriptor.supportIndirectCommandBuffers);
    Buffers(key, descriptor.objectBuffers);
    Buffers(key, descriptor.meshBuffers);
    Buffers(key, descriptor.fragmentBuffers);
    return key.TakeKey();
}

// ---- Binary archive -------------------------------------------------------------------------------

// A pipeline added to an archive, with what is needed to check that it reached the file and to add it
// again if it did not.
struct Recorded {
    std::function<bool(id<MTLBinaryArchive>, NSError**)> add;
    std::function<bool(id<MTLBinaryArchive>)> present;
};

struct Archive {
    id<MTLDevice> device = nil;
    id<MTLBinaryArchive> archive = nil;
    std::filesystem::path path;
    std::size_t pending = 0;
    std::vector<Recorded> unverified;  // added since the file was last verified
};

std::mutex archiveMutex;
std::map<std::pair<std::uintptr_t, bool>, Archive> archives;
// Metal does not document MTLBinaryArchive as safe to read (a pipeline created with it in
// binaryArchives) while another thread adds to or serializes it. Creations share this lock; adding
// and writing take it exclusively. Taken before archiveMutex, never while holding it.
std::shared_mutex archiveUse;
std::atomic<std::uint64_t> archiveLoads{0};
std::atomic<std::uint64_t> archiveAdditions{0};
std::atomic<std::uint64_t> archiveWrites{0};
std::atomic<std::uint64_t> archiveFailures{0};

struct QueuedRecord {
    id<MTLDevice> device;
    Recorded recorded;
};

std::mutex queueMutex;
std::condition_variable queueCv;
std::deque<QueuedRecord> queuedRecords;
std::uint64_t queuedSequence = 0;
std::uint64_t processedSequence = 0;
bool queueStopping = false;
bool queueShutdown = false;
std::thread queueThread;
std::condition_variable drainedCv;

std::atomic<std::uint64_t> pipelineCreations{0};
std::atomic<std::uint64_t> pipelineCreationMicros{0};
std::atomic<std::uint64_t> drawThreadCreations{0};
std::atomic<std::uint64_t> drawThreadCreationMicros{0};
std::atomic<std::uint64_t> libraryCompiles{0};
std::atomic<std::uint64_t> libraryCompileMicros{0};
std::atomic<std::uint64_t> drawThreadLibraryCompiles{0};
std::atomic<std::uint64_t> drawThreadLibraryCompileMicros{0};

Archive& ArchiveFor(id<MTLDevice> device);

std::uint64_t ElapsedMicros(const std::chrono::steady_clock::time_point& start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}

void AccountPipelineCreation(const std::chrono::steady_clock::time_point& start) {
    const auto micros = ElapsedMicros(start);
    pipelineCreations.fetch_add(1, std::memory_order_relaxed);
    pipelineCreationMicros.fetch_add(micros, std::memory_order_relaxed);
    if (!MetalWorkerPool::OnPoolThread()) {
        drawThreadCreations.fetch_add(1, std::memory_order_relaxed);
        drawThreadCreationMicros.fetch_add(micros, std::memory_order_relaxed);
    }
}

void AccountLibraryCompile(const std::chrono::steady_clock::time_point& start) {
    const auto micros = ElapsedMicros(start);
    libraryCompiles.fetch_add(1, std::memory_order_relaxed);
    libraryCompileMicros.fetch_add(micros, std::memory_order_relaxed);
    if (!MetalWorkerPool::OnPoolThread()) {
        drawThreadLibraryCompiles.fetch_add(1, std::memory_order_relaxed);
        drawThreadLibraryCompileMicros.fetch_add(micros, std::memory_order_relaxed);
    }
}

std::string Sanitized(NSString* text) {
    std::string result;
    for (const char character : std::string(text == nil ? "" : text.UTF8String)) {
        const bool plain = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                           (character >= '0' && character <= '9') || character == '.';
        if (plain) result += character;
        else if (!result.empty() && result.back() != '-') result += '-';
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    return result.empty() ? std::string("unknown") : result.substr(0, 96);
}

void ReportArchiveFailure(const char* operation, NSError* error) {
    archiveFailures.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) return;
    const char* message = error == nil ? "no Metal error description" : error.localizedDescription.UTF8String;
    std::fprintf(stderr, "[metal-pipeline-cache] %s failed (%s); continuing without it\n", operation,
                 message == nullptr ? "no Metal error description" : message);
}

enum class Serialized { Failed, Wrote, NothingNew };

// Requires archiveMutex. Writes the archive to a temporary file and moves it over the archive file.
Serialized SerializeArchive(Archive& state) {
    @autoreleasepool {
        auto temporary = state.path;
        static std::atomic<unsigned long> sequence{0};
        temporary += ".tmp." + std::to_string(static_cast<unsigned long>(getpid())) + "." + std::to_string(sequence.fetch_add(1));
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:temporary.c_str()]];
        NSError* error = nil;
        bool written = false;
        @try {
            written = [state.archive serializeToURL:url error:&error];
        } @catch (NSException*) {
            written = false;
        }
        std::error_code ignored;
        if (written && !std::filesystem::exists(temporary, ignored)) {
            // Metal writes no file when it considers the archive unchanged since it was loaded or last
            // written (two threads that built one pipeline at once both add it).
            return Serialized::NothingNew;
        }
        if (!written) {
            std::filesystem::remove(temporary, ignored);
            ReportArchiveFailure("writing the Metal pipeline archive", error);
            return Serialized::Failed;
        }
        std::error_code renamed;
        std::filesystem::rename(temporary, state.path, renamed);
        if (renamed) {
            std::filesystem::remove(temporary, ignored);
            ReportArchiveFailure("replacing the Metal pipeline archive", nil);
            return Serialized::Failed;
        }
        archiveWrites.fetch_add(1, std::memory_order_relaxed);
        return Serialized::Wrote;
    }
}

struct FileCheck {
    std::vector<Recorded> missing;
    id<MTLBinaryArchive> stored = nil;
};

// Reads the archive file back and returns the recorded pipelines it does not hold. Metal's
// serializeToURL: occasionally leaves out a pipeline that was added successfully (seen as one missing
// pipeline in roughly one write in fifty while other threads compile), and says nothing about it.
FileCheck CheckFile(const Archive& state) {
    @autoreleasepool {
        id<MTLBinaryArchive> stored = nil;
        std::error_code ignored;
        if (std::filesystem::is_regular_file(state.path, ignored)) {
            MTLBinaryArchiveDescriptor* descriptor = [MTLBinaryArchiveDescriptor new];
            descriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state.path.c_str()]];
            NSError* error = nil;
            stored = [state.device newBinaryArchiveWithDescriptor:descriptor error:&error];
        }
        FileCheck check;
        check.stored = stored;
        for (const auto& recorded : state.unverified) {
            if (stored == nil || !recorded.present(stored)) check.missing.push_back(recorded);
        }
        return check;
    }
}

// Requires archiveMutex. Replaces the archive with the file's contents plus `missing`, so that the next
// write has something new to write.
bool RebuildArchive(Archive& state, const std::vector<Recorded>& missing) {
    @autoreleasepool {
        MTLBinaryArchiveDescriptor* descriptor = [MTLBinaryArchiveDescriptor new];
        std::error_code ignored;
        if (std::filesystem::is_regular_file(state.path, ignored)) {
            descriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state.path.c_str()]];
        }
        NSError* error = nil;
        id<MTLBinaryArchive> fresh = [state.device newBinaryArchiveWithDescriptor:descriptor error:&error];
        if (fresh == nil && descriptor.url != nil) {
            descriptor.url = nil;
            error = nil;
            fresh = [state.device newBinaryArchiveWithDescriptor:descriptor error:&error];
        }
        if (fresh == nil) {
            ReportArchiveFailure("rebuilding the Metal pipeline archive", error);
            return false;
        }
        for (const auto& recorded : missing) {
            bool added = false;
            error = nil;
            @try {
                added = recorded.add(fresh, &error);
            } @catch (NSException*) {
                added = false;
            }
            if (!added) {
                ReportArchiveFailure("re-adding a pipeline to the Metal pipeline archive", error);
                return false;
            }
        }
        state.archive = fresh;
        state.unverified = missing;
        state.pending = missing.size();
        return true;
    }
}

// Requires archiveMutex. The archive file ends up holding every pipeline added to the archive.
bool WriteArchive(Archive& state) {
    if (state.archive == nil || state.pending == 0) return true;
    constexpr int Attempts = 4;
    for (int attempt = 0; attempt < Attempts; ++attempt) {
        const auto result = SerializeArchive(state);
        if (result == Serialized::Failed) return false;
        const auto check = CheckFile(state);
        const auto& missing = check.missing;
        if (missing.empty()) {
            // Metal keeps reading an archive from the file it was loaded from or last serialized to, and
            // that file has just been moved: carry on with the copy read back from the archive path.
            state.archive = check.stored;
            state.pending = 0;
            state.unverified.clear();
            return true;
        }
        if (!RebuildArchive(state, missing)) return false;
    }
    // Give up on these pipelines rather than retry them on every later write.
    state.pending = 0;
    state.unverified.clear();
    ReportArchiveFailure("completing the Metal pipeline archive", nil);
    return false;
}

bool FlushLocked() {
    bool ok = true;
    for (auto& [key, state] : archives) ok = WriteArchive(state) && ok;
    return ok;
}

void AddQueuedBatch(std::vector<QueuedRecord>& batch) {
    for (std::size_t offset = 0; offset < batch.size(); offset += ArchiveWriteBatch) {
        const auto end = std::min(batch.size(), offset + ArchiveWriteBatch);
        std::unique_lock use(archiveUse);
        std::lock_guard lock(archiveMutex);
        for (std::size_t i = offset; i < end; ++i) {
            auto& state = ArchiveFor(batch[i].device);
            if (state.archive == nil) continue;
            NSError* error = nil;
            bool added = false;
            @try {
                added = batch[i].recorded.add(state.archive, &error);
            } @catch (NSException*) {
                added = false;
            }
            if (!added) {
                ReportArchiveFailure("adding a pipeline to the Metal pipeline archive", error);
                continue;
            }
            state.unverified.push_back(std::move(batch[i].recorded));
            archiveAdditions.fetch_add(1, std::memory_order_relaxed);
            ++state.pending;
        }
    }
}

void WriteQueuedArchives(bool force) {
    std::unique_lock use(archiveUse);
    std::lock_guard lock(archiveMutex);
    for (auto& [key, state] : archives) {
        if (force || state.pending >= ArchiveWriteBatch) static_cast<void>(WriteArchive(state));
    }
}

void ArchiveWorkerMain() {
    pthread_setname_np("agc-pso-archive");
    for (;;) {
        std::vector<QueuedRecord> batch;
        std::uint64_t sequence = 0;
        bool stopping = false;
        {
            std::unique_lock lock(queueMutex);
            queueCv.wait(lock, [] { return queueStopping || !queuedRecords.empty(); });
            stopping = queueStopping;
            sequence = queuedSequence;
            batch.assign(std::make_move_iterator(queuedRecords.begin()), std::make_move_iterator(queuedRecords.end()));
            queuedRecords.clear();
        }
        if (!batch.empty()) {
            @autoreleasepool {
                try {
                    AddQueuedBatch(batch);
                } catch (...) {
                }
            }
        }
        {
            std::lock_guard lock(queueMutex);
            processedSequence = std::max(processedSequence, sequence);
        }
        drainedCv.notify_all();
        if (stopping) {
            try {
                WriteQueuedArchives(true);
            } catch (...) {
            }
            return;
        }
        bool idle = false;
        {
            std::lock_guard lock(queueMutex);
            idle = queuedRecords.empty() && !queueStopping;
        }
        if (idle) {
            try {
                WriteQueuedArchives(false);
            } catch (...) {
            }
        }
    }
}

void EnsureArchiveWorker() {
    std::lock_guard lock(queueMutex);
    if (queueShutdown) return;
    if (queueThread.joinable()) return;
    queueStopping = false;
    queueThread = std::thread(ArchiveWorkerMain);
}

void StopArchiveWorker() {
    std::thread thread;
    {
        std::lock_guard lock(queueMutex);
        if (!queueThread.joinable()) return;
        queueStopping = true;
        thread = std::move(queueThread);
    }
    queueCv.notify_all();
    if (thread.joinable()) thread.join();
}

void DrainArchiveQueue() {
    EnsureArchiveWorker();
    std::unique_lock lock(queueMutex);
    drainedCv.wait(lock, [] { return processedSequence >= queuedSequence; });
}

void EnqueueRecord(id<MTLDevice> device, Recorded recorded) {
    EnsureArchiveWorker();
    {
        std::lock_guard lock(queueMutex);
        if (queueStopping || queueShutdown) return;
        queuedRecords.push_back(QueuedRecord{device, std::move(recorded)});
        ++queuedSequence;
    }
    queueCv.notify_one();
}

void FlushAtExit() {
    WaitForMetalPipelineManifestReplay(120000);
    {
        std::lock_guard lock(queueMutex);
        queueShutdown = true;
    }
    StopArchiveWorker();
    std::unique_lock use(archiveUse);
    std::lock_guard lock(archiveMutex);
    static_cast<void>(FlushLocked());
}

// Requires archiveMutex. Opens (or creates) the archive for `device` the first time it is used.
Archive& ArchiveFor(id<MTLDevice> device) {
    const auto identity = reinterpret_cast<std::uintptr_t>((__bridge void*)device);
    const bool fastMath = FastMathEnabled();
    const auto archiveKey = std::make_pair(identity, fastMath);
    if (const auto found = archives.find(archiveKey); found != archives.end()) return found->second;
    auto& state = archives[archiveKey];
    state.device = device;
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    if (directory.empty()) return state;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        ReportArchiveFailure("creating the shader cache directory", nil);
        return state;
    }
    const std::string mathTag = fastMath ? "fastmath" : "safemath";
    state.path = directory / ("metal-pipelines-v1-" + mathTag + "-" + Sanitized(device.name) + "-" +
                              Sanitized(NSProcessInfo.processInfo.operatingSystemVersionString) + ".metalarchive");
    const auto legacyPath = directory / ("metal-pipelines-v1-" + Sanitized(device.name) + "-" +
                                         Sanitized(NSProcessInfo.processInfo.operatingSystemVersionString) + ".metalarchive");
    if (legacyPath != state.path) {
        std::error_code ignored;
        std::filesystem::remove(legacyPath, ignored);
    }
    @autoreleasepool {
        MTLBinaryArchiveDescriptor* descriptor = [MTLBinaryArchiveDescriptor new];
        NSError* failure = nil;
        if (std::filesystem::is_regular_file(state.path, error)) {
            descriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state.path.c_str()]];
            state.archive = [device newBinaryArchiveWithDescriptor:descriptor error:&failure];
            if (state.archive != nil) {
                archiveLoads.fetch_add(1, std::memory_order_relaxed);
            } else {
                // A stale or corrupt archive (another OS or driver build) is discarded and rebuilt.
                std::filesystem::remove(state.path, error);
                descriptor.url = nil;
            }
        }
        if (state.archive == nil) {
            failure = nil;
            state.archive = [device newBinaryArchiveWithDescriptor:descriptor error:&failure];
            if (state.archive == nil) ReportArchiveFailure("creating the Metal pipeline archive", failure);
        }
    }
    static std::once_flag registered;
    std::call_once(registered, [] { std::atexit(FlushAtExit); });
    return state;
}

id<MTLBinaryArchive> ArchiveHandle(id<MTLDevice> device) {
    std::lock_guard lock(archiveMutex);
    return ArchiveFor(device).archive;
}

// `archive` is the handle the pipeline was created against (nil when the disk cache is off).
void Record(id<MTLDevice> device, id<MTLBinaryArchive> archive, Recorded recorded) {
    if (archive == nil) return;
    EnqueueRecord(device, std::move(recorded));
}

}

bool RenderPipelineCacheable(MTLRenderPipelineDescriptor* descriptor) {
    return !Linked(descriptor.vertexLinkedFunctions.functions) && !Linked(descriptor.fragmentLinkedFunctions.functions) &&
           !Linked(descriptor.vertexPreloadedLibraries) && !Linked(descriptor.fragmentPreloadedLibraries);
}

bool ComputePipelineCacheable(MTLComputePipelineDescriptor* descriptor) {
    return !Linked(descriptor.linkedFunctions.functions) && !Linked(descriptor.preloadedLibraries);
}

MTLLanguageVersion ConvertedShaderLanguageVersion(MTLLanguageVersion processDefault, bool languageVersion32) {
    return languageVersion32 || processDefault < MTLLanguageVersion3_2 ? MTLLanguageVersion3_2 : processDefault;
}

id<MTLFunction> CachedShaderFunction(id<MTLDevice> device, const std::string& source, const std::string& entryPoint,
                                     bool languageVersion32, std::size_t sourceHash) {
    // The key is this short head followed by the whole source. A draw looks it up without copying
    // the source into a key, and with the source's hash from its conversion instead of hashing it again.
    const bool fastMath = FastMathEnabled();
    CacheKeyWriter writer;
    Device(writer, device, 'F');
    writer.Bool(languageVersion32);
    writer.Bool(fastMath);
    writer.Text(entryPoint);
    writer.Value<std::uint64_t>(source.size());
    const auto head = writer.Take();
    if (sourceHash == 0) sourceHash = ShaderRecompiler::MetalBackend::ShaderSourceHash(source);
    const std::size_t hash = std::hash<std::string_view>{}(head) * 0x9e3779b97f4a7c15ull ^ sourceHash;
    if (auto cached = Functions().Find(CacheKeyProbe{head, source, hash})) return cached->function;
    auto key = CacheKey(head + source, hash);
    // A source Metal rejects is rejected again: remember the error instead of compiling it for every
    // draw that uses it.
    static std::mutex failedMutex;
    static auto* failed = new std::unordered_map<std::string, std::string>();
    {
        std::lock_guard lock(failedMutex);
        if (const auto found = failed->find(key.Bytes()); found != failed->end()) throw std::runtime_error(found->second);
    }
    NSString* text = [[NSString alloc] initWithBytes:source.data() length:source.size() encoding:NSUTF8StringEncoding];
    NSString* name = [[NSString alloc] initWithBytes:entryPoint.data() length:entryPoint.size() encoding:NSUTF8StringEncoding];
    if (text == nil || name == nil) throw std::invalid_argument("Metal converted shader source and entry point must be valid UTF-8");
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = fastMath;
#pragma clang diagnostic pop
    if (@available(macOS 15.0, *)) {
        options.mathMode = fastMath ? MTLMathModeFast : MTLMathModeSafe;
        options.languageVersion = ConvertedShaderLanguageVersion(options.languageVersion, languageVersion32);
    }
    NSError* error = nil;
    const auto compileStart = std::chrono::steady_clock::now();
    id<MTLLibrary> library = [device newLibraryWithSource:text options:options error:&error];
    AccountLibraryCompile(compileStart);
    if (library == nil) {
        const char* message = error.localizedDescription.UTF8String;
        auto what = std::string("Converted Metal shader compilation failed: ") + (message == nullptr ? "no Metal error description" : message);
        std::lock_guard lock(failedMutex);
        if (failed->size() >= 1024) failed->clear();
        failed->emplace(key.Bytes(), what);
        throw std::runtime_error(what);
    }
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (function == nil) return nil;
    return Functions().Insert(std::move(key), FunctionEntry{function}, ObjectBytes).function;
}

id<MTLFunction> CachedConvertedShaderFunction(id<MTLDevice> device, const ShaderRecompiler::MetalBackend::ShaderSource& source,
                                              const std::string& entryPoint, bool languageVersion32, std::size_t sourceHash) {
    const auto& text = source.Shared();
    if (!text) return CachedShaderFunction(device, source.str(), entryPoint, languageVersion32, sourceHash);
    CacheKeyWriter writer;
    Device(writer, device, 'S');
    writer.Bool(languageVersion32);
    writer.Bool(FastMathEnabled());
    writer.Text(entryPoint);
    writer.Value(reinterpret_cast<std::uintptr_t>(text.get()));
    auto key = writer.TakeKey();
    if (auto cached = SharedFunctions().Find(key)) return cached->function;
    id<MTLFunction> function = CachedShaderFunction(device, *text, entryPoint, languageVersion32, sourceHash);
    if (function != nil) static_cast<void>(SharedFunctions().Insert(std::move(key), SharedFunctionEntry{text, function}, text->size() + ObjectBytes));
    return function;
}

CachedComputePipeline CachedComputePipelineState(id<MTLDevice> device, MTLComputePipelineDescriptor* descriptor, NSError** error) {
    const bool cacheable = !Linked(descriptor.linkedFunctions.functions) && !Linked(descriptor.preloadedLibraries);
    auto key = cacheable ? ComputeKey(device, descriptor) : CacheKey();
    if (cacheable) {
        if (auto cached = ComputePipelines().Find(key)) return {cached->state, cached->reflection};
    }
    MTLComputePipelineDescriptor* native = [descriptor copy];
    const auto archive = cacheable ? ArchiveHandle(device) : nil;
    if (archive != nil) native.binaryArchives = @[archive];
    MTLComputePipelineReflection* reflection = nil;
    id<MTLComputePipelineState> state = nil;
    {
        std::shared_lock use(archiveUse, std::defer_lock);
        if (archive != nil) use.lock();
        const auto creationStart = std::chrono::steady_clock::now();
        state = [device newComputePipelineStateWithDescriptor:native options:MTLPipelineOptionBindingInfo
                                                       reflection:&reflection error:error];
        AccountPipelineCreation(creationStart);
    }
    if (state == nil || !cacheable) return {state, reflection};
    {
        // Recorded without the archive it was created against: adding must not depend on any archive.
        MTLComputePipelineDescriptor* plain = [native copy];
        plain.binaryArchives = nil;
        Record(device, archive, Recorded{
            [plain](id<MTLBinaryArchive> target, NSError** failure) { return [target addComputePipelineFunctionsWithDescriptor:plain error:failure]; },
            [device, plain](id<MTLBinaryArchive> stored) {
                MTLComputePipelineDescriptor* probe = [plain copy];
                probe.binaryArchives = @[stored];
                NSError* ignored = nil;
                return [device newComputePipelineStateWithDescriptor:probe options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&ignored] != nil;
            }});
    }
    const auto entry = ComputePipelines().Insert(std::move(key), ComputeEntry{state, reflection, native}, ObjectBytes);
    return {entry.state, entry.reflection};
}

namespace {

bool RenderCacheable(MTLRenderPipelineDescriptor* descriptor) {
    return !Linked(descriptor.vertexLinkedFunctions.functions) && !Linked(descriptor.fragmentLinkedFunctions.functions) &&
           !Linked(descriptor.vertexPreloadedLibraries) && !Linked(descriptor.fragmentPreloadedLibraries);
}

// Creates (and, when cacheable, caches under `key`) the pipeline for `native`, a copy the cache may keep.
CachedRenderPipeline CreateRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* native, bool cacheable, CacheKey key,
                                               NSError** error);

}

CachedRenderPipeline CachedRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor, NSError** error) {
    if (Force1xMsaaEnabled() && descriptor.rasterSampleCount > 1) {
        descriptor.rasterSampleCount = 1;
    }
    const bool cacheable = RenderCacheable(descriptor);
    auto key = cacheable ? RenderKey(device, descriptor, descriptor.vertexFunction, descriptor.fragmentFunction) : CacheKey();
    if (cacheable) {
        if (auto cached = RenderPipelines().Find(key)) return {cached->state, cached->reflection};
    }
    return CreateRenderPipelineState(device, [descriptor copy], cacheable, std::move(key), error);
}

CachedRenderPipeline CachedRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor, id<MTLFunction> vertexFunction,
                                               id<MTLFunction> fragmentFunction, NSError** error) {
    if (Force1xMsaaEnabled() && descriptor.rasterSampleCount > 1) {
        descriptor.rasterSampleCount = 1;
    }
    const bool cacheable = RenderCacheable(descriptor);
    auto key = cacheable ? RenderKey(device, descriptor, vertexFunction, fragmentFunction) : CacheKey();
    if (cacheable) {
        if (auto cached = RenderPipelines().Find(key)) return {cached->state, cached->reflection};
    }
    MTLRenderPipelineDescriptor* native = [descriptor copy];
    native.vertexFunction = vertexFunction;
    native.fragmentFunction = fragmentFunction;
    return CreateRenderPipelineState(device, native, cacheable, std::move(key), error);
}

namespace {

CachedRenderPipeline CreateRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* native, bool cacheable, CacheKey key,
                                               NSError** error) {
    const auto archive = cacheable ? ArchiveHandle(device) : nil;
    if (archive != nil) native.binaryArchives = @[archive];
    MTLRenderPipelineReflection* reflection = nil;
    id<MTLRenderPipelineState> state = nil;
    {
        std::shared_lock use(archiveUse, std::defer_lock);
        if (archive != nil) use.lock();
        const auto creationStart = std::chrono::steady_clock::now();
        state = [device newRenderPipelineStateWithDescriptor:native options:MTLPipelineOptionBindingInfo
                                                     reflection:&reflection error:error];
        AccountPipelineCreation(creationStart);
    }
    if (state == nil || !cacheable) return {state, reflection};
    {
        // Recorded without the archive it was created against: adding must not depend on any archive.
        MTLRenderPipelineDescriptor* plain = [native copy];
        plain.binaryArchives = nil;
        Record(device, archive, Recorded{
            [plain](id<MTLBinaryArchive> target, NSError** failure) { return [target addRenderPipelineFunctionsWithDescriptor:plain error:failure]; },
            [device, plain](id<MTLBinaryArchive> stored) {
                MTLRenderPipelineDescriptor* probe = [plain copy];
                probe.binaryArchives = @[stored];
                NSError* ignored = nil;
                return [device newRenderPipelineStateWithDescriptor:probe options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&ignored] != nil;
            }});
    }
    const auto entry = RenderPipelines().Insert(std::move(key), RenderEntry{state, reflection, native}, ObjectBytes);
    return {entry.state, entry.reflection};
}

}

CachedRenderPipeline CachedMeshPipelineState(id<MTLDevice> device, MTLMeshRenderPipelineDescriptor* descriptor, NSError** error) {
    if (Force1xMsaaEnabled() && descriptor.rasterSampleCount > 1) {
        descriptor.rasterSampleCount = 1;
    }
    const bool cacheable = !Linked(descriptor.objectLinkedFunctions.functions) && !Linked(descriptor.meshLinkedFunctions.functions) &&
                           !Linked(descriptor.fragmentLinkedFunctions.functions);
    auto key = cacheable ? MeshKey(device, descriptor) : CacheKey();
    if (cacheable) {
        if (auto cached = RenderPipelines().Find(key)) return {cached->state, cached->reflection};
    }
    MTLMeshRenderPipelineDescriptor* native = [descriptor copy];
    const auto archive = cacheable ? ArchiveHandle(device) : nil;
    if (archive != nil) native.binaryArchives = @[archive];
    MTLRenderPipelineReflection* reflection = nil;
    id<MTLRenderPipelineState> state = nil;
    {
        std::shared_lock use(archiveUse, std::defer_lock);
        if (archive != nil) use.lock();
        const auto creationStart = std::chrono::steady_clock::now();
        state = [device newRenderPipelineStateWithMeshDescriptor:native options:MTLPipelineOptionBindingInfo
                                                         reflection:&reflection error:error];
        AccountPipelineCreation(creationStart);
    }
    if (state == nil || !cacheable) return {state, reflection};
    {
        // Recorded without the archive it was created against: adding must not depend on any archive.
        MTLMeshRenderPipelineDescriptor* plain = [native copy];
        plain.binaryArchives = nil;
        Record(device, archive, Recorded{
            [plain](id<MTLBinaryArchive> target, NSError** failure) { return [target addMeshRenderPipelineFunctionsWithDescriptor:plain error:failure]; },
            [device, plain](id<MTLBinaryArchive> stored) {
                MTLMeshRenderPipelineDescriptor* probe = [plain copy];
                probe.binaryArchives = @[stored];
                NSError* ignored = nil;
                return [device newRenderPipelineStateWithMeshDescriptor:probe options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&ignored] != nil;
            }});
    }
    const auto entry = RenderPipelines().Insert(std::move(key), RenderEntry{state, reflection, native}, ObjectBytes);
    return {entry.state, entry.reflection};
}

id<MTLDepthStencilState> CachedDepthStencilState(id<MTLDevice> device, MTLDepthStencilDescriptor* descriptor) {
    CacheKeyWriter writer;
    Device(writer, device, 'D');
    writer.Value(descriptor.depthCompareFunction);
    writer.Bool(descriptor.depthWriteEnabled);
    Stencil(writer, descriptor.frontFaceStencil);
    Stencil(writer, descriptor.backFaceStencil);
    auto key = writer.TakeKey();
    if (auto cached = DepthStencilStates().Find(key)) return *cached;
    id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:descriptor];
    if (state == nil) return nil;
    return DepthStencilStates().Insert(std::move(key), state, ObjectBytes);
}

id<MTLSamplerState> CachedSamplerState(id<MTLDevice> device, MTLSamplerDescriptor* descriptor) {
    CacheKeyWriter writer;
    Device(writer, device, 'S');
    writer.Value(descriptor.minFilter);
    writer.Value(descriptor.magFilter);
    writer.Value(descriptor.mipFilter);
    writer.Value(descriptor.maxAnisotropy);
    writer.Value(descriptor.sAddressMode);
    writer.Value(descriptor.tAddressMode);
    writer.Value(descriptor.rAddressMode);
    writer.Value(descriptor.borderColor);
    writer.Bool(descriptor.normalizedCoordinates);
    writer.Value(descriptor.lodMinClamp);
    writer.Value(descriptor.lodMaxClamp);
    writer.Bool(descriptor.lodAverage);
    writer.Value(descriptor.compareFunction);
    writer.Bool(descriptor.supportArgumentBuffers);
    OptionalNumber(writer, descriptor, @"lodBias");
    OptionalNumber(writer, descriptor, @"reductionMode");
    auto key = writer.TakeKey();
    if (auto cached = SamplerStates().Find(key)) return *cached;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
    if (sampler == nil) return nil;
    return SamplerStates().Insert(std::move(key), sampler, ObjectBytes);
}

MetalPipelineCacheStatistics GetMetalPipelineCacheStatistics() {
    const auto counter = [](const auto& cache) {
        const auto counters = cache.Counters();
        return MetalCacheCounter{counters.hits, counters.misses};
    };
    MetalPipelineCacheStatistics result;
    // A hit by shared source is a hit of the library cache it stands in front of.
    result.libraries = counter(Functions());
    result.libraries.hits += SharedFunctions().Counters().hits;
    result.computePipelines = counter(ComputePipelines());
    result.renderPipelines = counter(RenderPipelines());
    result.depthStencilStates = counter(DepthStencilStates());
    result.samplerStates = counter(SamplerStates());
    result.archiveLoads = archiveLoads.load(std::memory_order_relaxed);
    result.archiveAdditions = archiveAdditions.load(std::memory_order_relaxed);
    result.archiveWrites = archiveWrites.load(std::memory_order_relaxed);
    result.archiveFailures = archiveFailures.load(std::memory_order_relaxed);
    result.pipelineCreations = pipelineCreations.load(std::memory_order_relaxed);
    result.pipelineCreationMicros = pipelineCreationMicros.load(std::memory_order_relaxed);
    result.drawThreadCreations = drawThreadCreations.load(std::memory_order_relaxed);
    result.drawThreadCreationMicros = drawThreadCreationMicros.load(std::memory_order_relaxed);
    result.libraryCompiles = libraryCompiles.load(std::memory_order_relaxed);
    result.libraryCompileMicros = libraryCompileMicros.load(std::memory_order_relaxed);
    result.drawThreadLibraryCompiles = drawThreadLibraryCompiles.load(std::memory_order_relaxed);
    result.drawThreadLibraryCompileMicros = drawThreadLibraryCompileMicros.load(std::memory_order_relaxed);
    return result;
}

std::filesystem::path MetalPipelineArchivePath(id<MTLDevice> device) {
    std::lock_guard lock(archiveMutex);
    const auto& state = ArchiveFor(device);
    return state.archive == nil ? std::filesystem::path() : state.path;
}

bool FlushMetalPipelineArchives() {
    DrainArchiveQueue();
    std::unique_lock use(archiveUse);
    std::lock_guard lock(archiveMutex);
    return FlushLocked();
}

void ClearMetalPipelineCaches() {
    WaitForMetalPipelineManifestReplay(120000);
    DrainArchiveQueue();
    {
        std::unique_lock use(archiveUse);
        std::lock_guard lock(archiveMutex);
        static_cast<void>(FlushLocked());
        archives.clear();
    }
    SharedFunctions().Clear();
    Functions().Clear();
    ComputePipelines().Clear();
    RenderPipelines().Clear();
    DepthStencilStates().Clear();
    SamplerStates().Clear();
    ClearMetalPipelineManifest();
    archiveLoads = 0;
    archiveAdditions = 0;
    archiveWrites = 0;
    archiveFailures = 0;
    pipelineCreations = 0;
    pipelineCreationMicros = 0;
    drawThreadCreations = 0;
    drawThreadCreationMicros = 0;
    libraryCompiles = 0;
    libraryCompileMicros = 0;
    drawThreadLibraryCompiles = 0;
    drawThreadLibraryCompileMicros = 0;
}

}
