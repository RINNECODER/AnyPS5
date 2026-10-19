#include "MetalPipelineManifest.hpp"
#include "MetalPipelineCache.hpp"
#include "MetalWorkerPool.hpp"
#include "ShaderCacheDirectory.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AgcDriver::Metal {

namespace {

using ShaderRecompiler::HashBytes;
using ShaderRecompiler::ReadWholeFile;
using ShaderRecompiler::WriteFileAtomically;
using ShaderRecompiler::MetalBackend::ShaderSource;

constexpr std::uint32_t ManifestMagic = 0x354d5041u;
constexpr std::uint32_t ManifestFormat = 1;
constexpr std::size_t ManifestRecipeLimit = 8192;
constexpr NSUInteger ManifestColorAttachments = 8;
constexpr NSUInteger ManifestBufferSlots = 31;
constexpr NSUInteger ManifestVertexSlots = 31;
constexpr std::uint8_t RecipeRender = 1;
constexpr std::uint8_t RecipeCompute = 2;

struct Writer {
    std::string bytes;
    void U8(std::uint8_t value) { bytes.push_back(static_cast<char>(value)); }
    void U32(std::uint32_t value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
    void U64(std::uint64_t value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
    void Raw(const void* data, std::size_t size) { bytes.append(static_cast<const char*>(data), size); }
    void Text(std::string_view text) {
        U32(static_cast<std::uint32_t>(text.size()));
        bytes.append(text.data(), text.size());
    }
};

struct Reader {
    std::string_view bytes;
    std::size_t offset = 0;
    bool ok = true;

    bool Has(std::size_t size) const { return ok && offset + size <= bytes.size(); }
    std::uint8_t U8() {
        if (!Has(1)) { ok = false; return 0; }
        return static_cast<std::uint8_t>(bytes[offset++]);
    }
    std::uint32_t U32() {
        if (!Has(4)) { ok = false; return 0; }
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        offset += 4;
        return value;
    }
    std::uint64_t U64() {
        if (!Has(8)) { ok = false; return 0; }
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 8);
        offset += 8;
        return value;
    }
    std::string Text() {
        const auto size = U32();
        if (!Has(size)) { ok = false; return {}; }
        std::string value(bytes.substr(offset, size));
        offset += size;
        return value;
    }
};

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

std::string_view AsView(const std::string& text) { return {text.data(), text.size()}; }

std::shared_ptr<const std::string> SharedSource(const ShaderSource& source) {
    if (auto shared = source.Shared()) return shared;
    return std::make_shared<const std::string>(source.str());
}

void WriteVertexDescriptor(Writer& writer, MTLVertexDescriptor* descriptor) {
    writer.U8(descriptor == nil ? 0 : 1);
    if (descriptor == nil) return;
    for (NSUInteger i = 0; i < ManifestVertexSlots; ++i) {
        const auto attribute = descriptor.attributes[i];
        writer.U32(static_cast<std::uint32_t>(attribute.format));
        writer.U32(static_cast<std::uint32_t>(attribute.offset));
        writer.U32(static_cast<std::uint32_t>(attribute.bufferIndex));
        const auto layout = descriptor.layouts[i];
        writer.U32(static_cast<std::uint32_t>(layout.stride));
        writer.U32(static_cast<std::uint32_t>(layout.stepFunction));
        writer.U32(static_cast<std::uint32_t>(layout.stepRate));
    }
}

MTLVertexDescriptor* ReadVertexDescriptor(Reader& reader) {
    if (reader.U8() == 0) return nil;
    MTLVertexDescriptor* descriptor = [MTLVertexDescriptor vertexDescriptor];
    for (NSUInteger i = 0; i < ManifestVertexSlots; ++i) {
        auto attribute = descriptor.attributes[i];
        attribute.format = static_cast<MTLVertexFormat>(reader.U32());
        attribute.offset = reader.U32();
        attribute.bufferIndex = reader.U32();
        auto layout = descriptor.layouts[i];
        layout.stride = reader.U32();
        layout.stepFunction = static_cast<MTLVertexStepFunction>(reader.U32());
        layout.stepRate = reader.U32();
    }
    return descriptor;
}

void WriteStageInputs(Writer& writer, MTLStageInputOutputDescriptor* descriptor) {
    writer.U8(descriptor == nil ? 0 : 1);
    if (descriptor == nil) return;
    for (NSUInteger i = 0; i < ManifestVertexSlots; ++i) {
        const auto attribute = descriptor.attributes[i];
        writer.U32(static_cast<std::uint32_t>(attribute.format));
        writer.U32(static_cast<std::uint32_t>(attribute.offset));
        writer.U32(static_cast<std::uint32_t>(attribute.bufferIndex));
        const auto layout = descriptor.layouts[i];
        writer.U32(static_cast<std::uint32_t>(layout.stride));
        writer.U32(static_cast<std::uint32_t>(layout.stepFunction));
        writer.U32(static_cast<std::uint32_t>(layout.stepRate));
    }
    writer.U32(static_cast<std::uint32_t>(descriptor.indexType));
    writer.U32(static_cast<std::uint32_t>(descriptor.indexBufferIndex));
}

MTLStageInputOutputDescriptor* ReadStageInputs(Reader& reader) {
    if (reader.U8() == 0) return nil;
    MTLStageInputOutputDescriptor* descriptor = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    for (NSUInteger i = 0; i < ManifestVertexSlots; ++i) {
        auto attribute = descriptor.attributes[i];
        attribute.format = static_cast<MTLAttributeFormat>(reader.U32());
        attribute.offset = reader.U32();
        attribute.bufferIndex = reader.U32();
        auto layout = descriptor.layouts[i];
        layout.stride = reader.U32();
        layout.stepFunction = static_cast<MTLStepFunction>(reader.U32());
        layout.stepRate = reader.U32();
    }
    descriptor.indexType = static_cast<MTLIndexType>(reader.U32());
    descriptor.indexBufferIndex = reader.U32();
    return descriptor;
}

std::string EncodeRenderDescriptor(MTLRenderPipelineDescriptor* descriptor) {
    Writer writer;
    for (NSUInteger i = 0; i < ManifestColorAttachments; ++i) {
        const auto color = descriptor.colorAttachments[i];
        writer.U32(static_cast<std::uint32_t>(color.pixelFormat));
        writer.U8(color.blendingEnabled ? 1 : 0);
        writer.U32(static_cast<std::uint32_t>(color.sourceRGBBlendFactor));
        writer.U32(static_cast<std::uint32_t>(color.destinationRGBBlendFactor));
        writer.U32(static_cast<std::uint32_t>(color.rgbBlendOperation));
        writer.U32(static_cast<std::uint32_t>(color.sourceAlphaBlendFactor));
        writer.U32(static_cast<std::uint32_t>(color.destinationAlphaBlendFactor));
        writer.U32(static_cast<std::uint32_t>(color.alphaBlendOperation));
        writer.U32(static_cast<std::uint32_t>(color.writeMask));
    }
    writer.U32(static_cast<std::uint32_t>(descriptor.depthAttachmentPixelFormat));
    writer.U32(static_cast<std::uint32_t>(descriptor.stencilAttachmentPixelFormat));
    writer.U32(static_cast<std::uint32_t>(descriptor.rasterSampleCount));
    writer.U8(descriptor.alphaToCoverageEnabled ? 1 : 0);
    writer.U8(descriptor.alphaToOneEnabled ? 1 : 0);
    writer.U8(descriptor.rasterizationEnabled ? 1 : 0);
    writer.U32(static_cast<std::uint32_t>(descriptor.inputPrimitiveTopology));
    writer.U32(static_cast<std::uint32_t>(descriptor.tessellationPartitionMode));
    writer.U32(static_cast<std::uint32_t>(descriptor.maxTessellationFactor));
    writer.U8(descriptor.tessellationFactorScaleEnabled ? 1 : 0);
    writer.U32(static_cast<std::uint32_t>(descriptor.tessellationFactorFormat));
    writer.U32(static_cast<std::uint32_t>(descriptor.tessellationControlPointIndexType));
    writer.U32(static_cast<std::uint32_t>(descriptor.tessellationFactorStepFunction));
    writer.U32(static_cast<std::uint32_t>(descriptor.tessellationOutputWindingOrder));
    writer.U8(descriptor.supportIndirectCommandBuffers ? 1 : 0);
    writer.U32(static_cast<std::uint32_t>(descriptor.maxVertexAmplificationCount));
    writer.U32(static_cast<std::uint32_t>(descriptor.maxVertexCallStackDepth));
    writer.U32(static_cast<std::uint32_t>(descriptor.maxFragmentCallStackDepth));
    writer.U8(descriptor.supportAddingVertexBinaryFunctions ? 1 : 0);
    writer.U8(descriptor.supportAddingFragmentBinaryFunctions ? 1 : 0);
    WriteVertexDescriptor(writer, descriptor.vertexDescriptor);
    for (NSUInteger i = 0; i < ManifestBufferSlots; ++i) writer.U32(descriptor.vertexBuffers[i].mutability);
    for (NSUInteger i = 0; i < ManifestBufferSlots; ++i) writer.U32(descriptor.fragmentBuffers[i].mutability);
    return std::move(writer.bytes);
}

MTLRenderPipelineDescriptor* DecodeRenderDescriptor(std::string_view bytes) {
    Reader reader{bytes};
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    for (NSUInteger i = 0; i < ManifestColorAttachments; ++i) {
        auto color = descriptor.colorAttachments[i];
        color.pixelFormat = static_cast<MTLPixelFormat>(reader.U32());
        color.blendingEnabled = reader.U8() != 0;
        color.sourceRGBBlendFactor = static_cast<MTLBlendFactor>(reader.U32());
        color.destinationRGBBlendFactor = static_cast<MTLBlendFactor>(reader.U32());
        color.rgbBlendOperation = static_cast<MTLBlendOperation>(reader.U32());
        color.sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(reader.U32());
        color.destinationAlphaBlendFactor = static_cast<MTLBlendFactor>(reader.U32());
        color.alphaBlendOperation = static_cast<MTLBlendOperation>(reader.U32());
        color.writeMask = static_cast<MTLColorWriteMask>(reader.U32());
    }
    descriptor.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(reader.U32());
    descriptor.stencilAttachmentPixelFormat = static_cast<MTLPixelFormat>(reader.U32());
    descriptor.rasterSampleCount = reader.U32();
    if (descriptor.rasterSampleCount == 0) descriptor.rasterSampleCount = 1;
    descriptor.alphaToCoverageEnabled = reader.U8() != 0;
    descriptor.alphaToOneEnabled = reader.U8() != 0;
    descriptor.rasterizationEnabled = reader.U8() != 0;
    descriptor.inputPrimitiveTopology = static_cast<MTLPrimitiveTopologyClass>(reader.U32());
    descriptor.tessellationPartitionMode = static_cast<MTLTessellationPartitionMode>(reader.U32());
    descriptor.maxTessellationFactor = reader.U32();
    descriptor.tessellationFactorScaleEnabled = reader.U8() != 0;
    descriptor.tessellationFactorFormat = static_cast<MTLTessellationFactorFormat>(reader.U32());
    descriptor.tessellationControlPointIndexType = static_cast<MTLTessellationControlPointIndexType>(reader.U32());
    descriptor.tessellationFactorStepFunction = static_cast<MTLTessellationFactorStepFunction>(reader.U32());
    descriptor.tessellationOutputWindingOrder = static_cast<MTLWinding>(reader.U32());
    descriptor.supportIndirectCommandBuffers = reader.U8() != 0;
    descriptor.maxVertexAmplificationCount = reader.U32();
    descriptor.maxVertexCallStackDepth = reader.U32();
    descriptor.maxFragmentCallStackDepth = reader.U32();
    descriptor.supportAddingVertexBinaryFunctions = reader.U8() != 0;
    descriptor.supportAddingFragmentBinaryFunctions = reader.U8() != 0;
    descriptor.vertexDescriptor = ReadVertexDescriptor(reader);
    for (NSUInteger i = 0; i < ManifestBufferSlots; ++i) descriptor.vertexBuffers[i].mutability = static_cast<MTLMutability>(reader.U32());
    for (NSUInteger i = 0; i < ManifestBufferSlots; ++i) descriptor.fragmentBuffers[i].mutability = static_cast<MTLMutability>(reader.U32());
    if (!reader.ok) return nil;
    return descriptor;
}

std::string EncodeComputeDescriptor(MTLComputePipelineDescriptor* descriptor) {
    Writer writer;
    writer.U8(descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth ? 1 : 0);
    writer.U32(static_cast<std::uint32_t>(descriptor.maxTotalThreadsPerThreadgroup));
    writer.U8(descriptor.supportIndirectCommandBuffers ? 1 : 0);
    writer.U32(static_cast<std::uint32_t>(descriptor.maxCallStackDepth));
    writer.U8(descriptor.supportAddingBinaryFunctions ? 1 : 0);
    WriteStageInputs(writer, descriptor.stageInputDescriptor);
    for (NSUInteger i = 0; i < ManifestBufferSlots; ++i) writer.U32(descriptor.buffers[i].mutability);
    return std::move(writer.bytes);
}

MTLComputePipelineDescriptor* DecodeComputeDescriptor(std::string_view bytes) {
    Reader reader{bytes};
    MTLComputePipelineDescriptor* descriptor = [[MTLComputePipelineDescriptor alloc] init];
    descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth = reader.U8() != 0;
    descriptor.maxTotalThreadsPerThreadgroup = reader.U32();
    descriptor.supportIndirectCommandBuffers = reader.U8() != 0;
    descriptor.maxCallStackDepth = reader.U32();
    descriptor.supportAddingBinaryFunctions = reader.U8() != 0;
    descriptor.stageInputDescriptor = ReadStageInputs(reader);
    for (NSUInteger i = 0; i < ManifestBufferSlots; ++i) descriptor.buffers[i].mutability = static_cast<MTLMutability>(reader.U32());
    if (!reader.ok) return nil;
    return descriptor;
}

struct Recipe {
    std::uint8_t kind = 0;
    std::shared_ptr<const std::string> source0;
    std::shared_ptr<const std::string> source1;
    std::string entry0;
    std::string entry1;
    bool language32_0 = false;
    bool language32_1 = false;
    std::string descriptor;
};

std::mutex recipeMutex;
std::deque<std::pair<std::string, Recipe>> recipes;
std::unordered_set<std::string> recipeKeys;
std::string recipePath;
std::string recipeDeviceName;
std::string recipeOsVersion;

std::atomic<std::uint64_t> manifestLoaded{0};
std::atomic<std::uint64_t> manifestReplayed{0};
std::atomic<std::uint64_t> manifestFailed{0};
std::atomic<std::uint64_t> manifestMicros{0};

bool ManifestEnabled() {
    if (ShaderRecompiler::ShaderCacheDirectory().empty()) return false;
    return !ShaderRecompiler::ShaderCacheDisabledBy("ANYPS5_NO_PSO_MANIFEST");
}

struct ManifestIdentity {
    std::string path;
    std::string deviceName;
    std::string osVersion;
};

std::string TitleKey() {
    std::ifstream stream(std::filesystem::current_path() / "app0" / "sce_sys" / "param.json");
    if (!stream) return "default";
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (text.empty()) return "default";
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx",
                  static_cast<unsigned long long>(HashBytes({reinterpret_cast<const std::byte*>(text.data()), text.size()})));
    return name;
}

ManifestIdentity IdentityFor(id<MTLDevice> device) {
    ManifestIdentity identity;
    const auto directory = ShaderRecompiler::ShaderCacheDirectory();
    if (directory.empty()) return identity;
    identity.deviceName = Sanitized(device.name);
    identity.osVersion = Sanitized(NSProcessInfo.processInfo.operatingSystemVersionString);
    const std::string math = FastMathEnabled() ? "fastmath" : "safemath";
    identity.path = (directory / ("metal-pso-manifest-v1-" + math + "-" + TitleKey() + "-" + identity.deviceName + "-" +
                                  identity.osVersion + ".bin")).string();
    return identity;
}

std::string RecipeKey(const Recipe& recipe) {
    Writer writer;
    writer.U8(recipe.kind);
    writer.U64(std::hash<std::string_view>{}(AsView(*recipe.source0)));
    writer.U64(recipe.source1 == nullptr ? 0 : std::hash<std::string_view>{}(AsView(*recipe.source1)));
    writer.Text(recipe.entry0);
    writer.Text(recipe.entry1);
    writer.U8(recipe.language32_0 ? 1 : 0);
    writer.U8(recipe.language32_1 ? 1 : 0);
    writer.U64(HashBytes({reinterpret_cast<const std::byte*>(recipe.descriptor.data()), recipe.descriptor.size()}));
    return std::move(writer.bytes);
}

void RegisterFlushAtExit() {
    static std::once_flag registered;
    std::call_once(registered, [] { std::atexit(FlushMetalPipelineManifest); });
}

void AddRecipe(id<MTLDevice> device, Recipe recipe) {
    if (!ManifestEnabled() || device == nil) return;
    auto key = RecipeKey(recipe);
    std::lock_guard lock(recipeMutex);
    if (recipeKeys.count(key) != 0) return;
    if (recipePath.empty()) {
        const auto identity = IdentityFor(device);
        recipePath = identity.path;
        recipeDeviceName = identity.deviceName;
        recipeOsVersion = identity.osVersion;
    }
    if (recipePath.empty()) return;
    recipes.emplace_back(std::move(key), std::move(recipe));
    recipeKeys.insert(recipes.back().first);
    while (recipes.size() > ManifestRecipeLimit) {
        recipeKeys.erase(recipes.front().first);
        recipes.pop_front();
    }
    RegisterFlushAtExit();
}

struct DecodedRecipe {
    std::uint8_t kind = 0;
    const std::string* source0 = nullptr;
    const std::string* source1 = nullptr;
    const std::string* entry0 = nullptr;
    const std::string* entry1 = nullptr;
    bool language32_0 = false;
    bool language32_1 = false;
    std::string descriptor;
};

struct DecodedManifest {
    std::vector<std::string> strings;
    std::vector<DecodedRecipe> recipes;
};

bool DecodeManifest(const std::vector<std::byte>& file, id<MTLDevice> device, DecodedManifest& manifest) {
    Reader reader{std::string_view(reinterpret_cast<const char*>(file.data()), file.size())};
    if (reader.U32() != ManifestMagic) return false;
    if (reader.U32() != ManifestFormat) return false;
    const bool fastMath = reader.U8() != 0;
    const auto deviceName = reader.Text();
    const auto osVersion = reader.Text();
    const auto payloadBytes = reader.U64();
    const auto payloadHash = reader.U64();
    if (!reader.ok || deviceName != Sanitized(device.name) ||
        osVersion != Sanitized(NSProcessInfo.processInfo.operatingSystemVersionString) || fastMath != FastMathEnabled()) return false;
    if (payloadBytes != file.size() - reader.offset) return false;
    const auto payload = std::span(file).subspan(reader.offset);
    if (HashBytes(payload) != payloadHash) return false;

    Reader body{std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size())};
    const auto stringCount = body.U32();
    manifest.strings.reserve(stringCount);
    for (std::uint32_t i = 0; i < stringCount && body.ok; ++i) manifest.strings.push_back(body.Text());
    const auto recipeCount = body.U32();
    manifest.recipes.reserve(recipeCount);
    for (std::uint32_t i = 0; i < recipeCount && body.ok; ++i) {
        DecodedRecipe recipe;
        recipe.kind = body.U8();
        const auto source0 = body.U32();
        const auto source1 = body.U32();
        const auto entry0 = body.U32();
        const auto entry1 = body.U32();
        recipe.language32_0 = body.U8() != 0;
        recipe.language32_1 = body.U8() != 0;
        const auto descriptorBytes = body.U32();
        if (!body.Has(descriptorBytes)) { body.ok = false; break; }
        recipe.descriptor.assign(reinterpret_cast<const char*>(payload.data()) + body.offset, descriptorBytes);
        body.offset += descriptorBytes;
        if (source0 >= manifest.strings.size() || source1 >= manifest.strings.size() ||
            entry0 >= manifest.strings.size() || entry1 >= manifest.strings.size()) return false;
        recipe.source0 = &manifest.strings[source0];
        recipe.source1 = &manifest.strings[source1];
        recipe.entry0 = &manifest.strings[entry0];
        recipe.entry1 = &manifest.strings[entry1];
        manifest.recipes.push_back(std::move(recipe));
    }
    return body.ok;
}

