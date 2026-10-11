// Upstream libSceNpUniversalDataSystem, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpUniversalDataSystem/src/System.cpp"
#include "prx/libSceNpUniversalDataSystem/src/Handle.cpp"
#include "prx/libSceNpUniversalDataSystem/src/Context.cpp"
#include "prx/libSceNpUniversalDataSystem/src/Event.cpp"
#include "prx/libSceNpUniversalDataSystem/src/EventPropertyObject.cpp"
#include "prx/libSceNpUniversalDataSystem/src/EventPropertyArray.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
namespace {

// Not bridged: CreateEvent, DestroyEvent and the Set*Array/Object exports allocate or return host
// objects, and SetFloat32 takes a float (SceLeafSystemImports hands out opaque tokens instead).
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemInitialize, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemCreateHandle, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemDestroyHandle, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemCreateContext, Out, I32, I32, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemDestroyContext, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemRegisterContext, I32, I32, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemPostEvent, I32, I32, Opaque, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemEventPropertyObjectSetString, Opaque, Opaque, Opaque),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemEventPropertyObjectSetInt32, Opaque, Opaque, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemEventPropertyObjectSetUInt32, Opaque, Opaque, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpUniversalDataSystemEventPropertyArraySetString, Opaque, Opaque),
};

}

Library NpUniversalDataSystemExports() { return {"libSceNpUniversalDataSystem", exports}; }

}
