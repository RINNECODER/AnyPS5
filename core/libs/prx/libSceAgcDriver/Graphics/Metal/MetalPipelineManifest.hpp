#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALPIPELINEMANIFEST_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_METAL_METALPIPELINEMANIFEST_HPP

#import <Metal/Metal.h>
#include "MetalBackend/MetalShaderBridge.hpp"
#include <cstdint>

namespace AgcDriver::Metal {

struct MetalPipelineManifestStatistics {
    std::uint64_t entries = 0;
    std::uint64_t loaded = 0;
    std::uint64_t replayed = 0;
    std::uint64_t failed = 0;
    std::uint64_t micros = 0;
};

void RecordRenderPipelineRecipe(id<MTLDevice> device, const ShaderRecompiler::MetalBackend::Result& vertex,
                                const ShaderRecompiler::MetalBackend::Result& fragment,
                                MTLRenderPipelineDescriptor* descriptor);
void RecordComputePipelineRecipe(id<MTLDevice> device, const ShaderRecompiler::MetalBackend::Result& shader,
                                 MTLComputePipelineDescriptor* descriptor);

void FlushMetalPipelineManifest();
void ClearMetalPipelineManifest();

bool ReplayMetalPipelineManifest(id<MTLDevice> device);
void StartMetalPipelineManifestReplay(id<MTLDevice> device);
bool WaitForMetalPipelineManifestReplay(unsigned timeoutMillis);

[[nodiscard]] MetalPipelineManifestStatistics GetMetalPipelineManifestStatistics();

}
#endif