void EncodeManifest(const std::string& deviceName, const std::string& osVersion,
                    std::vector<std::pair<std::string, Recipe>>& snapshot, std::string& out) {
    std::vector<std::string> strings;
    std::unordered_map<std::string, std::uint32_t> indices;
    const auto intern = [&](const std::string& text) {
        if (const auto found = indices.find(text); found != indices.end()) return found->second;
        const auto index = static_cast<std::uint32_t>(strings.size());
        strings.push_back(text);
        indices.emplace(strings.back(), index);
        return index;
    };
    Writer payload;
    Writer body;
    std::vector<std::uint32_t> references;
    for (const auto& [key, recipe] : snapshot) {
        references.push_back(intern(*recipe.source0));
        references.push_back(recipe.source1 == nullptr ? intern("") : intern(*recipe.source1));
        references.push_back(intern(recipe.entry0));
        references.push_back(intern(recipe.entry1));
    }
    body.U32(static_cast<std::uint32_t>(strings.size()));
    for (const auto& text : strings) body.Text(text);
    body.U32(static_cast<std::uint32_t>(snapshot.size()));
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const auto& recipe = snapshot[i].second;
        body.U8(recipe.kind);
        body.U32(references[i * 4 + 0]);
        body.U32(references[i * 4 + 1]);
        body.U32(references[i * 4 + 2]);
        body.U32(references[i * 4 + 3]);
        body.U8(recipe.language32_0 ? 1 : 0);
        body.U8(recipe.language32_1 ? 1 : 0);
        body.U32(static_cast<std::uint32_t>(recipe.descriptor.size()));
        body.Raw(recipe.descriptor.data(), recipe.descriptor.size());
    }
    payload.U32(ManifestMagic);
    payload.U32(ManifestFormat);
    payload.U8(FastMathEnabled() ? 1 : 0);
    payload.Text(deviceName);
    payload.Text(osVersion);
    payload.U64(body.bytes.size());
    payload.U64(HashBytes({reinterpret_cast<const std::byte*>(body.bytes.data()), body.bytes.size()}));
    payload.bytes.append(body.bytes);
    out = std::move(payload.bytes);
}

