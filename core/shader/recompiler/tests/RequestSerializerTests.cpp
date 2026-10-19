#include "ControlFlow/RequestSerializer.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace {

constexpr char kHistoricalVersion21[] =
    "NVNQQRUAAAAFAAADAAAAAAABAAAAAAAAAAAAgb8AAAAAAAAAAAAAAAAAAAAAQAAAAAgAAAADAAAAAAAAAN+bVxPgrGgk776t3gABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAJAAAAAAAAAOQAAAAAAAAAAAAAAAAAAAAAAQEAAAEAAAAAAAAAAAAFAAAAAAAEAAAAAAAAABI0VngAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAALAAAAEAAAAEAAAAAAAAF4VjQSAQEBOTAAAAA=";
constexpr char kVersion21WithF16Tail[] =
    "NVNQQRUAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAQA0DAAEB";
constexpr char kVersion22WithoutF16Tail[] =
    "NVNQQRYAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAQA0DAAE=";

void Require(bool condition) {
    if (!condition) std::abort();
}
}

int main() {
    const ShaderRecompiler::RequestSerializer serializer;

    const auto historical = serializer.Deserialize(kHistoricalVersion21);
    Require(!historical.request.target.float16DenormPreserve);

    const auto version21WithTail = serializer.Deserialize(kVersion21WithF16Tail);
    Require(version21WithTail.request.target.float16DenormPreserve);

    bool rejectedTruncatedVersion22 = false;
    try {
        static_cast<void>(serializer.Deserialize(kVersion22WithoutF16Tail));
    } catch (const std::runtime_error& error) {
        rejectedTruncatedVersion22 = std::string_view(error.what()) == "RequestSerializer: truncated data";
    }
    Require(rejectedTruncatedVersion22);

    ShaderRecompiler::RecompileRequest request{};
    request.target.float16DenormPreserve = false;
    const auto withoutPreserve = serializer.Deserialize(serializer.Serialize(request));
    Require(!withoutPreserve.request.target.float16DenormPreserve);

    request.target.float16DenormPreserve = true;
    const auto withPreserve = serializer.Deserialize(serializer.Serialize(request));
    Require(withPreserve.request.target.float16DenormPreserve);

    request.context.pixel = ShaderPixelStageInfo{};
    request.context.pixel->reversedBlend = 0x55417u;
    const auto withReversedBlend = serializer.Deserialize(serializer.Serialize(request));
    Require(withReversedBlend.request.context.pixel.has_value());
    Require(withReversedBlend.request.context.pixel->reversedBlend == 0x55417u);

    for (const bool spanned : {false, true}) {
        for (const std::uint8_t version : {32u, 33u, 34u}) {
            pointRequest.target.spannedWaves = spanned;
            auto legacy = DecodeBase64(serializer.Serialize(pointRequest));
            legacy.resize(legacy.size() - 13u);
            legacy[4] = version;
            const auto replayLegacy = serializer.Deserialize(EncodeBase64(legacy));
            Require(replayLegacy.request.target.spannedWaves == spanned);
            Require(replayLegacy.request.context.vertex.has_value());
            Require(replayLegacy.request.context.vertex->paClVsOutCntl == pointRequest.context.vertex->paClVsOutCntl);
            Require(!replayLegacy.request.context.vertex->pointList && replayLegacy.request.context.vertex->pointSize == 1.0f);
            Require(replayLegacy.request.context.vertex->pointSizeMin == 0.0f && replayLegacy.request.context.vertex->pointSizeMax == 0.0f);
            const auto upgradedLegacy = DecodeBase64(serializer.Serialize(replayLegacy.request));
            Require(upgradedLegacy[4] == 35u && upgradedLegacy.size() == legacy.size() + 13u);
        }
    }

    pointRequest.context.pixel = pixel;
    auto legacy34 = DecodeBase64(serializer.Serialize(pointRequest));
    legacy34.resize(legacy34.size() - 13u);
    legacy34[4] = 34u;
    const auto replay34 = serializer.Deserialize(EncodeBase64(legacy34));
    Require(replay34.request.context.pixel.has_value() && replay34.request.context.pixel->iteratesSamples && replay34.request.context.pixel->positionLocation == pixel.positionLocation && replay34.request.context.pixel->sampleLocations == pixel.sampleLocations);
    Require(replay34.request.context.vertex.has_value() && !replay34.request.context.vertex->pointList && replay34.request.context.vertex->pointSize == 1.0f);

    return 0;
}
