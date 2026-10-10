#pragma once

#include <cpu/ScePadImports.hpp>
#include "prx/libSceVideoOut/include/ControllerSource.hpp"
#include "prx/libSceVideoOut/include/NativeHostWindow.hpp"
#include <cstdint>

namespace Cpu {

// Feeds one pump's drained window and controller batches into the virtual pad. Every controller,
// key, button and wheel event commits its own sample so a tap inside one pump still reaches
// scePadRead. After the window queue dropped events, held keys and mouse buttons resynchronise
// from the window snapshot.
class NativePadInput {
public:
    void Apply(const AnyPS5::Host::EventBatch& window, const AnyPS5::Host::ControllerEventBatch* controller,
               PadHostInput& pad);
private:
    std::uint64_t windowDropped = 0;
};

PadControllerSample PadSampleFromController(const AnyPS5::Host::ControllerState& state);

}
