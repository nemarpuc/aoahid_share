// SPDX-License-Identifier: MIT
//
// Everything the command line can do that is not a setting: the buttons for
// the computer and for a device, and the questions asked before something
// is removed or stopped.
#pragma once

#include "aoahid_share/config.hpp"

namespace gui {

struct App;

// The buttons for the whole program: pausing, the media keys.
void draw_actions(App& app);
// Where the files are, and exporting and importing everything.
void draw_files(App& app);
// Sending the input to a device, forgetting it, and exporting or importing it.
void draw_device_tools(App& app, aoas::DeviceConfig& device);
// The buttons under the list on the left: for the computer, disconnecting every
// device and stopping the daemon; for a device, connecting or disconnecting it.
void draw_sidebar_buttons(App& app);
// How much room they take.
[[nodiscard]] float sidebar_buttons_height(const App& app);

// The steps that set a device up by eye, for one without USB debugging.
void draw_probe(App& app, aoas::DeviceConfig& device);

// The questions that are open. Called once a frame.
void draw_confirmations(App& app);

} // namespace gui
