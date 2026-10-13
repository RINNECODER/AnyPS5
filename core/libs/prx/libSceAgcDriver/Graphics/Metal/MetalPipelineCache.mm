#include "MetalPipelineCache.hpp"
#include "MetalBackend/BoundedCache.hpp"
#include "ShaderCacheDirectory.hpp"
#include <unordered_map>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <system_error>

namespace AgcDriver::Metal {

namespace {

using ShaderRecompiler::MetalBackend::BoundedCache;
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

std::string ComputeKey(id<MTLDevice> device, MTLComputePipelineDescriptor* descriptor) {
    CacheKeyWriter key;
    Device(key, device, 'C');
    Object(key, descriptor.computeFunction);
    StageInputs(key, descriptor.stageInputDescriptor);
    key.Bool(descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth);
    key.Value(descriptor.maxTotalThreadsPerThreadgroup);
    key.Bool(descriptor.supportIndirectCommandBuffers);
    key.Value(descriptor.maxCallStackDepth);
    key.Bool(descriptor.supportAddingBinaryFunctions);
    Buffers(key, descriptor.buffers);
    return key.Take();
}

std::string RenderKey(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor) {
    CacheKeyWriter key;
    Device(key, device, 'R');
    Object(key, descriptor.vertexFunction);
    Object(key, descriptor.fragmentFunction);
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
    return key.Take();
}

std::string MeshKey(id<MTLDevice> device, MTLMeshRenderPipelineDescriptor* descriptor) {
    CacheKeyWriter key;
    Device(key, device, 'M');
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
    return key.Take();
}

// ---- Binary archive -------------------------------------------------------------------------------

struct Archive {
    id<MTLBinaryArchive> archive = nil;
    std::filesystem::path path;
    std::size_t pending = 0;
};

std::mutex archiveMutex;
std::map<std::uintptr_t, Archive> archives;
// Metal does not document MTLBinaryArchive as safe to read (a pipeline created with it in
// binaryArchives) while another thread adds to or serializes it. Creations share this lock; adding
// and writing take it exclusively. Taken before archiveMutex, never while holding it.
std::shared_mutex archiveUse;
std::atomic<std::uint64_t> archiveLoads{0};
std::atomic<std::uint64_t> archiveAdditions{0};
std::atomic<std::uint64_t> archiveWrites{0};
std::atomic<std::uint64_t> archiveFailures{0};

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

// Requires archiveMutex.
bool WriteArchive(Archive& state) {
    if (state.archive == nil || state.pending == 0) return true;
    @autoreleasepool {
        auto temporary = state.path;
        temporary += ".tmp." + std::to_string(static_cast<unsigned long>(getpid()));
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
            // Metal writes no file when the archive holds nothing new since it was loaded or last
            // written (two threads that built one pipeline at once both add it): the file is current.
            state.pending = 0;
            return true;
        }
        if (!written) {
            std::filesystem::remove(temporary, ignored);
            ReportArchiveFailure("writing the Metal pipeline archive", error);
            return false;
        }
        std::error_code renamed;
        std::filesystem::rename(temporary, state.path, renamed);
        if (renamed) {
            std::filesystem::remove(temporary, ignored);
            ReportArchiveFailure("replacing the Metal pipeline archive", nil);
            return false;
        }
        state.pending = 0;
        archiveWrites.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
}

bool FlushLocked() {
    bool ok = true;
    for (auto& [device, state] : archives) ok = WriteArchive(state) && ok;
    return ok;
}

void FlushAtExit() {
    std::unique_lock use(archiveUse);
    std::lock_guard lock(archiveMutex);
    static_cast<void>(FlushLocked());
}

// Requires archiveMutex. Opens (or creates) the archive for `device` the first time it is used.
Archive& ArchiveFor(id<MTLDevice> device) {
    const auto identity = reinterpret_cast<std::uintptr_t>((__bridge void*)device);
    if (const auto found = archives.find(identity); found != archives.end()) return found->second;
    auto& state = archives[identity];
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    if (directory.empty()) return state;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        ReportArchiveFailure("creating the shader cache directory", nil);
        return state;
    }
    state.path = directory / ("metal-pipelines-v1-" + Sanitized(device.name) + "-" +
                              Sanitized(NSProcessInfo.processInfo.operatingSystemVersionString) + ".metalarchive");
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

template<class Add>
void Record(id<MTLDevice> device, id<MTLBinaryArchive> archive, Add&& add) {
    if (archive == nil) return;
    std::unique_lock use(archiveUse);
    std::lock_guard lock(archiveMutex);
    auto& state = ArchiveFor(device);
    if (state.archive != archive) return;
    NSError* error = nil;
    bool added = false;
    @try {
        added = add(&error);
    } @catch (NSException*) {
        added = false;
    }
    if (!added) {
        ReportArchiveFailure("adding a pipeline to the Metal pipeline archive", error);
        return;
    }
    archiveAdditions.fetch_add(1, std::memory_order_relaxed);
    if (++state.pending >= ArchiveWriteBatch) static_cast<void>(WriteArchive(state));
}

}

MTLLanguageVersion ConvertedShaderLanguageVersion(MTLLanguageVersion processDefault, bool languageVersion32) {
    return languageVersion32 || processDefault < MTLLanguageVersion3_2 ? MTLLanguageVersion3_2 : processDefault;
}

id<MTLFunction> CachedShaderFunction(id<MTLDevice> device, const std::string& source, const std::string& entryPoint,
                                     bool languageVersion32) {
    CacheKeyWriter writer;
    Device(writer, device, 'F');
    writer.Bool(languageVersion32);
    writer.Text(entryPoint);
    writer.Text(source);
    auto key = writer.Take();
    if (auto cached = Functions().Find(key)) return cached->function;
    // A source Metal rejects is rejected again: remember the error instead of compiling it for every
    // draw that uses it.
    static std::mutex failedMutex;
    static auto* failed = new std::unordered_map<std::string, std::string>();
    {
        std::lock_guard lock(failedMutex);
        if (const auto found = failed->find(key); found != failed->end()) throw std::runtime_error(found->second);
    }
    NSString* text = [[NSString alloc] initWithBytes:source.data() length:source.size() encoding:NSUTF8StringEncoding];
    NSString* name = [[NSString alloc] initWithBytes:entryPoint.data() length:entryPoint.size() encoding:NSUTF8StringEncoding];
    if (text == nil || name == nil) throw std::invalid_argument("Metal converted shader source and entry point must be valid UTF-8");
    MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
    if (@available(macOS 15.0, *)) {
        options.mathMode = MTLMathModeSafe;
        options.languageVersion = ConvertedShaderLanguageVersion(options.languageVersion, languageVersion32);
    }
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:text options:options error:&error];
    if (library == nil) {
        const char* message = error.localizedDescription.UTF8String;
        auto what = std::string("Converted Metal shader compilation failed: ") + (message == nullptr ? "no Metal error description" : message);
        std::lock_guard lock(failedMutex);
        if (failed->size() >= 1024) failed->clear();
        failed->emplace(key, what);
        throw std::runtime_error(what);
    }
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (function == nil) return nil;
    return Functions().Insert(std::move(key), FunctionEntry{function}, ObjectBytes).function;
}

