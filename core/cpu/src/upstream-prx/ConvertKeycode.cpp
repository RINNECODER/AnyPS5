// Upstream libSceConvertKeycode, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libc/include/General.hpp"
#include "HitLog.hpp"
// The guest owns stdout and the runner owns the machine-readable stderr, so the upstream trace
// macros are silenced for this export table; no status code depends on them.
#undef APS5_LOG_OUT
#define APS5_LOG_OUT(...) ((void)0)
#undef APS5_HIT
#define APS5_HIT(...) ((void)0)
#include "prx/libSceConvertKeycode/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceConvertKeycodeGetVirtualKeycode, I64, I64, I64, I64),
};

}

Library ConvertKeycodeExports() { return {"libSceConvertKeycode", exports, "", "", 1, 0}; }

}
