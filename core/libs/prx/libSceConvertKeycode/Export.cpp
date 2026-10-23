#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"

namespace {
constexpr int kErrInvalidAddress = static_cast<int>(0x80BC0031u);
constexpr int kErrInvalidUserId = static_cast<int>(0x80BC0010u);
constexpr std::array<std::uint16_t, 256> BaseVirtualKeys = {
    255, 255, 255, 255, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76,
    77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 48, 13, 27, 8, 9, 32, 187, 219, 221,
    186, 191, 191, 192, 222, 220, 188, 190, 189, 20, 112, 113, 114, 115, 116, 117,
    118, 119, 120, 121, 122, 123, 44, 145, 19, 45, 36, 33, 46, 35, 34, 39,
    37, 40, 38, 144, 111, 106, 109, 107, 13, 97, 98, 99, 100, 101, 102, 103,
    104, 105, 96, 110, 226, 93, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 12, 255, 255, 255, 255, 255, 255, 255,
    162, 160, 164, 91, 163, 161, 165, 92, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

constexpr auto VirtualKeys = [] {
    std::array<std::array<std::uint16_t, 256>, 39> layouts{};
    for (auto& layout : layouts) layout = BaseVirtualKeys;
    layouts[1][28] = 90;
    layouts[1][29] = 89;
    layouts[1][45] = 189;
    layouts[1][46] = 221;
    layouts[1][47] = 186;
    layouts[1][48] = 187;
    layouts[2][28] = 90;
    layouts[2][29] = 89;
    layouts[2][45] = 219;
    layouts[2][46] = 221;
    layouts[2][47] = 186;
    layouts[2][48] = 192;
    layouts[2][49] = 223;
    layouts[2][50] = 223;
    layouts[2][51] = 222;
    layouts[2][52] = 220;
    layouts[2][53] = 191;
    layouts[3][45] = 189;
    layouts[3][46] = 187;
    layouts[3][47] = 219;
    layouts[3][48] = 221;
    layouts[3][49] = 220;
    layouts[3][50] = 220;
    layouts[3][51] = 186;
    layouts[3][53] = 192;
    layouts[3][56] = 191;
    layouts[3][100] = 255;
    layouts[4][45] = 189;
    layouts[4][46] = 187;
    layouts[4][47] = 219;
    layouts[4][48] = 221;
    layouts[4][49] = 222;
    layouts[4][50] = 222;
    layouts[4][51] = 186;
    layouts[4][52] = 192;
    layouts[4][53] = 223;
    layouts[4][56] = 191;
    layouts[5][45] = 219;
    layouts[5][46] = 221;
    layouts[5][47] = 186;
    layouts[5][48] = 187;
    layouts[6][45] = 219;
    layouts[6][46] = 221;
    layouts[6][47] = 186;
    layouts[6][48] = 187;
    layouts[6][100] = 255;
    layouts[8][4] = 81;
    layouts[8][16] = 188;
    layouts[8][20] = 65;
    layouts[8][26] = 90;
    layouts[8][29] = 87;
    layouts[8][45] = 219;
    layouts[8][46] = 187;
    layouts[8][49] = 220;
    layouts[8][50] = 220;
    layouts[8][51] = 77;
    layouts[8][52] = 192;
    layouts[8][53] = 222;
    layouts[8][54] = 190;
    layouts[8][55] = 191;
    layouts[8][56] = 223;
    layouts[9][4] = 81;
    layouts[9][16] = 188;
    layouts[9][20] = 65;
    layouts[9][26] = 90;
    layouts[9][29] = 87;
    layouts[9][45] = 219;
    layouts[9][46] = 189;
    layouts[9][49] = 220;
    layouts[9][50] = 220;
    layouts[9][51] = 77;
    layouts[9][52] = 192;
    layouts[9][53] = 222;
    layouts[9][54] = 190;
    layouts[9][55] = 191;
    layouts[9][56] = 187;
    layouts[10][45] = 189;
    layouts[10][46] = 187;
    layouts[10][47] = 219;
    layouts[10][48] = 221;
    layouts[10][49] = 220;
    layouts[10][50] = 220;
    layouts[10][51] = 186;
    layouts[10][52] = 192;
    layouts[10][53] = 222;
    layouts[10][56] = 191;
    layouts[11] = layouts[2];
    layouts[12] = layouts[5];
    layouts[13][45] = 219;
    layouts[13][46] = 191;
    layouts[13][49] = 220;
    layouts[13][50] = 220;
    layouts[13][51] = 187;
    layouts[13][52] = 192;
    layouts[13][53] = 222;
    layouts[15][46] = 191;
    layouts[15][47] = 219;
    layouts[15][48] = 221;
    layouts[15][49] = 220;
    layouts[15][50] = 220;
    layouts[15][51] = 186;
    layouts[15][53] = 192;
    layouts[16][45] = 189;
    layouts[16][46] = 187;
    layouts[16][47] = 219;
    layouts[16][48] = 221;
    layouts[16][49] = 220;
    layouts[16][50] = 220;
    layouts[16][51] = 186;
    layouts[16][53] = 192;
    layouts[16][56] = 191;
    layouts[17] = layouts[5];
    layouts[18] = layouts[3];
    layouts[20][45] = 223;
    layouts[20][46] = 189;
    layouts[20][47] = 219;
    layouts[20][48] = 221;
    layouts[20][49] = 188;
    layouts[20][50] = 188;
    layouts[20][51] = 186;
    layouts[20][52] = 190;
    layouts[20][53] = 192;
    layouts[20][54] = 191;
    layouts[20][55] = 220;
    layouts[20][56] = 190;
    layouts[21][45] = 189;
    layouts[21][46] = 222;
    layouts[21][47] = 192;
    layouts[21][48] = 219;
    layouts[21][49] = 221;
    layouts[21][50] = 221;
    layouts[21][51] = 187;
    layouts[21][52] = 186;
    layouts[21][53] = 243;
    layouts[21][56] = 191;
    layouts[21][100] = 255;
    layouts[21][135] = 226;
    layouts[21][136] = 242;
    layouts[21][137] = 220;
    layouts[21][138] = 28;
    layouts[21][139] = 247;
    layouts[22] = layouts[21];
    layouts[23][45] = 189;
    layouts[23][46] = 187;
    layouts[23][47] = 219;
    layouts[23][48] = 221;
    layouts[23][49] = 220;
    layouts[23][50] = 220;
    layouts[23][51] = 186;
    layouts[23][53] = 192;
    layouts[23][56] = 191;
    layouts[23][136] = 21;
    layouts[23][138] = 25;
    layouts[24] = layouts[3];
    layouts[25] = layouts[3];
    layouts[26] = layouts[3];
    layouts[27] = layouts[3];
    layouts[28] = layouts[3];
    layouts[29] = layouts[16];
    layouts[30] = layouts[16];
    layouts[31][28] = 90;
    layouts[31][29] = 89;
    layouts[31][46] = 191;
    layouts[31][47] = 219;
    layouts[31][48] = 221;
    layouts[31][49] = 220;
    layouts[31][50] = 220;
    layouts[31][51] = 186;
    layouts[31][53] = 192;
    layouts[32] = layouts[16];
    layouts[33] = layouts[16];
    layouts[34] = layouts[16];
    layouts[35] = layouts[16];
    layouts[36][28] = 90;
    layouts[36][29] = 89;
    layouts[36][39] = 192;
    layouts[36][45] = 191;
    layouts[36][46] = 187;
    layouts[36][47] = 219;
    layouts[36][48] = 221;
    layouts[36][49] = 220;
    layouts[36][50] = 220;
    layouts[36][51] = 186;
    layouts[36][53] = 48;
    layouts[37] = layouts[3];
    layouts[38] = layouts[3];
    return layouts;
}();

constexpr int kErrInvalidKeyboardType = static_cast<int>(0x80BC001Du);
constexpr std::uint32_t kIgnoredStateBits = 0x4000000u;

char16_t UsCharacter(std::uint16_t keycode) {
    static constexpr char16_t kPunctuation[] = u"-=[]\\\0;'`,./";
    static constexpr char16_t kKeypad[] = u"/*-+\0" u"1234567890.";
    if (keycode >= 0x04 && keycode <= 0x1D) return static_cast<char16_t>(u'a' + (keycode - 0x04));
    if (keycode >= 0x1E && keycode <= 0x26) return static_cast<char16_t>(u'1' + (keycode - 0x1E));
    if (keycode == 0x27) return u'0';
    if (keycode == 0x2C) return u' ';
    if (keycode >= 0x2D && keycode <= 0x38) return kPunctuation[keycode - 0x2D];
    if (keycode >= 0x54 && keycode <= 0x63) return kKeypad[keycode - 0x54];
    return 0;
}
}  // namespace

extern "C" {

int APS5_VABI sceConvertKeycodeGetImeKeyboardType(int32_t userId, uint32_t* type) {
 if (type == nullptr) return kErrInvalidAddress;
 if (userId < 0) return kErrInvalidUserId;
 *type = 0;
 return 0;
}
int APS5_VABI sceConvertKeycodeGetVirtualKeycode(std::uint16_t keycode, std::uint32_t keyboardType, std::uint16_t* virtualKeycode) {
    if (virtualKeycode == nullptr) return kErrInvalidAddress;
    if (keyboardType == 0 || keyboardType > VirtualKeys.size()) return kErrInvalidKeyboardType;
    if (keyboardType >= 31) throw std::runtime_error("sceConvertKeycodeGetVirtualKeycode: guest sdk version is unavailable");
    auto value = keycode < BaseVirtualKeys.size() ? VirtualKeys[keyboardType - 1][keycode] : 255;
    if ((keyboardType == 2 || keyboardType == 3 || keyboardType == 12) && (value == 89 || value == 90)) throw std::runtime_error("sceConvertKeycodeGetVirtualKeycode: guest sdk version is unavailable");
    *virtualKeycode = static_cast<std::uint16_t>(value);
    return 0;
}

int APS5_VABI sceConvertKeycodeGetCharacterFromKeyboardData(std::uint16_t keycode, std::uint32_t state, std::uint32_t type, char16_t* character) {
    if (character == nullptr) return kErrInvalidAddress;
    if ((state & ~kIgnoredStateBits) != 0) throw std::runtime_error("sceConvertKeycodeGetCharacterFromKeyboardData: unsupported state " + std::to_string(state));
    if (type != 0) throw std::runtime_error("sceConvertKeycodeGetCharacterFromKeyboardData: unsupported keyboard type " + std::to_string(type));
    *character = UsCharacter(keycode);
    return 0;
}

int APS5_VABI sceConvertKeycodeGetCharacter(std::uint16_t keycode, std::uint32_t state, std::uint32_t type, char16_t* character) {
    if (character == nullptr) return kErrInvalidAddress;
    if ((state & ~kIgnoredStateBits) != 0) throw std::runtime_error("sceConvertKeycodeGetCharacter: unsupported state " + std::to_string(state));
    if (type != 0) throw std::runtime_error("sceConvertKeycodeGetCharacter: unsupported keyboard type " + std::to_string(type));
    *character = UsCharacter(keycode);
    return 0;
}

}
