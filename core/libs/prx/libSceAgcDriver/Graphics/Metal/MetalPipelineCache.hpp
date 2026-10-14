#pragma once

#import <Metal/Metal.h>
#include <cstdint>
#include <filesystem>
#include <string>

// Process-wide caches for immutable native Metal objects. Every key is built from the complete native
// descriptor (and, for libraries, the complete MSL source), never from guest addresses or guest memory
// contents, so a hit always returns an object equivalent to a freshly created one. Pipelines are also
// recorded in an MTLBinaryArchive under the shader cache directory; archive failures are never fatal.
namespace AgcDriver::Metal {

struct MetalCacheCounter {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

struct MetalPipelineCacheStatistics {
    MetalCacheCounter libraries;
    MetalCacheCounter computePipelines;
    MetalCacheCounter renderPipelines;
    MetalCacheCounter depthStencilStates;
    MetalCacheCounter samplerStates;
    std::uint64_t archiveLoads = 0;
    std::uint64_t archiveAdditions = 0;
    std::uint64_t archiveWrites = 0;
    std::uint64_t archiveFailures = 0;
};

struct CachedComputePipeline {
    id<MTLComputePipelineState> state = nil;
    MTLComputePipelineReflection* reflection = nil;
};

struct CachedRenderPipeline {
    id<MTLRenderPipelineState> state = nil;
    MTLRenderPipelineReflection* reflection = nil;
};

// The language version converted shaders compile with, given the process default. The shader bridge
// targets MSL 3.2 (it emits atomic_thread_fence with memory_order_seq_cst and thread scopes), and the
// default follows the main executable's SDK: a relinked title (Rosetta direct path) declares SDK 11,
// whose default MSL 2.3 lacks those identifiers. Exactly MSL 3.2 when `languageVersion32`.
[[nodiscard]] MTLLanguageVersion ConvertedShaderLanguageVersion(MTLLanguageVersion processDefault, bool languageVersion32) API_AVAILABLE(macos(15.0));
// Returns `entryPoint` from the library compiled from `source` with the converted-shader compile options
// (safe math; ConvertedShaderLanguageVersion). Throws std::runtime_error when compilation fails and
// returns nil when the entry point is missing. `sourceHash` is ShaderSourceHash(source) when the caller
// has it (a converted shader's Result::sourceHash), or 0 to compute it.
[[nodiscard]] id<MTLFunction> CachedShaderFunction(id<MTLDevice> device, const std::string& source,
                                                   const std::string& entryPoint, bool languageVersion32,
                                                   std::size_t sourceHash = 0);
// Pipeline states with MTLPipelineOptionBindingInfo reflection. They return a nil state and set `error`
// when creation fails.
[[nodiscard]] CachedComputePipeline CachedComputePipelineState(id<MTLDevice> device, MTLComputePipelineDescriptor* descriptor,
                                                               NSError** error);
[[nodiscard]] CachedRenderPipeline CachedRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor,
                                                             NSError** error);
// The pipeline for `descriptor` with these functions in place of its own. The descriptor is only copied
// when the pipeline has to be created, so a draw whose pipeline is cached does not copy it.
[[nodiscard]] CachedRenderPipeline CachedRenderPipelineState(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor,
                                                             id<MTLFunction> vertexFunction, id<MTLFunction> fragmentFunction,
                                                             NSError** error);
[[nodiscard]] CachedRenderPipeline CachedMeshPipelineState(id<MTLDevice> device, MTLMeshRenderPipelineDescriptor* descriptor,
                                                           NSError** error);
[[nodiscard]] id<MTLDepthStencilState> CachedDepthStencilState(id<MTLDevice> device, MTLDepthStencilDescriptor* descriptor);
// Metal returns one shared object for identical sampler descriptors anyway; the cache only saves the
// creation call. Callers must not treat a sampler object as unique to one guest descriptor.
[[nodiscard]] id<MTLSamplerState> CachedSamplerState(id<MTLDevice> device, MTLSamplerDescriptor* descriptor);

[[nodiscard]] MetalPipelineCacheStatistics GetMetalPipelineCacheStatistics();
// The binary archive file for `device`, or empty when the disk cache is disabled.
[[nodiscard]] std::filesystem::path MetalPipelineArchivePath(id<MTLDevice> device);
// Writes every archive with unsaved pipelines; false when any write failed.
bool FlushMetalPipelineArchives();
// Flushes the archives, then drops every cached object, archive and counter (the cache directory is
// re-read on next use).
void ClearMetalPipelineCaches();

}
