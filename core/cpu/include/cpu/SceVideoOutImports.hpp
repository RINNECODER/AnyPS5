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

// Guest ABI of sceVideoOutGetFlipStatus (128 bytes, the PS5 layout).
struct SceVideoOutFlipStatus {
    std::uint64_t Count = 0;
    std::uint64_t ProcessTime = 0;
    std::uint64_t Reserved0 = 0;
    std::int64_t FlipArg = 0;
    std::uint64_t Reserved1 = 0;
    std::uint64_t ProcessTimeCounter = 0;
    std::int32_t GcQueueNum = 0;
    std::int32_t FlipPendingNum = 0;
    std::int32_t CurrentBuffer = 0;
    std::uint32_t Reserved2 = 0;
    std::uint64_t SubmitProcessTimeCounter = 0;
    std::array<std::uint64_t, 7> Reserved3{};
};

// Guest ABI of sceVideoOutGetVblankStatus (40 bytes).
struct SceVideoOutVblankStatus {
    std::uint64_t Count = 0;
    std::uint64_t ProcessTime = 0;
    std::uint64_t Reserved = 0;
    std::uint64_t ProcessTimeCounter = 0;
    std::uint8_t Flags = 0;
    std::uint8_t Phase = 0;
    std::array<std::uint8_t, 6> Pad{};
};

// Guest ABI of sceVideoOutGetResolutionStatus. Only the first 44 bytes are
// defined fields; the trailing alignment padding is never written.
struct SceVideoOutResolutionStatus {
    std::uint32_t FullWidth = 0;
    std::uint32_t FullHeight = 0;
    std::uint32_t PaneWidth = 0;
    std::uint32_t PaneHeight = 0;
    std::uint64_t RefreshRate = 0;
    float ScreenSizeInInch = 0;
    std::uint16_t Flags = 0;
    std::uint16_t Reserved0 = 0;
    std::array<std::uint32_t, 3> Reserved1{};
};

struct SceVideoOutFlipStatusResult { std::int32_t Result; SceVideoOutFlipStatus Status; };
struct SceVideoOutVblankStatusResult { std::int32_t Result; SceVideoOutVblankStatus Status; };
struct SceVideoOutResolutionStatusResult { std::int32_t Result; SceVideoOutResolutionStatus Status; };

// SCE VideoOut result codes returned to the guest for invalid arguments.
namespace VideoOutError {
inline constexpr std::int32_t InvalidValue = static_cast<std::int32_t>(0x80290001u);
inline constexpr std::int32_t InvalidAddress = static_cast<std::int32_t>(0x80290002u);
inline constexpr std::int32_t InvalidPixelFormat = static_cast<std::int32_t>(0x80290003u);
inline constexpr std::int32_t ResourceBusy = static_cast<std::int32_t>(0x80290009u);
inline constexpr std::int32_t InvalidIndex = static_cast<std::int32_t>(0x8029000au);
inline constexpr std::int32_t InvalidHandle = static_cast<std::int32_t>(0x8029000bu);
inline constexpr std::int32_t InvalidEventQueue = static_cast<std::int32_t>(0x8029000cu);
inline constexpr std::int32_t InvalidEvent = static_cast<std::int32_t>(0x8029000du);
inline constexpr std::int32_t NoEmptySlot = static_cast<std::int32_t>(0x8029000fu);
inline constexpr std::int32_t SlotOccupied = static_cast<std::int32_t>(0x80290010u);
inline constexpr std::int32_t FlipQueueFull = static_cast<std::int32_t>(0x80290012u);
inline constexpr std::int32_t UnavailableOutputMode = static_cast<std::int32_t>(0x80290019u);
inline constexpr std::int32_t InvalidOption = static_cast<std::int32_t>(0x8029001au);
inline constexpr std::int32_t InvalidCategory = static_cast<std::int32_t>(0x8029001du);
}

// VideoOut equeue event kinds (the event ident under EVFILT_VIDEO_OUT, -13).
inline constexpr std::int32_t VideoOutEventFlip = 0;
inline constexpr std::int32_t VideoOutEventVblank = 1;

// Kernel equeue delivery for VideoOut events. Add/Delete run on the guest
// owner thread inside a host call and return an SCE result; Publish is the
// thread-safe producer used by the presentation and vblank threads.
struct SceVideoOutEventSink {
    std::function<std::int32_t(std::uint64_t, std::int32_t, std::int32_t, std::uint64_t)> Add;
    std::function<std::int32_t(std::uint64_t, std::int32_t, std::int32_t)> Delete;
    std::function<void(std::int32_t, std::int32_t, std::int64_t)> Publish;
    // Parks the calling guest thread until the output's next vblank; without
    // it the backend blocks the host call on its own vblank condition.
    std::function<std::int32_t(std::int32_t)> WaitVblank;
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
    // handle, index (-2..15), flip mode, flip argument.
    std::function<std::int32_t(std::int32_t, std::int32_t, std::int32_t, std::int64_t)> SubmitFlip;
    std::function<SceVideoOutFlipStatusResult(std::int32_t)> GetFlipStatus;
    std::function<SceVideoOutVblankStatusResult(std::int32_t)> GetVblankStatus;
    std::function<SceVideoOutResolutionStatusResult(std::int32_t)> GetResolutionStatus;
    std::function<std::int32_t(std::int32_t)> WaitVblank;
    std::function<std::int32_t(std::int32_t)> IsFlipPending;
    std::function<std::int32_t(std::int32_t, std::uint64_t)> ConfigureOutput;
    std::function<std::int32_t(std::int32_t, std::int32_t, std::int32_t)> SetWindowModeMargins;
    // equeue, handle, event kind, udata (Add) -- see SceVideoOutEventSink.
    std::function<std::int32_t(std::uint64_t, std::int32_t, std::int32_t, std::uint64_t)> AddEvent;
    std::function<std::int32_t(std::uint64_t, std::int32_t, std::int32_t)> DeleteEvent;
};

// A name or matching NID is not a target ABI qualification. The default is
// empty; the native runner selects this finite profile for every importing image.
enum class VideoOutAbiContract {
    Open, Close, OutputStatus, RegisterBuffers, SetAttribute, FlipRate, Unregister,
    SubmitFlip, AddFlipEvent, DeleteFlipEvent, AddVblankEvent, DeleteVblankEvent,
    GetFlipStatus, GetVblankStatus, WaitVblank, IsFlipPending, GetResolutionStatus,
    ConfigureOutput, SetWindowModeMargins, RegisterBuffersV1, GetEventId, GetEventData, GetEventCount
};
struct VideoOutAbiAdmission {
    VideoOutAbiContract Contract;
    std::string_view Evidence;
};
std::span<const VideoOutAbiAdmission> TargetVideoOutAdmissions();

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