std::mutex replayMutex;
std::condition_variable replayCv;
bool replayDone = true;

void ReplayDecoded(id<MTLDevice> device, const DecodedManifest& manifest) {
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t replayed = 0;
    std::uint64_t failed = 0;
    for (const auto& recipe : manifest.recipes) {
        @autoreleasepool {
            try {
                if (recipe.kind == RecipeRender) {
                    id<MTLFunction> vertexFunction = CachedShaderFunction(device, *recipe.source0, *recipe.entry0, recipe.language32_0, 0);
                    id<MTLFunction> fragmentFunction = CachedShaderFunction(device, *recipe.source1, *recipe.entry1, recipe.language32_1, 0);
                    if (vertexFunction == nil || fragmentFunction == nil) { ++failed; continue; }
                    MTLRenderPipelineDescriptor* descriptor = DecodeRenderDescriptor(recipe.descriptor);
                    if (descriptor == nil) { ++failed; continue; }
                    NSError* error = nil;
                    const auto created = CachedRenderPipelineState(device, descriptor, vertexFunction, fragmentFunction, &error);
                    if (created.state == nil) { ++failed; continue; }
                    ++replayed;
                } else if (recipe.kind == RecipeCompute) {
                    id<MTLFunction> function = CachedShaderFunction(device, *recipe.source0, *recipe.entry0, recipe.language32_0, 0);
                    if (function == nil) { ++failed; continue; }
                    MTLComputePipelineDescriptor* descriptor = DecodeComputeDescriptor(recipe.descriptor);
                    if (descriptor == nil) { ++failed; continue; }
                    descriptor.computeFunction = function;
                    NSError* error = nil;
                    const auto created = CachedComputePipelineState(device, descriptor, &error);
                    if (created.state == nil) { ++failed; continue; }
                    ++replayed;
                } else {
                    ++failed;
                }
            } catch (...) {
                ++failed;
            }
        }
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
    manifestReplayed.fetch_add(replayed, std::memory_order_relaxed);
    manifestFailed.fetch_add(failed, std::memory_order_relaxed);
    manifestMicros.fetch_add(static_cast<std::uint64_t>(micros), std::memory_order_relaxed);
    manifestLoaded.fetch_add(1, std::memory_order_relaxed);
}

}

