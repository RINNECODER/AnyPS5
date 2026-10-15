#include "MetalConversionDiskCache.hpp"
#include "../ShaderCacheDirectory.hpp"
#include "../ShaderCacheEviction.hpp"

#include <compression.h>
#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <type_traits>

#ifndef ANYPS5_MSL_CACHE_VERSION
#define ANYPS5_MSL_CACHE_VERSION "unversioned"
#endif

namespace ShaderRecompiler::MetalBackend::ConversionDiskCache {

namespace {

constexpr std::uint32_t kMagic = 0x4c534d41u;  // "AMSL"
constexpr std::uint32_t kFormat = 1;  // header: magic, format, key digest, size, hash, LZ4 size; then the LZ4 payload

// A new field in one of these structs must be added to Write and Read below
// (and kFormat bumped); these sizes are the reminder. Result::sourceHash and storesThroughBda (8 bytes with padding, included in the size below) are not stored: they are
// derived from the source (ConvertToMetal sets it on every result it returns). The source is stored
// as its text (ShaderSource is a shared, immutable std::string).
static_assert(sizeof(Result) - sizeof(RecompileResult) - sizeof(Result::sourceHash) == 416, "Result changed: update the MSL disk cache encoding");
static_assert(sizeof(ResourceMapping) == 48 && sizeof(RectListInfo) == 104 && sizeof(InterfaceMember) == 32 && sizeof(MinimumLodPair) == 36 &&
                  sizeof(ImplicitBufferMapping) == 12 && sizeof(MeshOutputInfo) == 8 && sizeof(MinimumLodImage) == 12 &&
                  sizeof(CapturedSamplerRequirement) == 12,
              "a Result member changed: update the MSL disk cache encoding");

class Writer {
public:
    std::vector<std::byte> bytes;
    void Raw(const void* data, std::size_t size) {
        const auto* begin = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), begin, begin + size);
    }
    void U32(std::uint32_t value) { Raw(&value, sizeof value); }
    void U64(std::uint64_t value) { Raw(&value, sizeof value); }
    void F32(float value) { Raw(&value, sizeof value); }
    void Bool(bool value) { U32(value ? 1 : 0); }
    template<typename TEnum> void Enum(TEnum value) { U32(static_cast<std::uint32_t>(value)); }
    void Optional(const std::optional<std::uint32_t>& value) {
        Bool(value.has_value());
        U32(value.value_or(0));
    }
    void String(const std::string& value) {
        U64(value.size());
        Raw(value.data(), value.size());
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : _bytes(bytes) {}
    bool ok = true;
    bool Raw(void* data, std::size_t size) {
        if (!ok || size > _bytes.size() - _offset) return ok = false;
        std::memcpy(data, _bytes.data() + _offset, size);
        _offset += size;
        return true;
    }
    std::uint32_t U32() {
        std::uint32_t value = 0;
        Raw(&value, sizeof value);
        return value;
    }
    std::uint64_t U64() {
        std::uint64_t value = 0;
        Raw(&value, sizeof value);
        return value;
    }
    float F32() {
        float value = 0;
        Raw(&value, sizeof value);
        return value;
    }
    bool Bool() { return U32() != 0; }
    template<typename TEnum> TEnum Enum() { return static_cast<TEnum>(U32()); }
    std::optional<std::uint32_t> Optional() {
        const bool present = Bool();
        const auto value = U32();
        return present ? std::optional<std::uint32_t>(value) : std::nullopt;
    }
    std::string String() {
        const auto size = U64();
        if (!ok || size > _bytes.size() - _offset) {
            ok = false;
            return {};
        }
        std::string value(reinterpret_cast<const char*>(_bytes.data() + _offset), static_cast<std::size_t>(size));
        _offset += static_cast<std::size_t>(size);
        return value;
    }
    // Element counts are bounded by the bytes left, so a corrupt count cannot allocate much.
    std::size_t Count(std::size_t minimumElementBytes) {
        const auto count = U64();
        if (!ok || count > (_bytes.size() - _offset) / minimumElementBytes) {
            ok = false;
            return 0;
        }
        return static_cast<std::size_t>(count);
    }
    [[nodiscard]] bool AtEnd() const { return ok && _offset == _bytes.size(); }
    [[nodiscard]] std::size_t Offset() const { return _offset; }

private:
    std::span<const std::byte> _bytes;
    std::size_t _offset = 0;
};

void WriteLayout(Writer& out, const InterfaceLayout& layout) {
    out.U32(layout.stride);
    out.U32(layout.alignment);
    out.U64(layout.members.size());
    for (const auto& member : layout.members) {
        out.Optional(member.location);
        out.Optional(member.builtin);
        out.U32(member.components);
        out.U32(member.width);
        out.U32(member.offset);
        out.U32(member.bytes);
    }
}

void ReadLayout(Reader& in, InterfaceLayout& layout) {
    layout.stride = in.U32();
    layout.alignment = in.U32();
    layout.members.resize(in.Count(32));
    for (auto& member : layout.members) {
        member.location = in.Optional();
        member.builtin = in.Optional();
        member.components = in.U32();
        member.width = in.U32();
        member.offset = in.U32();
        member.bytes = in.U32();
    }
}

void WriteResult(Writer& out, const Result& result) {
    out.U64(result.capturedSamplerRequirements.size());
    for (const auto& item : result.capturedSamplerRequirements) {
        out.U32(item.descriptorSet);
        out.U32(item.binding);
        out.U32(item.element);
    }
    out.U64(result.minimumLodImages.size());
    for (const auto& item : result.minimumLodImages) {
        out.U32(item.descriptorSet);
        out.U32(item.binding);
        out.U32(item.element);
    }
    out.Bool(result.requiresTextureLodQueries);
    out.U64(result.minimumLodPairs.size());
    for (const auto& pair : result.minimumLodPairs) {
        for (const auto value : {pair.imageSet, pair.imageBinding, pair.imageElement, pair.samplerSet, pair.samplerBinding, pair.samplerElement, pair.magArgument, pair.minArgument})
            out.U32(value);
        out.F32(pair.relativeViewMin);
    }
    out.Enum(result.stage);
    out.U32(result.vertexBufferCount);
    out.String(result.source);
    out.String(result.entryPoint);
    out.U64(result.resources.size());
    for (const auto& resource : result.resources) {
        out.U32(resource.descriptorSet);
        out.U32(resource.binding);
        out.U32(resource.count);
        out.Enum(resource.kind);
        out.Enum(resource.role);
        out.Optional(resource.buffer);
        out.Optional(resource.texture);
        out.Optional(resource.sampler);
        out.Bool(resource.active);
        out.Bool(resource.requiresByteLengths);
        out.Bool(resource.unsignedStorageImage);
    }
    out.Optional(result.pushConstantBuffer);
    out.U32(result.pushConstantBytes);
    out.U64(result.pushConstantData.size());
    out.Raw(result.pushConstantData.data(), result.pushConstantData.size());
    out.Optional(result.bufferSizesBuffer);
    out.Optional(result.samplerArgumentBuffer);
    out.U32(result.samplerArgumentCount);
    for (const auto value : result.threadsPerThreadgroup) out.U32(value);
    out.U64(result.capabilities.size());
    for (const auto value : result.capabilities) out.U32(value);
    out.U64(result.extensions.size());
    for (const auto& extension : result.extensions) out.String(extension);
    out.Bool(result.requiresWorkgroupAtomicFences);
    out.Bool(result.requiresGpuAddresses);
    out.Bool(result.requiresInt64);
    out.Bool(result.requiresSimdGroups);
    out.Bool(result.mesh.has_value());
    if (result.mesh) {
        out.U32(result.mesh->maxVertices);
        out.U32(result.mesh->maxPrimitives);
    }
    out.Enum(result.nativeExecutionKind);
    out.Bool(result.rectList.has_value());
    if (result.rectList) {
        const auto& rect = *result.rectList;
        out.Enum(rect.mode);
        out.U32(rect.indexSize);
        out.U32(rect.inputControlPoints);
        out.U32(rect.outputControlPoints);
        WriteLayout(out, rect.inputLayout);
        WriteLayout(out, rect.outputLayout);
        out.U64(rect.buffers.size());
        for (const auto& buffer : rect.buffers) {
            out.Enum(buffer.role);
            out.U32(buffer.index);
            out.U32(buffer.elementBytes);
        }
    }
}

bool ReadResult(Reader& in, Result& result) {
    result.capturedSamplerRequirements.resize(in.Count(12));
    for (auto& item : result.capturedSamplerRequirements) {
        item.descriptorSet = in.U32();
        item.binding = in.U32();
        item.element = in.U32();
    }
    result.minimumLodImages.resize(in.Count(12));
    for (auto& item : result.minimumLodImages) {
        item.descriptorSet = in.U32();
        item.binding = in.U32();
        item.element = in.U32();
    }
    result.requiresTextureLodQueries = in.Bool();
    result.minimumLodPairs.resize(in.Count(36));
    for (auto& pair : result.minimumLodPairs) {
        for (auto* value : {&pair.imageSet, &pair.imageBinding, &pair.imageElement, &pair.samplerSet, &pair.samplerBinding, &pair.samplerElement, &pair.magArgument, &pair.minArgument})
            *value = in.U32();
        pair.relativeViewMin = in.F32();
    }
    result.stage = in.Enum<ShaderStage>();
    result.vertexBufferCount = in.U32();
    result.source = in.String();
    result.entryPoint = in.String();
    result.resources.resize(in.Count(56));
    for (auto& resource : result.resources) {
        resource.descriptorSet = in.U32();
        resource.binding = in.U32();
        resource.count = in.U32();
        resource.kind = in.Enum<DescriptorKind>();
        resource.role = in.Enum<DescriptorRole>();
        resource.buffer = in.Optional();
        resource.texture = in.Optional();
        resource.sampler = in.Optional();
        resource.active = in.Bool();
        resource.requiresByteLengths = in.Bool();
        resource.unsignedStorageImage = in.Bool();
    }
    result.pushConstantBuffer = in.Optional();
    result.pushConstantBytes = in.U32();
    result.pushConstantData.resize(in.Count(1));
    in.Raw(result.pushConstantData.data(), result.pushConstantData.size());
    result.bufferSizesBuffer = in.Optional();
    result.samplerArgumentBuffer = in.Optional();
    result.samplerArgumentCount = in.U32();
    for (auto& value : result.threadsPerThreadgroup) value = in.U32();
    result.capabilities.resize(in.Count(4));
    for (auto& value : result.capabilities) value = in.U32();
    result.extensions.resize(in.Count(8));
    for (auto& extension : result.extensions) extension = in.String();
    result.requiresWorkgroupAtomicFences = in.Bool();
    result.requiresGpuAddresses = in.Bool();
    result.requiresInt64 = in.Bool();
    result.requiresSimdGroups = in.Bool();
    result.mesh.reset();
    if (in.Bool()) {
        MeshOutputInfo mesh{};
        mesh.maxVertices = in.U32();
        mesh.maxPrimitives = in.U32();
        result.mesh = mesh;
    }
    result.nativeExecutionKind = in.Enum<NativeExecutionKind>();
    result.rectList.reset();
    if (in.Bool()) {
        RectListInfo rect{};
        rect.mode = in.Enum<RectListMode>();
        rect.indexSize = in.U32();
        rect.inputControlPoints = in.U32();
        rect.outputControlPoints = in.U32();
        ReadLayout(in, rect.inputLayout);
        ReadLayout(in, rect.outputLayout);
        rect.buffers.resize(in.Count(12));
        for (auto& buffer : rect.buffers) {
            buffer.role = in.Enum<ImplicitBufferRole>();
            buffer.index = in.U32();
            buffer.elementBytes = in.U32();
        }
        result.rectList = std::move(rect);
    }
    return in.ok;
}

std::span<const std::byte> AsBytes(const std::string& text) { return {reinterpret_cast<const std::byte*>(text.data()), text.size()}; }

// Keys run to hundreds of KiB (they hold the SPIR-V): entries keep three independent 64-bit hashes of
// the key instead, two of which name the file.
struct Digest {
    std::uint64_t words[3];
};

Digest KeyDigest(const std::string& key) {
    const auto bytes = AsBytes(key);
    return {{HashBytes(bytes, 0x9e3779b97f4a7c15ull), HashBytes(bytes, 0xc2b2ae3d27d4eb4full), HashBytes(bytes, 0x165667b19e3779f9ull)}};
}

const std::filesystem::path& Directory() {
    static const std::filesystem::path directory = [] {
        // ANYPS5_NO_MSL_CACHE turns off only this cache (for comparisons and troubleshooting).
        const auto root = ShaderCacheDisabledBy("ANYPS5_NO_MSL_CACHE") ? std::filesystem::path() : ShaderCacheDirectory();
        if (root.empty()) return root;
        auto current = root / ("msl-" ANYPS5_MSL_CACHE_VERSION);
        // Older MSL cache versions can never hit again; the cap then applies to the whole cache directory.
        CacheEviction::MarkInUse(current);
        CacheEviction::RemoveStaleVersions(root, current, [](const std::string& name) { return name.rfind("msl-", 0) == 0; });
        CacheEviction::SharedBudget().Startup(root);
        return current;
    }();
    return directory;
}

std::filesystem::path EntryPath(const Digest& digest) {
    char name[48];
    std::snprintf(name, sizeof name, "%016llx%016llx.msl", static_cast<unsigned long long>(digest.words[0]), static_cast<unsigned long long>(digest.words[1]));
    return Directory() / name;
}

std::vector<std::byte> Encode(const Digest& digest, const Result& result) {
    Writer payload;
    WriteResult(payload, result);
    // The MSL text is most of an entry and compresses several times over.
    std::vector<std::byte> packed(payload.bytes.size() + 4096);
    const auto packedSize = compression_encode_buffer(reinterpret_cast<std::uint8_t*>(packed.data()), packed.size(),
                                                      reinterpret_cast<const std::uint8_t*>(payload.bytes.data()), payload.bytes.size(), nullptr, COMPRESSION_LZ4);
    if (packedSize == 0) return {};
    Writer out;
    out.U32(kMagic);
    out.U32(kFormat);
    for (const auto word : digest.words) out.U64(word);
    out.U64(payload.bytes.size());
    out.U64(HashBytes(payload.bytes));
    out.U64(packedSize);
    out.Raw(packed.data(), packedSize);
    return std::move(out.bytes);
}

bool Decode(std::span<const std::byte> file, const Digest& digest, Result& result) {
    Reader in(file);
    if (in.U32() != kMagic || in.U32() != kFormat) return false;
    for (const auto word : digest.words)
        if (in.U64() != word) return false;
    const auto size = in.U64();
    const auto checksum = in.U64();
    const auto packedSize = in.U64();
    // Bound the inflated size: no entry comes near 256 MiB.
    if (!in.ok || packedSize != file.size() - in.Offset() || size == 0 || size > (std::uint64_t{256} << 20)) return false;
    std::vector<std::byte> payload(static_cast<std::size_t>(size));
    const auto unpacked = compression_decode_buffer(reinterpret_cast<std::uint8_t*>(payload.data()), payload.size(),
                                                    reinterpret_cast<const std::uint8_t*>(file.data() + in.Offset()), static_cast<std::size_t>(packedSize), nullptr, COMPRESSION_LZ4);
    if (unpacked != payload.size() || HashBytes(payload) != checksum) return false;
    Reader body(payload);
    Result decoded;
    if (!ReadResult(body, decoded) || !body.AtEnd()) return false;
    decoded.guest = std::move(result.guest);
    result = std::move(decoded);
    return true;
}

}

