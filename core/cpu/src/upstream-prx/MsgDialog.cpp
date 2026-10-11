// Upstream libSceMsgDialog, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceMsgDialog.native/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceMsgDialogInitialize),
    ANYPS5_UPSTREAM_EXPORT(sceMsgDialogOpen, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceMsgDialogTerminate),
    ANYPS5_UPSTREAM_EXPORT(sceMsgDialogUpdateStatus),
};

}

Library MsgDialogExports() { return {"libSceMsgDialog", exports, "libSceMsgDialog.native", "libSceMsgDialog.native.prx", 1, 1}; }

}
