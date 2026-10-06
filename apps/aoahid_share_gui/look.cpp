// SPDX-License-Identifier: MIT
#include "look.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

// The UI font, compiled in (see CMakeLists.txt).
extern const unsigned char aoahid_player_font_data[];
extern const unsigned int aoahid_player_font_size;

namespace gui::look {
namespace {

float g_scale = 1.0F;
std::string g_jump;

ImVec4 colour(const ImU32 value) { return ImGui::ColorConvertU32ToFloat4(value); }

// The first byte of a UTF-8 character, or any ASCII byte.
bool is_character_start(const char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }

} // namespace

float px(const float value) { return value * g_scale; }

void setup(const float scale) {
    g_scale = scale;
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(aoahid_player_font_data),
                                   static_cast<int>(aoahid_player_font_size), 15.0F * scale,
                                   &config);

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsLight(&style);
    style.ScaleAllSizes(scale);
    style.WindowRounding = 0.0F;
    style.ChildRounding = 0.0F;
    style.FrameRounding = 0.0F;
    style.PopupRounding = 0.0F;
    style.GrabRounding = 0.0F;
    style.ScrollbarRounding = 0.0F;
    style.TabRounding = 0.0F;
    style.FrameBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg] = colour(paper);
    c[ImGuiCol_ChildBg] = colour(paper);
    c[ImGuiCol_PopupBg] = colour(paper);
    c[ImGuiCol_Text] = colour(ink);
    c[ImGuiCol_TextDisabled] = colour(grey);
    c[ImGuiCol_Border] = colour(ink);
    c[ImGuiCol_FrameBg] = colour(paper);
    c[ImGuiCol_FrameBgHovered] = colour(light);
    c[ImGuiCol_FrameBgActive] = colour(light);
    c[ImGuiCol_Button] = colour(paper);
    c[ImGuiCol_ButtonHovered] = colour(light);
    c[ImGuiCol_ButtonActive] = colour(light);
    c[ImGuiCol_Header] = colour(light);
    c[ImGuiCol_HeaderHovered] = colour(light);
    c[ImGuiCol_HeaderActive] = colour(light);
    c[ImGuiCol_CheckMark] = colour(ink);
    c[ImGuiCol_SliderGrab] = colour(ink);
    c[ImGuiCol_SliderGrabActive] = colour(ink);
    c[ImGuiCol_ScrollbarBg] = colour(paper);
    c[ImGuiCol_ScrollbarGrab] = colour(grey);
    c[ImGuiCol_ScrollbarGrabHovered] = colour(ink);
    c[ImGuiCol_ScrollbarGrabActive] = colour(ink);
    c[ImGuiCol_Separator] = colour(ink);
    c[ImGuiCol_TextSelectedBg] = colour(light);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(1.0F, 1.0F, 1.0F, 0.6F);
}

const char* mark(const std::string& state, const bool enabled) {
    if (!enabled)
        return "[-]";
    if (state == "ready")
        return "[x]";
    if (state == "error")
        return "[!]";
    return "[ ]";
}

bool toggle(const char* const id, bool* const on) {
    const ImVec2 size(px(40), px(20));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##toggle", size);
    ImGui::PopID();
    if (clicked)
        *on = !*on;
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const float radius = size.y / 2.0F;
    const ImVec2 end(origin.x + size.x, origin.y + size.y);
    draw->AddRectFilled(origin, end, *on ? ink : paper, radius);
    draw->AddRect(origin, end, ink, radius, 0, 1.5F);
    const float knob_x = *on ? end.x - radius : origin.x + radius;
    draw->AddCircleFilled(ImVec2(knob_x, origin.y + radius), radius - px(4), *on ? paper : ink);
    return clicked;
}

void row(const char* const label, const char* const tip, const float width) {
    const float start = ImGui::GetCursorPosX();
    const float available = ImGui::GetContentRegionAvail().x;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tip != nullptr && tip[0] != '\0' && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(start + available - width);
}