std::vector<std::byte> EncodeEntry(const std::string& key, const Result& result) { return Encode(KeyDigest(key), result); }

bool DecodeEntry(std::span<const std::byte> file, const std::string& key, Result& result) { return Decode(file, KeyDigest(key), result); }

bool Load(const std::string& key, Result& result) {
    if (Directory().empty()) return false;
    const auto digest = KeyDigest(key);
    std::vector<std::byte> file;
    const auto path = EntryPath(digest);
    const bool hit = ReadWholeFile(path, file) && Decode(file, digest, result);
    if (hit) CacheEviction::Touch(path);
    // ANYPS5_MSL_CACHE_STATS=1 logs the running hit and miss counts every 100 lookups.
    static const bool stats = std::getenv("ANYPS5_MSL_CACHE_STATS") != nullptr;
    if (stats) {
        static std::atomic<std::uint32_t> hits{0}, misses{0};
        const auto hitCount = hit ? hits.fetch_add(1) + 1 : hits.load();
        const auto missCount = hit ? misses.load() : misses.fetch_add(1) + 1;
        if ((hitCount + missCount) % 100 == 0) std::fprintf(stderr, "[msl-cache] %u hits, %u misses\n", hitCount, missCount);
    }
    return hit;
}

void Store(const std::string& key, const Result& result) {
    if (Directory().empty()) return;
    const auto digest = KeyDigest(key);
    const auto bytes = Encode(digest, result);
    if (!bytes.empty() && WriteFileAtomically(EntryPath(digest), bytes)) CacheEviction::SharedBudget().NoteWrite(Directory().parent_path(), bytes.size());
}

}