CachedComputePipeline CachedComputePipelineState(id<MTLDevice> device, MTLComputePipelineDescriptor* descriptor, NSError** error) {
    const bool cacheable = !Linked(descriptor.linkedFunctions.functions) && !Linked(descriptor.preloadedLibraries);
    auto key = cacheable ? ComputeKey(device, descriptor) : std::string();
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
        state = [device newComputePipelineStateWithDescriptor:native options:MTLPipelineOptionBindingInfo
                                                       reflection:&reflection error:error];
    }
    if (state == nil || !cacheable) return {state, reflection};
    Record(device, archive, [&](NSError** failure) { return [archive addComputePipelineFunctionsWithDescriptor:native error:failure]; });
    const auto entry = ComputePipelines().Insert(std::move(key), ComputeEntry{state, reflection, native}, ObjectBytes);
    return {entry.state, entry.reflection};
}

CachedRenderPipeline CachedRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor, NSError** error) {
    const bool cacheable = !Linked(descriptor.vertexLinkedFunctions.functions) && !Linked(descriptor.fragmentLinkedFunctions.functions) &&
                           !Linked(descriptor.vertexPreloadedLibraries) && !Linked(descriptor.fragmentPreloadedLibraries);
    auto key = cacheable ? RenderKey(device, descriptor) : std::string();
    if (cacheable) {
        if (auto cached = RenderPipelines().Find(key)) return {cached->state, cached->reflection};
    }
    MTLRenderPipelineDescriptor* native = [descriptor copy];
    const auto archive = cacheable ? ArchiveHandle(device) : nil;
    if (archive != nil) native.binaryArchives = @[archive];
    MTLRenderPipelineReflection* reflection = nil;
    id<MTLRenderPipelineState> state = nil;
    {
        std::shared_lock use(archiveUse, std::defer_lock);
        if (archive != nil) use.lock();
        state = [device newRenderPipelineStateWithDescriptor:native options:MTLPipelineOptionBindingInfo
                                                     reflection:&reflection error:error];
    }
    if (state == nil || !cacheable) return {state, reflection};
    Record(device, archive, [&](NSError** failure) { return [archive addRenderPipelineFunctionsWithDescriptor:native error:failure]; });
    const auto entry = RenderPipelines().Insert(std::move(key), RenderEntry{state, reflection, native}, ObjectBytes);
    return {entry.state, entry.reflection};
}

