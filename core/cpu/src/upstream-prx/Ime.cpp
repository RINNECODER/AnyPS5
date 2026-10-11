// Upstream libSceIme, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceIme/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged: sceImeUpdate takes an event callback (SceLeafSystemImports).
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceImeKeyboardOpen, I32, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceImeKeyboardClose, I32),
    ANYPS5_UPSTREAM_EXPORT(sceImeKeyboardGetInfo, I32, Opaque),
};

}

Library ImeExports() { return {"libSceIme", exports}; }

}
