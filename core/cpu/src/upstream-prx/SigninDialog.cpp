// Upstream libSceSigninDialog, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceSigninDialog/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceSigninDialogInitialize),
    ANYPS5_UPSTREAM_EXPORT(sceSigninDialogOpen, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceSigninDialogTerminate),
    ANYPS5_UPSTREAM_EXPORT(sceSigninDialogUpdateStatus),
};

}

Library SigninDialogExports() { return {"libSceSigninDialog", exports}; }

}
