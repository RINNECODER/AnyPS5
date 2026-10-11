#pragma once

#import <Metal/Metal.h>
#include <cstdint>
#include <optional>

namespace AgcDriver::Metal {

// Opt-in diagnostics: captures the final composited image presented to the engine's own
// game window (the drawable texture), never the display or any other window.
// Off unless ANYPS5_FRAME_CAPTURE_DIR is set. ANYPS5_FRAME_CAPTURE_INTERVAL_MS (default
// 30000) spaces captures and the periodic frames_presented event; the first presented
// frame is always captured. Events are JSON lines on stderr.
class FrameCapture {
public:
    struct Ticket {
        std::uint64_t flipIndex;
        std::uint32_t guestWidth;
        std::uint32_t guestHeight;
    };
    static FrameCapture& Shared();
    [[nodiscard]] bool Enabled() const { return enabled; }
    // Counts one presented flip; returns a ticket when this flip should be read back.
    // Skips (returns nullopt) while an earlier capture is still being written.
    [[nodiscard]] std::optional<Ticket> Begin(std::uint32_t guestWidth, std::uint32_t guestHeight);
    // Encodes the readback of the window texture into `commands` (before commit) and
    // finishes asynchronously from the completion handler. Never waits.
    void Encode(id<MTLCommandBuffer> commands, id<MTLTexture> windowTexture, const Ticket& ticket);
    // Returns the in-flight slot when a ticket is dropped without Encode.
    void Abandon();

private:
    FrameCapture();
    struct State;
    bool enabled = false;
    State* state = nullptr;
};

}
