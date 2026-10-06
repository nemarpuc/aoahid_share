// SPDX-License-Identifier: MIT
#pragma once

#include <string>

namespace aoas {

// Where the settings, the daemon's own state and the control socket live
// (docs/CONFIG.md). The directories are created on first use.
//
// settings_dir() is the "settings" folder next to the program, or a folder of
// the user's own when that cannot be written to; AOAHID_SHARE_SETTINGS_DIR
// names another. It holds config.ini, state.ini, daemon.log and device/.
[[nodiscard]] std::string settings_dir();
[[nodiscard]] std::string config_file_path();
[[nodiscard]] std::string state_file_path();
// A Unix socket path, or a named pipe name on Windows. Empty when no place
// private to this user could be found for it.
[[nodiscard]] std::string control_endpoint();

[[nodiscard]] bool read_file(const std::string& path, std::string& out);
// Writes through a temporary file so a crash never leaves half a file.
[[nodiscard]] bool write_file(const std::string& path, const std::string& text);

// The value of `key = value` in an ini-style text, or empty.
[[nodiscard]] std::string state_get(const std::string& text, const std::string& key);
// The text with that key set (or removed, for an empty value).
[[nodiscard]] std::string state_set(const std::string& text, const std::string& key,
                                    const std::string& value);

} // namespace aoas