void RecordRenderPipelineRecipe(id<MTLDevice> device, const ShaderRecompiler::MetalBackend::Result& vertex,
                                const ShaderRecompiler::MetalBackend::Result& fragment, MTLRenderPipelineDescriptor* descriptor) {
    if (device == nil || descriptor == nil || !ManifestEnabled() || !RenderPipelineCacheable(descriptor)) return;
    Recipe recipe;
    recipe.kind = RecipeRender;
    recipe.source0 = SharedSource(vertex.source);
    recipe.source1 = SharedSource(fragment.source);
    recipe.entry0 = vertex.entryPoint;
    recipe.entry1 = fragment.entryPoint;
    recipe.language32_0 = vertex.requiresWorkgroupAtomicFences;
    recipe.language32_1 = fragment.requiresWorkgroupAtomicFences;
    recipe.descriptor = EncodeRenderDescriptor(descriptor);
    AddRecipe(device, std::move(recipe));
}

void RecordComputePipelineRecipe(id<MTLDevice> device, const ShaderRecompiler::MetalBackend::Result& shader,
                                 MTLComputePipelineDescriptor* descriptor) {
    if (device == nil || descriptor == nil || !ManifestEnabled() || !ComputePipelineCacheable(descriptor)) return;
    Recipe recipe;
    recipe.kind = RecipeCompute;
    recipe.source0 = SharedSource(shader.source);
    recipe.entry0 = shader.entryPoint;
    recipe.language32_0 = shader.requiresWorkgroupAtomicFences;
    recipe.descriptor = EncodeComputeDescriptor(descriptor);
    AddRecipe(device, std::move(recipe));
}

