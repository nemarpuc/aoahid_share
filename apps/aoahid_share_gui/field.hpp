// SPDX-License-Identifier: MIT
//
// One row of settings: a key of the tables (aoahid_share/keys.hpp) as a
// label, a control, its unit and default, and its help. It is the only place
// a value is edited; what a value may be is the parsers' to say.
#pragma once

#include "aoahid_share/keys.hpp"

#include <string>

namespace gui {

struct App;

// Draws the row for `key`: its name at the left (the help is a tip on it) and
// its control at the right edge. `device` is null for a computer-wide row, else
// the device whose file is edited; `label` is shown in place of the key's name.
// Returns true when the value changed.
bool field(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
           const char* label = nullptr);

// Sets or clears a key from a control that is not its own row (a button), with
// the same bookkeeping: through the parsers, the reason kept on a refusal.
bool set_field(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
               const std::string& text);
bool clear_field(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device);

} // namespace gui
