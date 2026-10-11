// Upstream libSceNpTrophy2, compiled for the macOS runner, and the exports a guest may call through the
// bridge. Checked against the upstream bodies; see NpManager.cpp.
#include "prx/libSceNpTrophy2/src/Context.cpp"
#include "prx/libSceNpTrophy2/src/Handle.cpp"
#include "prx/libSceNpTrophy2/src/GameInfo.cpp"
#include "prx/libSceNpTrophy2/src/TrophyInfo.cpp"
#include "Marshal.hpp"

namespace Cpu::UpstreamPrx {
template <> struct GuestLayout<NpTrophy2GameDetails> { static constexpr std::size_t Size = sizeof(NpTrophy2GameDetails); };
template <> struct GuestLayout<NpTrophy2GameData> { static constexpr std::size_t Size = sizeof(NpTrophy2GameData); };
template <> struct GuestLayout<NpTrophy2Details> { static constexpr std::size_t Size = sizeof(NpTrophy2Details); };
template <> struct GuestLayout<NpTrophy2Data> { static constexpr std::size_t Size = sizeof(NpTrophy2Data); };

namespace {

// The four detail structs hold integers, bools and char arrays only.
constexpr Export exports[]{
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2CreateContext, Out, I32, I32, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2DestroyContext, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2RegisterContext, I32, I32, I64),
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2CreateHandle, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2DestroyHandle, I32),
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2GetTrophyInfo, I32, I32, I32, Out, Out),
    ANYPS5_UPSTREAM_EXPORT(sceNpTrophy2GetGameInfo, I32, I32, Out, Out),
};

}

Library NpTrophy2Exports() { return {"libSceNpTrophy2", exports}; }

}
