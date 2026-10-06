// SPDX-License-Identifier: MIT
//
// Where a device sits: the picture to drag it in, and the few controls that
// say the same in words.
#pragma once

#include "aoahid_share/config.hpp"

namespace gui {

struct App;

void draw_placement(App& app, aoas::DeviceConfig& device);

} // namespace gui