CachedRenderPipeline CachedMeshPipelineState(id<MTLDevice> device, MTLMeshRenderPipelineDescriptor* descriptor, NSError** error) {
    const bool cacheable = !Linked(descriptor.objectLinkedFunctions.functions) && !Linked(descriptor.meshLinkedFunctions.functions) &&
                           !Linked(descriptor.fragmentLinkedFunctions.functions);
    auto key = cacheable ? MeshKey(device, descriptor) : std::string();
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
        state = [device newRenderPipelineStateWithMeshDescriptor:native options:MTLPipelineOptionBindingInfo
                                                         reflection:&reflection error:error];
    }
    if (state == nil || !cacheable) return {state, reflection};
    Record(device, archive, [&](NSError** failure) { return [archive addMeshRenderPipelineFunctionsWithDescriptor:native error:failure]; });
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
    auto key = writer.Take();
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
    auto key = writer.Take();
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
    result.libraries = counter(Functions());
    result.computePipelines = counter(ComputePipelines());
    result.renderPipelines = counter(RenderPipelines());
    result.depthStencilStates = counter(DepthStencilStates());
    result.samplerStates = counter(SamplerStates());
    result.archiveLoads = archiveLoads.load(std::memory_order_relaxed);
    result.archiveAdditions = archiveAdditions.load(std::memory_order_relaxed);
    result.archiveWrites = archiveWrites.load(std::memory_order_relaxed);
    result.archiveFailures = archiveFailures.load(std::memory_order_relaxed);
    return result;
}

std::filesystem::path MetalPipelineArchivePath(id<MTLDevice> device) {
    std::lock_guard lock(archiveMutex);
    const auto& state = ArchiveFor(device);
    return state.archive == nil ? std::filesystem::path() : state.path;
}

bool FlushMetalPipelineArchives() {
    std::unique_lock use(archiveUse);
    std::lock_guard lock(archiveMutex);
    return FlushLocked();
}

void ClearMetalPipelineCaches() {
    {
        std::unique_lock use(archiveUse);
        std::lock_guard lock(archiveMutex);
        static_cast<void>(FlushLocked());
        archives.clear();
    }
    Functions().Clear();
    ComputePipelines().Clear();
    RenderPipelines().Clear();
    DepthStencilStates().Clear();
    SamplerStates().Clear();
    archiveLoads = 0;
    archiveAdditions = 0;
    archiveWrites = 0;
    archiveFailures = 0;
}

}
