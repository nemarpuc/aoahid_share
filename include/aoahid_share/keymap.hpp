// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aoas {

struct HidKey {
    uint16_t usage{};
    // True for a Consumer page usage (media key), false for the Keyboard page.
    bool media{};
};

// Linux input key code (linux/input-event-codes.h KEY_*) to its HID usage.
// The mapping is by physical key; Android's own layout turns it into
// characters. False for a key with no entry.
[[nodiscard]] bool evdev_to_hid(unsigned code, HidKey& out) noexcept;

// Linux BTN_LEFT..BTN_TASK to a one-based HID button. False for other codes.
[[nodiscard]] bool evdev_to_button(unsigned code, unsigned& button) noexcept;

// Modifier bits a Hotkey and the pressed state are compared with.
inline constexpr uint8_t mod_ctrl = 1;
inline constexpr uint8_t mod_shift = 2;
inline constexpr uint8_t mod_alt = 4;
inline constexpr uint8_t mod_meta = 8;

// The modifier bit of a Keyboard page usage 0xE0-0xE7, else 0.
[[nodiscard]] uint8_t modifier_bit(uint16_t usage) noexcept;

struct Hotkey {
    uint8_t modifiers{};
    uint16_t usage{};
    [[nodiscard]] bool set() const noexcept { return usage != 0; }
};

// A media key that a hotkey or the `media` command can send: its name in the
// config file and its Consumer page usage.
struct MediaKey {
    std::string_view name;
    uint16_t usage;
};
inline constexpr MediaKey media_keys[] = {
    {"previous", 0x00B6},      {"play_pause", 0x00CD},      {"next", 0x00B5},
    {"brightness_up", 0x006F}, {"brightness_down", 0x0070},
};
inline constexpr size_t media_key_count = sizeof(media_keys) / sizeof(media_keys[0]);

// Parses "ctrl+alt+s". An empty string gives an unset Hotkey and succeeds.
// False for an unknown name or a combination without a non-modifier key.
[[nodiscard]] bool parse_hotkey(std::string_view text, Hotkey& out) noexcept;

} // namespace aoas
