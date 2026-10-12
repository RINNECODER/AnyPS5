#include "prx/libScePad/include/InputScript.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "pad_input_script: FAILED: %s\n", what);
    std::exit(1);
}

void RequireRejected(const char* script, const char* fragment) {
    try {
        Pad::ParseInputScript(script);
    } catch (const std::invalid_argument& error) {
        Require(std::string(error.what()).find(fragment) != std::string::npos, script);
        return;
    }
    Require(false, script);
}

constexpr std::uint64_t Seconds(double value) { return static_cast<std::uint64_t>(value * 1e6); }

}

int main() {
    // Entries are sorted by time; buttons combine with '+', names ignore case, hold is optional.
    const auto presses = Pad::ParseInputScript(" t=45.5:Down ; t=40:cross;t=46:cross+R1:400; ");
    Require(presses.size() == 3, "three entries");
    Require(presses[0].atMicros == Seconds(40) && presses[0].buttons == 0x4000 && presses[0].holdMicros == 250000, "default hold");
    Require(presses[1].atMicros == Seconds(45.5) && presses[1].buttons == 0x0040, "fractional time");
    Require(presses[2].buttons == (0x4000 | 0x0800) && presses[2].holdMicros == 400000, "combined buttons and hold");

    RequireRejected("t=40:jump", "unknown button 'jump'");
    RequireRejected("40:cross", "expected t=");
    RequireRejected("t=abc:cross", "invalid time");
    RequireRejected("t=40:cross:0", "hold must be");
    RequireRejected("t=40", "missing buttons");

    // A press goes down at its time and stays down for its hold from the first read that sees it,
    // even when the title reads long after the scheduled time.
    Pad::InputScript script(Pad::ParseInputScript("t=1:cross;t=2:circle:50"));
    std::vector<const Pad::ScriptedPress*> started;
    Require(script.Buttons(Seconds(0.5), &started) == 0 && started.empty(), "nothing before the first press");
    Require(script.Buttons(Seconds(1.9), &started) == 0x4000 && started.size() == 1, "late read still sees the press");
    Require(script.Buttons(Seconds(2.05), &started) == (0x4000 | 0x2000) && started.size() == 2, "overlapping presses");
    Require(script.Buttons(Seconds(2.12), &started) == 0x4000, "short hold released first");
    Require(script.Buttons(Seconds(2.2), &started) == 0, "both released");
    Require(script.Buttons(Seconds(9), &started) == 0 && started.size() == 2, "each press fires once");

    std::puts("pad_input_script: ok");
    return 0;
}
