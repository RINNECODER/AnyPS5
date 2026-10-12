// The MSL disk cache returns exactly what was stored, for the same key only,
// and rejects damaged entries.

#include "MetalConversionDiskCache.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace ShaderRecompiler;
using namespace ShaderRecompiler::MetalBackend;

namespace {

int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

InterfaceLayout Layout(std::uint32_t seed) {
    InterfaceLayout layout;
    layout.stride = seed;
    layout.alignment = seed + 1;
    layout.members.push_back({seed + 2, std::nullopt, 4, 32, 8, 16});
    layout.members.push_back({std::nullopt, seed + 3, 1, 16, 24, 2});
    return layout;
}

// Every field set to something other than its default.
Result Sample() {
    Result result;
    result.capturedSamplerRequirements = {{1, 2, 3}, {4, 5, 6}};
    result.minimumLodImages = {{7, 8, 9}};
    result.requiresTextureLodQueries = true;
    result.minimumLodPairs = {{1, 2, 3, 4, 5, 6, 7, 8, 0.25f}};
    result.stage = ShaderStage::Fragment;
    result.vertexBufferCount = 3;
    result.source = std::string("#include <metal_stdlib>\nfragment float4 main0() { return float4(1); }\n") + std::string(70000, 'x');
    result.entryPoint = "main0";
    result.guest.workgroupMemoryDwords = 7;
    result.resources.push_back({1, 2, 3, DescriptorKind::StorageBuffer, DescriptorRole::GuestImages, 4u, std::nullopt, 6u, true, true, true});
    result.resources.push_back({7, 8, 9, DescriptorKind::UniformBuffer, DescriptorRole::GuestBuffers, std::nullopt, 10u, std::nullopt, false, false, false});
    result.pushConstantBuffer = 30;
    result.pushConstantBytes = 64;
    result.pushConstantData = {std::byte{1}, std::byte{2}, std::byte{3}};
    result.bufferSizesBuffer = 29;
    result.samplerArgumentBuffer = 28;
    result.samplerArgumentCount = 5;
    result.threadsPerThreadgroup = {8, 4, 2};
    result.capabilities = {1, 2, 3};
    result.extensions = {"SPV_KHR_a", "SPV_EXT_b"};
    result.requiresWorkgroupAtomicFences = true;
    result.requiresGpuAddresses = true;
    result.requiresInt64 = true;
    result.requiresSimdGroups = true;
    result.mesh = MeshOutputInfo{64, 126};
    result.nativeExecutionKind = NativeExecutionKind::Mesh;
    RectListInfo rect;
    rect.mode = RectListMode::Control;
    rect.indexSize = 2;
    rect.inputControlPoints = 5;
    rect.outputControlPoints = 6;
    rect.inputLayout = Layout(10);
    rect.outputLayout = Layout(20);
    rect.buffers = {{ImplicitBufferRole::Indices, 21, 2}, {ImplicitBufferRole::StageOutput, 28, 16}};
    result.rectList = rect;
    return result;
}

bool SameLayout(const InterfaceLayout& a, const InterfaceLayout& b) {
    if (a.stride != b.stride || a.alignment != b.alignment || a.members.size() != b.members.size()) return false;
    for (std::size_t i = 0; i < a.members.size(); ++i) {
        const auto &x = a.members[i], &y = b.members[i];
        if (x.location != y.location || x.builtin != y.builtin || x.components != y.components || x.width != y.width || x.offset != y.offset || x.bytes != y.bytes)
            return false;
    }
    return true;
}

bool Same(const Result& a, const Result& b) {
    const auto triples = [](const auto& x, const auto& y) {
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i].descriptorSet != y[i].descriptorSet || x[i].binding != y[i].binding || x[i].element != y[i].element) return false;
        return true;
    };
    if (!triples(a.capturedSamplerRequirements, b.capturedSamplerRequirements) || !triples(a.minimumLodImages, b.minimumLodImages)) return false;
    if (a.requiresTextureLodQueries != b.requiresTextureLodQueries || a.minimumLodPairs.size() != b.minimumLodPairs.size()) return false;
    for (std::size_t i = 0; i < a.minimumLodPairs.size(); ++i) {
        const auto &x = a.minimumLodPairs[i], &y = b.minimumLodPairs[i];
        if (x.imageSet != y.imageSet || x.imageBinding != y.imageBinding || x.imageElement != y.imageElement || x.samplerSet != y.samplerSet ||
            x.samplerBinding != y.samplerBinding || x.samplerElement != y.samplerElement || x.magArgument != y.magArgument || x.minArgument != y.minArgument ||
            x.relativeViewMin != y.relativeViewMin)
            return false;
    }
    if (a.stage != b.stage || a.vertexBufferCount != b.vertexBufferCount || a.source != b.source || a.entryPoint != b.entryPoint) return false;
    if (a.resources.size() != b.resources.size()) return false;
    for (std::size_t i = 0; i < a.resources.size(); ++i) {
        const auto &x = a.resources[i], &y = b.resources[i];
        if (x.descriptorSet != y.descriptorSet || x.binding != y.binding || x.count != y.count || x.kind != y.kind || x.role != y.role || x.buffer != y.buffer ||
            x.texture != y.texture || x.sampler != y.sampler || x.active != y.active || x.requiresByteLengths != y.requiresByteLengths ||
            x.unsignedStorageImage != y.unsignedStorageImage)
            return false;
    }
    if (a.pushConstantBuffer != b.pushConstantBuffer || a.pushConstantBytes != b.pushConstantBytes || a.pushConstantData != b.pushConstantData ||
        a.bufferSizesBuffer != b.bufferSizesBuffer || a.samplerArgumentBuffer != b.samplerArgumentBuffer || a.samplerArgumentCount != b.samplerArgumentCount ||
        a.threadsPerThreadgroup != b.threadsPerThreadgroup || a.capabilities != b.capabilities || a.extensions != b.extensions)
        return false;
    if (a.requiresWorkgroupAtomicFences != b.requiresWorkgroupAtomicFences || a.requiresGpuAddresses != b.requiresGpuAddresses ||
        a.requiresInt64 != b.requiresInt64 || a.requiresSimdGroups != b.requiresSimdGroups || a.nativeExecutionKind != b.nativeExecutionKind)
        return false;
    if (a.mesh.has_value() != b.mesh.has_value() || (a.mesh && (a.mesh->maxVertices != b.mesh->maxVertices || a.mesh->maxPrimitives != b.mesh->maxPrimitives)))
        return false;
    if (a.rectList.has_value() != b.rectList.has_value()) return false;
    if (a.rectList) {
        const auto &x = *a.rectList, &y = *b.rectList;
        if (x.mode != y.mode || x.indexSize != y.indexSize || x.inputControlPoints != y.inputControlPoints || x.outputControlPoints != y.outputControlPoints ||
            !SameLayout(x.inputLayout, y.inputLayout) || !SameLayout(x.outputLayout, y.outputLayout) || x.buffers.size() != y.buffers.size())
            return false;
        for (std::size_t i = 0; i < x.buffers.size(); ++i)
            if (x.buffers[i].role != y.buffers[i].role || x.buffers[i].index != y.buffers[i].index || x.buffers[i].elementBytes != y.buffers[i].elementBytes) return false;
    }
    return true;
}

}

