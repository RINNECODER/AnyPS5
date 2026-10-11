// Upstream libSceNpCommerce, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpCommerce/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// sceNpCommerceDialogGetResult writes an int32 result and a one-byte authorized flag.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpCommerceDialogInitialize),
    ANYPS5_UPSTREAM_EXPORT(sceNpCommerceDialogOpen, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpCommerceDialogUpdateStatus),
    ANYPS5_UPSTREAM_EXPORT(sceNpCommerceDialogTerminate),
    ANYPS5_UPSTREAM_EXPORT(sceNpCommerceDialogGetResult, OutBytes<5>),
};

}

Library NpCommerceExports() { return {"libSceNpCommerce", exports}; }

}
