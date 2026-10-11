// Upstream libSceNpWebApi2, compiled for the macOS runner, and the exports a guest may
// call through the bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpWebApi2/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged, so SceNpOfflineImports keeps them hand-written:
//   callback registration: sceNpWebApi2PushEventRegisterCallback and
//     sceNpWebApi2PushEventRegisterPushContextCallback (upstream declares it without parameters)
//   NotImplemented upstream: sceNpWebApi2SetRequestTimeout
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2AbortRequest, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2AddHttpRequestHeader, I64, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2CheckTimeout),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2CreateRequest, I32, Opaque, Opaque, Opaque, Opaque, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2CreateUserContext, I32, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2DeleteRequest, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2DeleteUserContext, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2GetHttpResponseHeaderValue, I64, Opaque, Opaque, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2GetHttpResponseHeaderValueLength, I64, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2Initialize, I32, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventCreateFilter, I32, I32, Opaque, I32, Opaque, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventCreateHandle, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventCreatePushContext),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventDeleteFilter),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventDeleteHandle, I32, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventDeletePushContext, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventStartPushContextCallback),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventUnregisterCallback),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2PushEventUnregisterPushContextCallback),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2ReadData, I64, Opaque, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2SendRequest, I64, Opaque, I64, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpWebApi2Terminate, I32),
};

}

Library NpWebApi2Exports() { return {"libSceNpWebApi2", exports}; }

}
