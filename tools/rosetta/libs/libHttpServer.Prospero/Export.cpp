// Stub for libHttpServer.Prospero, which has no source in core/libs/prx. A title
// that lists the module as a dependency can load; the server itself is not
// provided, so the module exports no working entry points yet. Add each export
// the titles import here as a function returning an SCE "not supported" error.
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

extern "C" {

APS5_DUMMY_FUN

}
