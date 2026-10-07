#pragma once

#include <cpu/Cpu.hpp>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Cpu {

struct SceImport;

struct SceVideoOutOpenParam {
    std::uint32_t FirstWord = 0;
    std::uint32_t SetPriority = 0;
    std::int32_t Priority = 0;
    std::uint32_t SetAffinity = 0;
    std::uint64_t Affinity = 0;
};

struct SceVideoOutAttribute {
    std::uint32_t Reserved0 = 0;
    std::uint32_t TilingMode = 0;
    std::uint32_t AspectRatio = 0;
    std::uint32_t Width = 0;
    std::uint32_t Height = 0;
    std::uint32_t PitchInPixel = 0;
    std::uint64_t Option = 0;
    std::uint64_t PixelFormat = 0;
    std::uint64_t DccClearColor = 0;
    std::uint32_t DccControl = 0;
    std::uint32_t Pad0 = 0;
    std::array<std::uint64_t, 3> Reserved1{};
};

struct SceVideoOutBuffer {
    std::uint64_t DataAddress = 0;
    std::uint64_t MetadataAddress = 0;
    std::array<std::uint64_t, 2> Reserved{};
};

struct SceVideoOutStatus {
    std::uint32_t Resolution = 0;
    std::uint32_t DynamicRange = 0;
    std::uint64_t RefreshRate = 0;
    std::uint64_t Flags = 0;
    std::array<std::uint64_t, 3> Reserved{};
};

struct SceVideoOutStatusResult {
    std::int32_t Result;
    SceVideoOutStatus Status;
};

struct SceVideoOutBackend {
    std::function<std::int32_t(std::int32_t, std::int32_t, std::int32_t,
                              const std::optional<SceVideoOutOpenParam>&)> Open;
    std::function<std::int32_t(std::int32_t)> Close;
    std::function<SceVideoOutStatusResult(std::int32_t)> GetOutputStatus;
    std::function<std::int32_t(std::int32_t, std::int32_t, std::int32_t,
                              std::span<const SceVideoOutBuffer>, const SceVideoOutAttribute&,
                              std::int32_t)> RegisterBuffers;
    std::function<std::int32_t(std::int32_t, std::int32_t)> SetFlipRate;
    std::function<std::int32_t(std::int32_t, std::int32_t)> UnregisterBuffers;
};

// A name or matching NID is not a target ABI qualification. The default is
// empty; callers verify the source image before selecting this finite profile.
enum class VideoOutAbiContract { Open, Close, OutputStatus, RegisterBuffers, SetAttribute, FlipRate, Unregister };
struct VideoOutAbiAdmission {
    VideoOutAbiContract Contract;
    std::string_view Evidence;
};
std::span<const VideoOutAbiAdmission> QualifiedVideoOutAdmissionsForImage(std::string_view verifiedSha256);

class SceVideoOutImports {
public:
    explicit SceVideoOutImports(Machine& machine, SceVideoOutBackend backend = {},
                               std::uint64_t gateBase = 0x7ffdfd000000,
                               std::span<const VideoOutAbiAdmission> admissions = {});
    ~SceVideoOutImports();
    SceVideoOutImports(const SceVideoOutImports&) = delete;
    SceVideoOutImports& operator=(const SceVideoOutImports&) = delete;
    // Production always supplies the observed ELF metadata; no FUNC default.
    std::uint64_t Resolve(const SceImport& import, std::uint8_t symbolType, std::uint64_t symbolSize);
    // Public candidate ABI only. Synthetic callers use this separate route;
    // it never qualifies or activates a target consumer.
    std::uint64_t ResolvePublicFixture(const SceImport& import);
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
