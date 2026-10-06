// SPDX-License-Identifier: MIT
#include "aoahid_share/keymap.hpp"

#include <array>
#include <cctype>

namespace aoas {
namespace {

struct Entry {
    uint16_t code;
    uint16_t usage;
};

// Keyboard page. Codes are Linux KEY_* values, usages are HUT 1.5 section 10.
constexpr Entry keyboard[] = {
    {1, 0x29},                                                      // ESC
    {2, 0x1E},   {3, 0x1F},   {4, 0x20},   {5, 0x21},   {6, 0x22},  // 1-5
    {7, 0x23},   {8, 0x24},   {9, 0x25},   {10, 0x26},  {11, 0x27}, // 6-0
    {12, 0x2D},  {13, 0x2E},  {14, 0x2A},  {15, 0x2B},              // - = Backspace Tab
    {16, 0x14},  {17, 0x1A},  {18, 0x08},  {19, 0x15},  {20, 0x17}, // Q W E R T
    {21, 0x1C},  {22, 0x18},  {23, 0x0C},  {24, 0x12},  {25, 0x13}, // Y U I O P
    {26, 0x2F},  {27, 0x30},  {28, 0x28},  {29, 0xE0},              // [ ] Enter LeftCtrl
    {30, 0x04},  {31, 0x16},  {32, 0x07},  {33, 0x09},  {34, 0x0A}, // A S D F G
    {35, 0x0B},  {36, 0x0D},  {37, 0x0E},  {38, 0x0F},              // H J K L
    {39, 0x33},  {40, 0x34},  {41, 0x35},  {42, 0xE1},  {43, 0x31}, // ; ' ` LeftShift Backslash
    {44, 0x1D},  {45, 0x1B},  {46, 0x06},  {47, 0x19},  {48, 0x05}, // Z X C V B
    {49, 0x11},  {50, 0x10},  {51, 0x36},  {52, 0x37},  {53, 0x38}, // N M , . /
    {54, 0xE5},  {55, 0x55},  {56, 0xE2},  {57, 0x2C},  {58, 0x39}, // RShift KP* LAlt Space Caps
    {59, 0x3A},  {60, 0x3B},  {61, 0x3C},  {62, 0x3D},  {63, 0x3E}, // F1-F5
    {64, 0x3F},  {65, 0x40},  {66, 0x41},  {67, 0x42},  {68, 0x43}, // F6-F10
    {69, 0x53},  {70, 0x47},                                        // NumLock ScrollLock
    {71, 0x5F},  {72, 0x60},  {73, 0x61},  {74, 0x56},              // KP7 KP8 KP9 KP-
    {75, 0x5C},  {76, 0x5D},  {77, 0x5E},  {78, 0x57},              // KP4 KP5 KP6 KP+
    {79, 0x59},  {80, 0x5A},  {81, 0x5B},  {82, 0x62},  {83, 0x63}, // KP1 KP2 KP3 KP0 KP.
    {85, 0x35},  {86, 0x64},  {87, 0x44},  {88, 0x45},              // Zenkaku/Hankaku 102nd F11 F12
    {89, 0x87},  {90, 0x92},  {91, 0x93},  {92, 0x8A},              // Ro Katakana Hiragana Henkan
    {93, 0x88},  {94, 0x8B},  {95, 0x8C}, // Katakana/Hiragana Muhenkan KPJPComma
    {96, 0x58},  {97, 0xE4},  {98, 0x54},  {99, 0x46},  {100, 0xE6}, // KPEnter RCtrl KP/ SysRq RAlt
    {102, 0x4A}, {103, 0x52}, {104, 0x4B}, {105, 0x50}, {106, 0x4F}, // Home Up PgUp Left Right
    {107, 0x4D}, {108, 0x51}, {109, 0x4E}, {110, 0x49}, {111, 0x4C}, // End Down PgDn Ins Del
    {117, 0x67}, {119, 0x48}, {121, 0x85},                           // KP= Pause KPComma
    {122, 0x90}, {123, 0x91}, {124, 0x89},                           // Hangeul Hanja Yen
    {125, 0xE3}, {126, 0xE7}, {127, 0x65},                           // LeftMeta RightMeta Compose
    {183, 0x68}, {184, 0x69}, {185, 0x6A}, {186, 0x6B}, {187, 0x6C}, {188, 0x6D}, // F13-F18
    {189, 0x6E}, {190, 0x6F}, {191, 0x70}, {192, 0x71}, {193, 0x72}, {194, 0x73}, // F19-F24
};

// Consumer page; only usages the toggle Node declares.
constexpr Entry consumer[] = {
    {113, 0x00E2}, // MUTE
    {114, 0x00EA}, // VOLUMEDOWN
    {115, 0x00E9}, // VOLUMEUP
    {158, 0x0224}, // BACK
    {163, 0x00B5}, // NEXTSONG
    {164, 0x00CD}, // PLAYPAUSE
    {165, 0x00B6}, // PREVIOUSSONG
    {166, 0x00B7}, // STOPCD
    {172, 0x0223}, // HOMEPAGE
    {224, 0x0070}, // BRIGHTNESSDOWN
    {225, 0x006F}, // BRIGHTNESSUP
};

struct Name {
    std::string_view name;
    uint16_t usage;
};

constexpr Name names[] = {
    {"escape", 0x29}, {"esc", 0x29},       {"space", 0x2C}, {"tab", 0x2B},
    {"enter", 0x28},  {"backspace", 0x2A}, {"pause", 0x48}, {"scrolllock", 0x47},
    {"insert", 0x49}, {"delete", 0x4C},    {"home", 0x4A},  {"end", 0x4D},
    {"pageup", 0x4B}, {"pagedown", 0x4E},  {"up", 0x52},    {"down", 0x51},
    {"left", 0x50},   {"right", 0x4F},
};

} // namespace

bool evdev_to_hid(const unsigned code, HidKey& out) noexcept {
    for (const Entry& entry : keyboard) {
        if (entry.code == code) {
            out = {entry.usage, false};
            return true;
        }
    }
    for (const Entry& entry : consumer) {
        if (entry.code == code) {
            out = {entry.usage, true};
            return true;
        }
    }
    return false;
}

bool evdev_to_button(const unsigned code, unsigned& button) noexcept {
    // BTN_LEFT, RIGHT, MIDDLE, SIDE, EXTRA, FORWARD, BACK, TASK.
    if (code < 0x110 || code > 0x117)
        return false;
    button = code - 0x110 + 1;
    return true;
}

uint8_t modifier_bit(const uint16_t usage) noexcept {
    switch (usage) {
    case 0xE0:
    case 0xE4:
        return mod_ctrl;
    case 0xE1:
    case 0xE5:
        return mod_shift;
    case 0xE2:
    case 0xE6:
        return mod_alt;
    case 0xE3:
    case 0xE7:
        return mod_meta;
    default:
        return 0;
    }
}

bool parse_hotkey(const std::string_view text, Hotkey& out) noexcept {
    out = {};
    Hotkey parsed;
    size_t begin = 0;
    bool any = false;
    while (begin <= text.size()) {
        size_t end = text.find('+', begin);
        if (end == std::string_view::npos)
            end = text.size();
        std::array<char, 16> buffer{};
        size_t length = 0;
        for (size_t i = begin; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (std::isspace(c) != 0)
                continue;
            if (length + 1 >= buffer.size())
                return false;
            buffer[length++] = static_cast<char>(std::tolower(c));
        }
        const std::string_view token(buffer.data(), length);
        begin = end + 1;
        if (token.empty()) {
            if (any || end != text.size())
                return false;
            break;
        }
        any = true;

        if (token == "ctrl" || token == "control") {
            parsed.modifiers |= mod_ctrl;
        } else if (token == "shift") {
            parsed.modifiers |= mod_shift;
        } else if (token == "alt") {
            parsed.modifiers |= mod_alt;
        } else if (token == "meta" || token == "super" || token == "win" || token == "cmd") {
            parsed.modifiers |= mod_meta;
        } else if (parsed.usage != 0) {
            return false;
        } else if (token.size() == 1 && token[0] >= 'a' && token[0] <= 'z') {
            parsed.usage = static_cast<uint16_t>(0x04 + (token[0] - 'a'));
        } else if (token.size() == 1 && token[0] >= '1' && token[0] <= '9') {
            parsed.usage = static_cast<uint16_t>(0x1E + (token[0] - '1'));
        } else if (token == "0") {
            parsed.usage = 0x27;
        } else if (token[0] == 'f' && token.size() <= 3 && token.size() >= 2) {
            int number = 0;
            for (const char c : token.substr(1)) {
                if (c < '0' || c > '9')
                    return false;
                number = number * 10 + (c - '0');
            }
            if (number >= 1 && number <= 12)
                parsed.usage = static_cast<uint16_t>(0x3A + number - 1);
            else if (number >= 13 && number <= 24)
                parsed.usage = static_cast<uint16_t>(0x68 + number - 13);
            else
                return false;
        } else {
            bool found = false;
            for (const Name& name : names) {
                if (name.name == token) {
                    parsed.usage = name.usage;
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    if (any && parsed.usage == 0)
        return false;
    out = parsed;
    return true;
}

} // namespace aoas
