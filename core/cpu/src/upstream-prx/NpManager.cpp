// Upstream libSceNpManager, compiled for the macOS runner, and the exports a guest may
// call through the bridge. Every row was checked against the upstream body; the drift
// test (tests/UpstreamPrxBridgeDriftTest.py) fails when upstream changes one.
#include "prx/libSceNpManager/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged, so SceNpOfflineImports keeps them hand-written:
//   callback registration: sceNpRegisterGamePresenceCallback, sceNpRegisterNpReachabilityStateCallback,
//     sceNpRegisterPlusEventCallback, sceNpRegisterPremiumEventCallback, sceNpRegisterStateCallback,
//     sceNpRegisterStateCallbackA, and sceNpUnregisterNpReachabilityStateCallback, which shares the
//     registration state with its register call
//   NotImplemented upstream: sceNpSetContentRestriction, sceNpGetUserIdByAccountId
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpAbortRequest, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpCheckCallback),
    ANYPS5_UPSTREAM_EXPORT(sceNpCheckNpAvailability, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpCheckNpReachability, I32, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpCheckPremium, I32, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpCreateAsyncRequest, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpCreateRequest),
    ANYPS5_UPSTREAM_EXPORT(sceNpDeleteRequest, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetAccountAge, I32, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetAccountCountryA, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetAccountIdA, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetAccountLanguage2, I32, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetNpId, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetNpReachabilityState, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetOnlineId, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpGetState, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpHasSignedUp, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpNotifyPremiumFeature, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpPollAsync, I32, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpSetNpTitleId, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpSetTimeout, I32, I32, I32, I32, I32, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpUnregisterPremiumEventCallback),
    ANYPS5_UPSTREAM_EXPORT(sceNpUnregisterStateCallback),
    ANYPS5_UPSTREAM_EXPORT(sceNpUnregisterStateCallbackA, I32),
};

}

Library NpManagerExports() { return {"libSceNpManager", exports}; }

}
