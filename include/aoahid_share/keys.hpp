// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/config.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace aoas {

// Where a setting lives: in config.ini, in a device's file, or in both (the
// computer's value is the default and a device may replace it).
enum class KeyScope { computer, device, both };
enum class KeyKind { boolean, integer, real, choice, text, hotkey, list };

// What the settings window needs to know about one key of the files. Only
// words are kept here: what a value may be is decided by the parsers, so
// there is one place that does, and these tables cannot disagree with it.
struct ConfigKey {
    KeyScope scope;
    // As in the file: "daemon", "motion", ...
    const char* section;
    const char* key;
    // The heading the window shows it under.
    const char* group;
    KeyKind kind;
    // "a|b|c" for a choice; for a number, the range as "low..high" (shown
    // only); otherwise empty.
    const char* choices;
    const char* unit;
    // What is in force when the line is absent; empty when nothing is.
    const char* initial;
    // One line, as in docs/CONFIG.md.
    const char* help;
    // Shown, never typed: changing it would mean nothing.
    bool read_only = false;
};

[[nodiscard]] std::span<const ConfigKey> config_keys();

// The key of that place for that side (a key both may set is found from
// either), or null.
[[nodiscard]] const ConfigKey* find_config_key(KeyScope scope, std::string_view section,
                                               std::string_view key);

// The text a key holds now, or nullopt when its line is absent. `device` is
// null for a computer-wide row, else the device whose file is meant.
[[nodiscard]] std::optional<std::string> key_text(const Config& config, const DeviceConfig* device,
                                                  const ConfigKey& key);

// Takes `text` for the key, through the parsers. Returns what they object to,
// or empty; on an objection nothing is changed.
[[nodiscard]] std::string set_key_text(Config& config, DeviceConfig* device, const ConfigKey& key,
                                       std::string_view text);

// Removes the key's line: a device goes back to the computer's value, a
// computer-wide key back to its default.
[[nodiscard]] std::string clear_key(Config& config, DeviceConfig* device, const ConfigKey& key);

// One key of the daemon's `status` answer. Per device the daemon writes
// device.<serial>.<key>; the others are the top level.
struct StatusKey {
    bool per_device;
    // "queue_us"; a key that ends in '#' stands for a numbered or serial-named
    // one ("monitor.#" is monitor.1, monitor.2, ...).
    const char* key;
    // Device | Link | Timing | Position | Host | Files
    const char* group;
    const char* unit;
    // The names of the numbers when a value is several, as "a|b|c"; else empty.
    const char* parts;
    const char* help;
};

[[nodiscard]] std::span<const StatusKey> status_keys();

// The entry for a key as the daemon writes it, or null when the table does
// not know it (a newer daemon): the window then shows it as it is.
[[nodiscard]] const StatusKey* find_status_key(bool per_device, std::string_view key);

} // namespace aoas
