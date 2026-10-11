// Upstream libSceNpAuth, compiled for the macOS runner, and the exports a guest may
// call through the bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpAuth/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged: sceNpAuthCreateRequest, sceNpAuthGetIdTokenV3 and sceNpAuthWaitAsync are
// NotImplemented upstream, so a guest call reaches the unresolved-import trap stub.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpAuthAbortRequest, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpAuthCreateAsyncRequest, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpAuthDeleteRequest, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpAuthGetAuthorizationCodeV3, I32, Opaque, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpAuthPollAsync, I32, Out),
};

}

Library NpAuthExports() { return {"libSceNpAuth", exports}; }

}
