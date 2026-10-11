// Upstream libSceSysmodule, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libc/include/General.hpp"
#include "HitLog.hpp"
// The guest owns stdout and the runner owns the machine-readable stderr, so the upstream trace
// macros are silenced for this export table; no status code depends on them.
#undef APS5_LOG_OUT
#define APS5_LOG_OUT(...) ((void)0)
#undef APS5_HIT
#define APS5_HIT(...) ((void)0)
#include "prx/libSceSysmodule/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged: sceSysmoduleGetModuleInfoForUnwind reads /proc/self/maps of the host process; the
// *Internal exports are not imported by the target title.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceSysmoduleLoadModule, I32),
    ANYPS5_UPSTREAM_EXPORT(sceSysmoduleUnloadModule, I32),
};

}

Library SysmoduleExports() { return {"libSceSysmodule", exports}; }

}
