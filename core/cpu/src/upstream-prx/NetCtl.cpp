// Upstream libSceNetCtl, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNetCtl/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNetCtlGetInfo, I32, Opaque),
};

}

Library NetCtlExports() { return {"libSceNetCtl", exports}; }

}
