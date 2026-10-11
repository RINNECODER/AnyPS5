// Upstream libSceSystemGesture, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceSystemGesture/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged here: AppendTouchRecognizer, RemoveTouchRecognizer and UpdateAllTouchRecognizer are
// NotImplemented upstream (they throw); SceLeafSystemImports answers them with success because no touch
// input exists.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureInitializePrimitiveTouchRecognizer, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureFinalizePrimitiveTouchRecognizer),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureOpen, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureCreateTouchRecognizer, I32, Opaque, I32, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureClose, I32),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureUpdatePrimitiveTouchRecognizer, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureGetTouchEventsCount, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceSystemGestureGetTouchEventByIndex, I32, Opaque, I32, Opaque),
};

}

Library SystemGestureExports() { return {"libSceSystemGesture", exports}; }

}
