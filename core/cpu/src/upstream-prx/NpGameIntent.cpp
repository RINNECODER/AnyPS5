// Upstream libSceNpGameIntent, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpGameIntent/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
template <> struct GuestLayout<NpGameIntentInfo> { static constexpr std::size_t Size = sizeof(NpGameIntentInfo); };

namespace {

// NpGameIntentInfo holds integers and byte arrays only (size_t is 8 bytes on both ABIs).
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpGameIntentInitialize, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGameIntentTerminate),
    ANYPS5_UPSTREAM_EXPORT(sceNpGameIntentReceiveIntent, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpGameIntentGetPropertyValueString, Opaque, Opaque, OutArray<3>, I64),
};

}

Library NpGameIntentExports() { return {"libSceNpGameIntent", exports}; }

}
