#ifndef CORE_SHADER_RECOMPILER_SHADERDISKCACHE_HPP
#define CORE_SHADER_RECOMPILER_SHADERDISKCACHE_HPP

#include "CompiledVariant.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourcePlan.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ShaderRecompiler::ShaderDiskCache {

// 29: upstream format 27 plus the native pixel-sampler certificate fields (fork format 26).
// 30: fork format 29 plus upstream format 28 (the pixel ancillary word reaches the request).
// 32: fork format 31 plus upstream format 29 (layerExported in the pixel info) and the sync-8 shader output changes.
// 33: fork format 32 plus the shader output changes of upstream float and denormal peepholes (U1).
// 34: fork format 33 plus upstream PR 4883 (stored front-end resource plans).
// 35: fork format 34 plus the shader output changes of upstream PRs 5110-5498 and 907 (U6: shader semantics).
// 36: fork format 35 plus the image-mode ImageResource fields of upstream PRs 5375, 5461 and 5509 and
//     BufferResource::formattedWritten of upstream PR 5172 (U8a: Metal image-mode views).
// 37: fork format 36 plus upstream PR 5500 (layerBase in the pixel info and the request key; U7).
inline constexpr std::uint32_t FormatVersion = 37;

enum class LoadStatus {
    Loaded,
    Absent,
    KeyMismatch,
    Rejected,
};

struct Counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t writes = 0;
    std::uint64_t loadFailures = 0;
    std::uint64_t writeFailures = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t planHits = 0;
    std::uint64_t planMisses = 0;
    std::uint64_t planWrites = 0;
    std::uint64_t planLoadFailures = 0;
    std::uint64_t prebuiltHits = 0;
    std::uint64_t prebuiltMisses = 0;
    std::uint64_t prebuiltRejected = 0;
    std::uint64_t lookupNanoseconds = 0;
    std::uint64_t readNanoseconds = 0;
    std::uint64_t decodeNanoseconds = 0;
    std::uint64_t loadNanoseconds = 0;
    std::uint64_t maxLoadNanoseconds = 0;
    std::uint64_t prebuiltBytesRead = 0;
    std::uint64_t prebuiltLoadNanoseconds = 0;
};

[[nodiscard]] std::uint64_t SourceVersion();

void BuildKey(const RecompileRequest& request, std::uint32_t hostSubgroupSize, std::vector<std::byte>& key);

[[nodiscard]] std::string EntryName(std::span<const std::byte> key);

void EncodeResult(const RecompileResult& result, std::vector<std::byte>& out);
[[nodiscard]] bool DecodeResult(std::span<const std::byte> bytes, RecompileResult& result);

[[nodiscard]] std::vector<std::byte> EncodeEntry(std::span<const std::byte> key, const CompiledVariant& variant);
[[nodiscard]] LoadStatus DecodeEntry(std::span<const std::byte> file, std::span<const std::byte> key, CompiledVariant& variant);

void BuildPlanKey(const RecompileRequest& request, std::uint32_t hostSubgroupSize, std::vector<std::byte>& key);
[[nodiscard]] bool EncodePlan(const IrResourcePlan& plan, std::vector<std::byte>& out);
[[nodiscard]] bool DecodePlan(std::span<const std::byte> bytes, IrResourcePlan& plan);
[[nodiscard]] std::vector<std::byte> EncodePlanEntry(std::span<const std::byte> key, const IrResourcePlan& plan);
[[nodiscard]] LoadStatus DecodePlanEntry(std::span<const std::byte> file, std::span<const std::byte> key, IrResourcePlan& plan);

[[nodiscard]] bool Enabled();
[[nodiscard]] std::filesystem::path EntryDirectory();

[[nodiscard]] bool Load(std::span<const std::byte> key, CompiledVariant& variant);
void Store(std::vector<std::byte> key, std::shared_ptr<const CompiledVariant> variant);
[[nodiscard]] bool LoadPlan(std::span<const std::byte> key, IrResourcePlan& plan);
void StorePlan(std::vector<std::byte> key, std::shared_ptr<const IrResourcePlan> plan);
void Flush();
[[nodiscard]] Counters Totals();

}

#endif
