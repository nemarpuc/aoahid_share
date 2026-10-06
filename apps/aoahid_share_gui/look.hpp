// SPDX-License-Identifier: MIT
//
// How the window looks: black on white, no colour. A state is told by a
// mark, never by a colour.
#pragma once

#include <imgui.h>

#include <string>

namespace gui::look {

inline constexpr ImU32 ink = IM_COL32(0, 0, 0, 255);
inline constexpr ImU32 paper = IM_COL32(255, 255, 255, 255);
inline constexpr ImU32 grey = IM_COL32(120, 120, 120, 255);
inline constexpr ImU32 light = IM_COL32(225, 225, 225, 255);

// The style and the font; `scale` is the monitor's content scale.
void setup(float scale);
// A length in the window's pixels.
[[nodiscard]] float px(float value);

// "[x]" ready, "[!]" error, "[ ]" absent or unknown, "[-]" left alone.
[[nodiscard]] const char* mark(const std::string& state, bool enabled);

void dim(const char* text);
// A switch: a track with a knob, filled when on. Returns true when it was
// clicked, and `on` is then the new value.
bool toggle(const char* id, bool* on);
// The start of a row of settings: the label at the left (with `tip` on hover
// when it is given). The next control is put at the right edge of the page and
// is `width` wide: give it SetNextItemWidth(width) if it takes one.
void row(const char* label, const char* tip, float width);
// The thin line under a row.
void row_end();
// A row of buttons of which one is selected (shown inverted). Returns the
// one clicked, else `selected`. The caller puts an id on the stack around it.
int choose(const char* const* names, int count, int selected);
// A heading over a part of the page.
void heading(const char* text);
// The next heading with this text is scrolled to the top of the page.
void jump_to(const char* text);
// `text` cut with "..." to fit `max_width`.
void draw_text_ellipsized(ImDrawList* list, ImVec2 pos, ImU32 colour, const char* text,
                          float max_width);

} // namespace gui::look