int main() {
    char directory[] = "/tmp/anyps5-msl-cache-XXXXXX";
    if (::mkdtemp(directory) == nullptr) return 1;
    ::setenv("ANYPS5_SHADER_CACHE_DIR", directory, 1);
    ::unsetenv("ANYPS5_NO_SHADER_CACHE");
    ::unsetenv("APS5_NO_SHADER_CACHE");

    const auto stored = Sample();
    const std::string key = "conversion key " + std::string(1000, 'k');
    auto entry = ConversionDiskCache::EncodeEntry(key, stored);
    require(!entry.empty() && entry.size() < stored.source.size(), "an entry is not compressed");

    Result decoded;
    decoded.guest.workgroupMemoryDwords = 42;
    require(ConversionDiskCache::DecodeEntry(entry, key, decoded) && Same(decoded, stored), "a decoded entry differs from the stored result");
    require(decoded.guest.workgroupMemoryDwords == 42, "decoding replaced the caller's guest");
    Result other;
    require(!ConversionDiskCache::DecodeEntry(entry, key + "!", other), "an entry was accepted for another key");
    auto damaged = entry;
    damaged[damaged.size() / 2] ^= std::byte{0x40};
    require(!ConversionDiskCache::DecodeEntry(damaged, key, other), "a damaged entry was accepted");
    auto truncated = entry;
    truncated.resize(entry.size() - 1);
    require(!ConversionDiskCache::DecodeEntry(truncated, key, other), "a truncated entry was accepted");

    Result loaded;
    require(!ConversionDiskCache::Load(key, loaded), "an empty cache returned an entry");
    ConversionDiskCache::Store(key, stored);
    require(ConversionDiskCache::Load(key, loaded) && Same(loaded, stored), "a stored result did not load back");
    require(!ConversionDiskCache::Load(key + "?", loaded), "the cache returned an entry for another key");

    std::error_code error;
    std::filesystem::remove_all(directory, error);
    if (failures != 0) return 1;
    std::printf("metal conversion disk cache: ok\n");
    return 0;
}
