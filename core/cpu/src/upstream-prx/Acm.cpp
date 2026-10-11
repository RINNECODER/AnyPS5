// Upstream libSceAcm, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceAcm/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Batches execute as no-ops upstream; the command buffers are never read, so they are not copied.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceAcmContextCreate, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAcmBatchStartBuffers, I32, I32, Ignored, Ignored, Out),
    ANYPS5_UPSTREAM_EXPORT(sceAcmBatchWait, I32, I32, I32),
};

}

Library AcmExports() { return {"libSceAcm", exports}; }

}
