#pragma once

#include "SceTypes.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgcDriver/Execution/include/NativeGuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Presentation.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AgcDriver::Metal {

struct ReadableGuestRange {
    std::uint64_t address;
    std::size_t bytes;
};

// Draw and dispatch packets the driver skipped instead of failing the session:
// unsupported state, shader translation and pipeline failures before the packet's
// GPU work is committed. Device loss, command-buffer errors, GPU faults and guest
// writes into read-only memory stay sticky and are not counted here.
struct SkippedWorkDiagnostics {
    struct Reason {
        std::string kind;  // "draw" or "dispatch"
        std::string what;  // first line of the error, truncated
        std::uint64_t count = 0;
    };
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    // The first MaxReasons distinct reasons in the order they were first seen.
    std::vector<Reason> reasons;
    // Skips whose reason arrived after MaxReasons distinct reasons were recorded.
    std::uint64_t unlistedReasons = 0;
    static constexpr std::size_t MaxReasons = 64;
};

class MetalDriver {
public:
    using EopInterrupt = std::function<void(std::uint32_t)>;
    MetalDriver();
    ~MetalDriver();
    MetalDriver(const MetalDriver&) = delete;
    MetalDriver& operator=(const MetalDriver&) = delete;
    static MetalDriver& Get();
    void Configure(void* nativeDevice, void* nativeLibrary,
        std::span<const NativeGuestMemory::BorrowedRange> ranges, EopInterrupt eopInterrupt = {},
        std::uint64_t initialGeneration = 0);
    void ReplaceBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation);
    void MutateBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation,
        std::function<void()> mutateCpu, std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner);
    void WithValidatedReadableRanges(std::span<const ReadableGuestRange> ranges, const std::function<void()>& publish);
    void Submit(const Packet* packet, std::uint32_t queue);
    void SubmitCommandBuffer(std::uint64_t commandAddress, std::uint32_t wordCount, std::uint8_t flags, std::uint32_t queue);
    void WaitIdle();
    void Shutdown();
    void SuspendPoint();
    void RegisterShader(const Shader* shader);
    // Publish a staged guest header and capture its immutable registry entry
    // under one mapping admission. Callbacks may access guest memory but must
    // not reenter the driver; a failed capture restores the header before the
    // admission is released. Neither callback may change guest mappings.
    void RegisterShaderWithPublication(std::uint64_t guestHeaderAddress,
        std::span<const ReadableGuestRange> ranges, const std::function<void()>& publish,
        const std::function<void()>& rollback);
    void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output);
    void Present(const PresentationWindow& window, const DisplayBuffer* buffer, bool opaque,
                 void (*gpuReady)(void*), void* context);
    void ReleaseWindow(void* window);
    void ReportFailure(std::exception_ptr error);
    [[nodiscard]] SkippedWorkDiagnostics SkippedWork() const;

private:
    struct CommandBufferSubmission {
        std::uint64_t address;
        std::uint32_t words;
        std::uint8_t flags;
    };
    void submit(CommandBufferSubmission descriptor, std::uint32_t queue, std::optional<std::uint64_t> guestPacketAddress);
    void replaceBorrowedRanges(std::span<const NativeGuestMemory::BorrowedRange> ranges, std::uint64_t generation,
        const std::function<void()>& mutateCpu, std::shared_ptr<const void> previousOwner, std::shared_ptr<const void> nextOwner);
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
