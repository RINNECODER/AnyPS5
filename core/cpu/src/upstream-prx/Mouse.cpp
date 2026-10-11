// Upstream libSceMouse, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceMouse/Export.cpp"
#include "prx/libSceMouse/src/mouse_impl.cpp"
#include "Marshal.hpp"

#include <chrono>

// mouse_impl.cpp stamps samples with libkernel's process time. libkernel is not linked into this
// host library (the guest's own libkernel provider serves the title), so stamp with the host
// monotonic clock in microseconds since first use instead.
extern "C" std::uint64_t sceKernelGetProcessTime() {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

namespace Cpu::UpstreamPrx {
template <> struct GuestLayout<MouseData> { static constexpr std::size_t Size = sizeof(MouseData); };

namespace {

// sceMouseOpen reads one MouseOpenParam (8 bytes); MouseData (40 bytes) is a plain integer struct.
// Both sizes are asserted by upstream mouse_structs.h.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceMouseInit),
    ANYPS5_UPSTREAM_EXPORT(sceMouseOpen, I32, I32, I32, InBytes<8>),
    ANYPS5_UPSTREAM_EXPORT(sceMouseRead, I32, OutArray<2>, I32),
};

}

Library MouseExports() { return {"libSceMouse", exports}; }

}
