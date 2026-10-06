// SPDX-License-Identifier: MIT
//
// The window: a header, the list of what can be opened on the left, one page
// for what is open, and a line at the bottom.
#pragma once

namespace gui {

struct App;

// Draws one frame of the window.
void draw_window(App& app);

} // namespace gui
