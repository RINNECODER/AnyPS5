// Upstream libSceRtc, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceRtc/Export.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
template <> struct GuestLayout<RtcTick> { static constexpr std::size_t Size = sizeof(RtcTick); };

namespace {

// RFC 3339 text is at most 32 bytes; the formatter is run on a host buffer and only the string is
// written back.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceRtcFormatRFC3339, OutString<40>, In, I32),
    ANYPS5_UPSTREAM_EXPORT(sceRtcParseRFC3339, Out, InString<64>),
};

}

Library RtcExports() { return {"libSceRtc", exports}; }

}
