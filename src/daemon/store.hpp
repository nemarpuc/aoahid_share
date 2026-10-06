// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/config.hpp"

#include <string>

namespace aoas {

// The folder with one file per device, "device" under the settings folder.
[[nodiscard]] std::string devices_dir();
// The file a device with that serial is kept in. A serial is letters,
// digits and a few marks (parse_device() refuses any other), so it is the
// file's name as it stands.
[[nodiscard]] std::string device_file_path(const std::string& serial);

// Reads config.ini and every file under device/. A first run writes the
// defaults. A config.ini from an older layout, which kept its devices in numbered
// sections, is split into those files, the old one kept beside it as
// config.ini.0.2. Empty on success, else which file and what is wrong; out
// is then unchanged.
[[nodiscard]] std::string load_store(Config& out);

// Writes config.ini and one file per device, and removes the files of
// devices that are no longer in the config. False when a file could not be
// written.
[[nodiscard]] bool save_store(const Config& config);

} // namespace aoas
