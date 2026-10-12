#ifndef CORE_LIBS_PRX_LIBSCEPAD_INPUTSCRIPT_HPP
#define CORE_LIBS_PRX_LIBSCEPAD_INPUTSCRIPT_HPP

// Fork-only: ANYPS5_INPUT_SCRIPT, an opt-in list of timed button presses for driving a title's
// menus in automated runs (agents, CI). Off unless the variable is set. The presses go through
// the same pad state the title reads (Pad::ReadState / Pad::Read), on top of any live input.
//
//   ANYPS5_INPUT_SCRIPT="t=40:cross;t=45.5:down;t=46:cross+r1:400"
//
// Each entry is t=<seconds since process start>:<button>[+<button>...][:<hold ms>]. The hold
// (default 250 ms, at most 5000 ms) counts from the first pad read that sees the press, so a title
// that polls slowly still sees every press go down and come back up.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pad {

struct ScriptedPress {
    std::uint64_t atMicros = 0;
    std::uint64_t holdMicros = 0;
    std::uint32_t buttons = 0;
    std::string text;
};

inline std::uint32_t ScriptButton(std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    struct Name { const char* name; std::uint32_t bit; };
    static constexpr Name names[] = {
        {"l3", 0x0002}, {"r3", 0x0004}, {"options", 0x0008}, {"up", 0x0010}, {"right", 0x0020},
        {"down", 0x0040}, {"left", 0x0080}, {"l2", 0x0100}, {"r2", 0x0200}, {"l1", 0x0400},
        {"r1", 0x0800}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000},
        {"square", 0x8000}, {"touchpad", 0x100000}};
    for (const auto& entry : names) {
        if (lower == entry.name) return entry.bit;
    }
    throw std::invalid_argument("unknown button '" + std::string(name) + "'");
}

inline double ScriptNumber(std::string_view text, const char* what) {
    const std::string value(text);
    char* end = nullptr;
    const double number = value.empty() ? -1.0 : std::strtod(value.c_str(), &end);
    if (value.empty() || end != value.c_str() + value.size() || !(number >= 0.0) || number > 1e6)
        throw std::invalid_argument(std::string("invalid ") + what + " '" + value + "'");
    return number;
}

inline std::string_view ScriptTrim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

// Throws std::invalid_argument naming the bad entry. The result is sorted by time.
inline std::vector<ScriptedPress> ParseInputScript(std::string_view script) {
    std::vector<ScriptedPress> presses;
    while (!script.empty()) {
        const auto split = script.find(';');
        const auto entry = ScriptTrim(script.substr(0, split));
        script = split == std::string_view::npos ? std::string_view{} : script.substr(split + 1);
        if (entry.empty()) continue;
        try {
            if (!entry.starts_with("t=")) throw std::invalid_argument("expected t=<seconds>:<buttons>");
            const auto fields = entry.substr(2);
            const auto timeEnd = fields.find(':');
            if (timeEnd == std::string_view::npos) throw std::invalid_argument("missing buttons");
            ScriptedPress press;
            press.text = std::string(entry);
            press.atMicros = static_cast<std::uint64_t>(ScriptNumber(fields.substr(0, timeEnd), "time") * 1e6);
            auto rest = fields.substr(timeEnd + 1);
            const auto holdStart = rest.find(':');
            double holdMs = 250.0;
            if (holdStart != std::string_view::npos) {
                holdMs = ScriptNumber(rest.substr(holdStart + 1), "hold");
                if (holdMs < 1.0 || holdMs > 5000.0) throw std::invalid_argument("hold must be 1-5000 ms");
                rest = rest.substr(0, holdStart);
            }
            press.holdMicros = static_cast<std::uint64_t>(holdMs * 1000.0);
            while (true) {
                const auto plus = rest.find('+');
                press.buttons |= ScriptButton(ScriptTrim(rest.substr(0, plus)));
                if (plus == std::string_view::npos) break;
                rest = rest.substr(plus + 1);
            }
            presses.push_back(std::move(press));
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("ANYPS5_INPUT_SCRIPT entry '" + std::string(entry) + "': " + error.what());
        }
    }
    std::stable_sort(presses.begin(), presses.end(), [](const auto& a, const auto& b) { return a.atMicros < b.atMicros; });
    return presses;
}

class InputScript {
public:
    InputScript() = default;
    explicit InputScript(std::vector<ScriptedPress> value) : presses(std::move(value)), firstSeen(presses.size(), 0) {}

    // Buttons the script holds at `now` (microseconds since process start). `started` receives
    // the presses that went down in this call, for logging.
    std::uint32_t Buttons(std::uint64_t now, std::vector<const ScriptedPress*>* started = nullptr) {
        std::uint32_t buttons = 0;
        for (std::size_t index = done; index < presses.size(); ++index) {
            const auto& press = presses[index];
            if (now < press.atMicros) break;
            if (firstSeen[index] == 0) {
                firstSeen[index] = now == 0 ? 1 : now;
                if (started != nullptr) started->push_back(&press);
            }
            if (now < firstSeen[index] + press.holdMicros) buttons |= press.buttons;
        }
        while (done < presses.size() && firstSeen[done] != 0 && now >= firstSeen[done] + presses[done].holdMicros) ++done;
        return buttons;
    }

    bool Empty() const { return presses.empty(); }

private:
    std::vector<ScriptedPress> presses;
    std::vector<std::uint64_t> firstSeen;
    std::size_t done = 0;
};

}

#endif