void row_end() {
    ImGui::Spacing();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(at, ImVec2(at.x + width, at.y), light);
    ImGui::Dummy(ImVec2(0, 1.0F));
}

int choose(const char* const* const names, const int count, const int selected) {
    int result = selected;
    for (int index = 0; index < count; ++index) {
        ImGui::PushID(index);
        const bool on = index == selected;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ink);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ink);
            ImGui::PushStyleColor(ImGuiCol_Text, paper);
        }
        if (ImGui::Button(names[index]))
            result = index;
        if (on)
            ImGui::PopStyleColor(3);
        ImGui::PopID();
        if (index + 1 < count)
            ImGui::SameLine(0.0F, 0.0F);
    }
    return result;
}

void dim(const char* const text) {
    // Long help wraps at the edge of the window or of the table cell.
    ImGui::PushTextWrapPos(0.0F);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

void heading(const char* const text) {
    ImGui::Spacing();
    if (g_jump == text) {
        ImGui::SetScrollHereY(0.0F);
        g_jump.clear();
    }
    ImGui::SeparatorText(text);
}

void jump_to(const char* const text) { g_jump = text; }

void draw_text_ellipsized(ImDrawList* const list, const ImVec2 pos, const ImU32 colour,
                          const char* const text, const float max_width) {
    const size_t length = std::strlen(text);
    if (max_width <= 0.0F || ImGui::CalcTextSize(text, text + length).x <= max_width) {
        list->AddText(pos, colour, text);
        return;
    }
    constexpr char ellipsis[] = "...";
    const float ellipsis_width = ImGui::CalcTextSize(ellipsis).x;
    if (ellipsis_width >= max_width) {
        list->AddText(pos, colour, ".");
        return;
    }
    const float budget = max_width - ellipsis_width;
    // The longest prefix whose width still fits.
    size_t low = 0;
    size_t high = length;
    while (low < high) {
        const size_t middle = low + (high - low + 1) / 2;
        if (ImGui::CalcTextSize(text, text + middle).x <= budget)
            low = middle;
        else
            high = middle - 1;
    }
    while (low > 0 && !is_character_start(text[low]))
        --low;
    const std::string cut = std::string(text, low) + ellipsis;
    list->AddText(pos, colour, cut.c_str());
}

bool icon_button_refresh(const char* const id, const char* const tooltip) {
    const float side = ImGui::GetFrameHeight();
    const ImVec2 size(side, side);
    ImGui::PushStyleColor(ImGuiCol_Border, colour(paper));
    const bool pressed = ImGui::Button(id, size);
    ImGui::PopStyleColor();
    if (tooltip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);

    const ImVec2 p_min = ImGui::GetItemRectMin();
    const ImVec2 p_max = ImGui::GetItemRectMax();
    const ImVec2 c((p_min.x + p_max.x) * 0.5F, (p_min.y + p_max.y) * 0.5F);
    const float s = std::min(size.x, size.y);
    const float t = std::max(1.0F, px(1.2F));
    constexpr float pi = 3.14159265358979323846F;
    const float r = s * 0.30F;
    const float a0 = -pi * 0.35F;
    const float a1 = pi * 1.35F;
    ImDrawList* const list = ImGui::GetWindowDrawList();
    list->PathArcTo(c, r, a0, a1, 24);
    list->PathStroke(ink, 0, t);
    const ImVec2 tip(c.x + r * std::cos(a0), c.y + r * std::sin(a0));
    const float h = s * 0.16F;
    list->AddTriangleFilled(ImVec2(tip.x - h * 0.2F, tip.y - h * 1.1F),
                            ImVec2(tip.x + h * 1.1F, tip.y + h * 0.1F),
                            ImVec2(tip.x - h * 0.6F, tip.y + h * 0.6F), ink);
    return pressed;
}

} // namespace gui::look
