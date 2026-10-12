#include "Mixer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void Require(bool value, const char* message) {
    if (value) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

int main() {
    std::int16_t pcm[4] = {16384, -16384, 8192, -8192};
    Audio3dInternal::Source s{pcm, 2, 2, false, 1.0f, 0.0f};
    std::vector<std::int16_t> out;
    Audio3dInternal::MixFrame(&s, 1, 2, out);
    Require(out.size() == 4, "frame is granularity*2 samples");
    Require(out[0] == 16384 && out[1] == -16384, "centered stereo passes through");
    std::int16_t panned[2] = {10000, 20000};
    Audio3dInternal::Source hard_right{panned, 1, 2, false, 1.0f, 1.0f};
    std::vector<std::int16_t> panned_out;
    Audio3dInternal::MixFrame(&hard_right, 1, 1, panned_out);
    Require(panned_out.size() == 2 && panned_out[0] == 0 && panned_out[1] == 19999, "hard right keeps only the right channel");
    return 0;
}