void FlushMetalPipelineManifest() {
    if (!ManifestEnabled()) return;
    std::vector<std::pair<std::string, Recipe>> snapshot;
    std::string path;
    std::string deviceName;
    std::string osVersion;
    {
        std::lock_guard lock(recipeMutex);
        path = recipePath;
        deviceName = recipeDeviceName;
        osVersion = recipeOsVersion;
        snapshot.assign(recipes.begin(), recipes.end());
    }
    if (path.empty() || snapshot.empty()) return;
    std::string bytes;
    EncodeManifest(deviceName, osVersion, snapshot, bytes);
    WriteFileAtomically(path, {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
}

bool ReplayMetalPipelineManifest(id<MTLDevice> device) {
    if (device == nil || !ManifestEnabled()) return false;
    const auto path = IdentityFor(device).path;
    if (path.empty()) return false;
    std::vector<std::byte> file;
    if (!ReadWholeFile(path, file)) return false;
    DecodedManifest manifest;
    if (!DecodeManifest(file, device, manifest)) return false;
    ReplayDecoded(device, manifest);
    return true;
}

void StartMetalPipelineManifestReplay(id<MTLDevice> device) {
    if (device == nil || !ManifestEnabled()) return;
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    {
        std::lock_guard lock(replayMutex);
        replayDone = false;
    }
    MetalWorkerPool::Shared().Enqueue([device] {
        ReplayMetalPipelineManifest(device);
        {
            std::lock_guard lock(replayMutex);
            replayDone = true;
        }
        replayCv.notify_all();
    });
}

bool WaitForMetalPipelineManifestReplay(unsigned timeoutMillis) {
    std::unique_lock lock(replayMutex);
    return replayCv.wait_for(lock, std::chrono::milliseconds(timeoutMillis), [] { return replayDone; });
}

MetalPipelineManifestStatistics GetMetalPipelineManifestStatistics() {
    MetalPipelineManifestStatistics result;
    {
        std::lock_guard lock(recipeMutex);
        result.entries = recipes.size();
    }
    result.loaded = manifestLoaded.load(std::memory_order_relaxed);
    result.replayed = manifestReplayed.load(std::memory_order_relaxed);
    result.failed = manifestFailed.load(std::memory_order_relaxed);
    result.micros = manifestMicros.load(std::memory_order_relaxed);
    return result;
}

void ClearMetalPipelineManifest() {
    std::lock_guard lock(recipeMutex);
    recipes.clear();
    recipeKeys.clear();
    recipePath.clear();
    recipeDeviceName.clear();
    recipeOsVersion.clear();
}

}
