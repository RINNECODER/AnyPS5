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
        pointRequest.target.spannedWaves = spanned;
        auto main32 = DecodeBase64(serializer.Serialize(pointRequest));
        main32.resize(main32.size() - 13u);
        main32[4] = 32u;
        const auto replay32 = serializer.Deserialize(EncodeBase64(main32));
        Require(replay32.request.target.spannedWaves == spanned);
        Require(replay32.request.context.vertex.has_value());
        Require(replay32.request.context.vertex->paClVsOutCntl == pointRequest.context.vertex->paClVsOutCntl);
        Require(!replay32.request.context.vertex->pointList && replay32.request.context.vertex->pointSize == 1.0f);
        Require(replay32.request.context.vertex->pointSizeMin == 0.0f && replay32.request.context.vertex->pointSizeMax == 0.0f);
        const auto upgraded32 = DecodeBase64(serializer.Serialize(replay32.request));
        Require(upgraded32[4] == 33u && upgraded32.size() == main32.size() + 13u);
    }

    return 0;
}
