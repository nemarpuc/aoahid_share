// SPDX-License-Identifier: MIT
//
// The picture of the monitors and the devices around them. It is worked out
// here from the config being edited, the way the daemon will once it is
// applied, so it follows every control at once.
#include "app.hpp"
#include "look.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace aoas;

namespace gui {
namespace {

struct Box {
    float x0{};
    float y0{};
    float x1{};
    float y1{};
};

// Where a device sits, in the PC's pixels.
struct Placed {
    Box box;
    // The display as it is held, and its pixels per millimetre (0: unknown).
    Size view{};
    double per_mm{};
    // PC pixels per pixel of the device, as drawn.
    double scale{};
    // False while its size is not known and its shape is a guess.
    bool sized{};
    bool ok{};
    std::string error;
    // The edge of its host it sits at; the host is a monitor or a device.
    Side side{Side::right};
    bool on_monitor{true};
    size_t host{};
    // Its extent along that edge and the part the cursor crosses at, in the
    // host's own pixels (a monitor's are the PC's).
    Span anchor{};
    Span cross{0, -1};
};

Box box_of(const Rect& r) {
    return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.x + r.w),
            static_cast<float>(r.y + r.h)};
}

size_t monitor_index(const App& app, const DeviceConfig& device) {
    for (size_t index = 0; index < app.monitors.size(); ++index) {
        if (app.monitors[index].name == device.monitor)
            return index;
    }
    return 0;
}

// The coordinate of a box's edge across it, and where the edge starts along.
float edge_at(const Box& box, const Side side) {
    return side == Side::left    ? box.x0
           : side == Side::right ? box.x1
           : side == Side::top   ? box.y0
                                 : box.y1;
}
float edge_origin(const Box& box, const Side side) { return vertical(side) ? box.y0 : box.x0; }
float edge_length(const Box& box, const Side side) {
    return vertical(side) ? box.y1 - box.y0 : box.x1 - box.x0;
}

// A box at `side` of `host`, from `from` along that edge, `length` long and
// as deep as the view's own proportions make it.
Box beside_box(const Box& host, const Side side, const float from, const float length,
               const Size view) {
    const bool is_vertical = vertical(side);
    const float along = static_cast<float>(is_vertical ? view.h : view.w);
    const float across = static_cast<float>(is_vertical ? view.w : view.h);
    const float depth = length * across / along;
    const float fixed = edge_at(host, side);
    Box box{from, from, from + length, from + length};
    if (side == Side::left) {
        box.x1 = fixed;
        box.x0 = fixed - depth;
    } else if (side == Side::right) {
        box.x0 = fixed;
        box.x1 = fixed + depth;
    } else if (side == Side::top) {
        box.y1 = fixed;
        box.y0 = fixed - depth;
    } else {
        box.y0 = fixed;
        box.y1 = fixed + depth;
    }
    return box;
}

void measure(const App& app, const DeviceConfig& device, Placed& out) {
    Size natural{device.width, device.height};
    int width = 0;
    int height = 0;
    if ((natural.w <= 0 || natural.h <= 0) &&
        std::sscanf(status_of(app, device, "size").c_str(), "%dx%d", &width, &height) == 2)
        natural = {width, height};
    out.sized = natural.w > 0 && natural.h > 0;
    out.per_mm = std::strtod(status_of(app, device, "density").c_str(), nullptr);
    if (out.sized && device.diagonal_inch > 0.0)
        out.per_mm = std::hypot(natural.w, natural.h) / (device.diagonal_inch * 25.4);
    if (!out.sized) {
        natural = {1600, 2560};
        out.per_mm = 0.0;
    }
    int rotation = device.rotation;
    if (rotation < 0)
        rotation = std::atoi(status_of(app, device, "rotation").c_str());
    out.view = view_size(natural, rotation, device.mount_rotation);
    out.side = device.side;
}

void place_on_monitor(const App& app, const DeviceConfig& device, Placed& out) {
    const size_t index = monitor_index(app, device);
    const Monitor& monitor = app.monitors[index];
    const bool is_vertical = vertical(device.side);
    double pc = monitor_density(monitor, !is_vertical);
    if (device.monitor_diagonal_inch > 0.0)
        pc = std::hypot(monitor.rect.w, monitor.rect.h) / (device.monitor_diagonal_inch * 25.4);
    if (pc <= 0.0)
        pc = monitor_density(monitor, is_vertical);

    const Span edge = is_vertical ? Span{monitor.rect.y, monitor.rect.y + monitor.rect.h - 1}
                                  : Span{monitor.rect.x, monitor.rect.x + monitor.rect.w - 1};
    PortalRequest request;
    request.monitor = index;
    request.side = device.side;
    request.android_length = is_vertical ? out.view.h : out.view.w;
    if (device.segment_length > 0) {
        request.anchor_length = device.segment_length;
    } else if (out.sized && out.per_mm > 0.0 && pc > 0.0) {
        request.anchor_length =
            static_cast<int>(std::lround(request.android_length / out.per_mm * pc));
    } else {
        request.anchor_length = edge.length() / 3;
        out.sized = false;
    }
    request.anchor_length = std::max(request.anchor_length, 2);
    request.centered = device.segment_start == centred_start;
    request.anchor_start = edge.lo + device.segment_start;
    request.corner_margin = device.corner_margin;
    Portal portal;
    out.error = resolve_portal(app.monitors, request, portal);
    if (out.error.empty()) {
        out.anchor = portal.anchor;
        out.cross = portal.segment;
    } else {
        // Still drawn where it was asked for, with nowhere to cross.
        const int start = request.centered ? edge.lo + (edge.length() - request.anchor_length) / 2
                                           : request.anchor_start;
        out.anchor = {start, start + request.anchor_length - 1};
        out.cross = {0, -1};
    }
    out.on_monitor = true;
    out.host = index;
    const float length = static_cast<float>(out.anchor.length());
    out.box = beside_box(box_of(monitor.rect), device.side, static_cast<float>(out.anchor.lo),
                         length, out.view);
    out.scale = static_cast<double>(length) / request.android_length;
    out.ok = true;
}

void place_beside(const DeviceConfig& device, const size_t host, const Placed& parent,
                  Placed& out) {
    const bool is_vertical = vertical(device.side);
    const int parent_length = is_vertical ? parent.view.h : parent.view.w;
    const int own_length = is_vertical ? out.view.h : out.view.w;
    int length = parent_length / 2;
    if (device.segment_length > 0) {
        length = device.segment_length;
    } else if (out.sized && out.per_mm > 0.0 && parent.per_mm > 0.0) {
        length = static_cast<int>(std::lround(own_length / out.per_mm * parent.per_mm));
    } else {
        out.sized = false;
    }
    length = std::max(length, 2);
    const int start =
        device.segment_start == centred_start ? (parent_length - length) / 2 : device.segment_start;
    out.anchor = {start, start + length - 1};
    out.cross = {std::max(start, 0), std::min(start + length - 1, parent_length - 1)};
    if (out.cross.hi - out.cross.lo < 1) {
        out.cross = {0, -1};
        out.error = "it does not touch that device there";
    }
    out.on_monitor = false;
    out.host = host;
    const float scale = static_cast<float>(parent.scale);
    const float from = edge_origin(parent.box, device.side) + static_cast<float>(start) * scale;
    out.box =
        beside_box(parent.box, device.side, from, static_cast<float>(length) * scale, out.view);
    out.scale = static_cast<double>(length) * parent.scale / own_length;
    out.ok = true;
}

std::vector<Placed> place_all(const App& app) {
    const std::vector<DeviceConfig>& devices = app.config.devices;
    std::vector<Placed> placed(devices.size());
    for (size_t index = 0; index < devices.size(); ++index) {
        measure(app, devices[index], placed[index]);
        if (devices[index].beside.empty())
            place_on_monitor(app, devices[index], placed[index]);
    }
    // A device beside another is placed once that one is.
    for (bool progress = true; progress;) {
        progress = false;
        for (size_t index = 0; index < devices.size(); ++index) {
            if (placed[index].ok || devices[index].beside.empty())
                continue;
            const size_t host = find_device(app.config, devices[index].beside);
            if (host >= devices.size() || host == index || !placed[host].ok)
                continue;
            place_beside(devices[index], host, placed[host], placed[index]);
            progress = true;
        }
    }
    for (size_t index = 0; index < devices.size(); ++index) {
        if (placed[index].ok)
            continue;
        // Beside nothing that can be drawn: shown on the first monitor.
        DeviceConfig lone = devices[index];
        lone.beside.clear();
        place_on_monitor(app, lone, placed[index]);
        placed[index].error = "it is beside " + devices[index].beside + ", which is not placed";
        placed[index].cross = {0, -1};
    }
    return placed;
}

// True when `index` sits beside `of`, directly or through others.
bool hangs_from(const App& app, size_t index, const size_t of) {
    const std::vector<DeviceConfig>& devices = app.config.devices;
    for (size_t steps = 0; steps <= devices.size(); ++steps) {
        if (devices[index].beside.empty())
            return false;
        index = find_device(app.config, devices[index].beside);
        if (index >= devices.size())
            return false;
        if (index == of)
            return true;
    }
    return true;
}

} // namespace

bool effective_placement(const App& app, const size_t device, int& start, int& length) {
    const std::vector<Placed> placed = place_all(app);
    if (device >= placed.size() || !placed[device].ok)
        return false;
    const DeviceConfig& config = app.config.devices[device];
    const int base = placed[device].on_monitor
                         ? (vertical(config.side) ? app.monitors[placed[device].host].rect.y
                                                  : app.monitors[placed[device].host].rect.x)
                         : 0;
    start = placed[device].anchor.lo - base;
    length = placed[device].anchor.length();
    return true;
}

void draw_layout(App& app) {
    if (app.monitors.empty()) {
        look::dim("No monitor could be found.");
        return;
    }
    std::vector<DeviceConfig>& devices = app.config.devices;
    std::vector<Placed> placed = place_all(app);

    // Everything that is drawn has to fit: the monitors and each device.
    Box bounds{1e9F, 1e9F, -1e9F, -1e9F};
    const auto include = [&](const Box& box) {
        bounds.x0 = std::min(bounds.x0, box.x0);
        bounds.y0 = std::min(bounds.y0, box.y0);
        bounds.x1 = std::max(bounds.x1, box.x1);
        bounds.y1 = std::max(bounds.y1, box.y1);
    };
    for (const Monitor& monitor : app.monitors)
        include(box_of(monitor.rect));
    // Room on every side, so a device can be dragged to any edge.
    const float room = 0.2F * std::max(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0);
    bounds = {bounds.x0 - room, bounds.y0 - room, bounds.x1 + room, bounds.y1 + room};
    for (const Placed& each : placed)
        include(each.box);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, look::px(300));
    const float pad = look::px(14);
    const float scale = std::min((size.x - 2 * pad) / (bounds.x1 - bounds.x0),
                                 (size.y - 2 * pad) / (bounds.y1 - bounds.y0));
    const ImVec2 shift(origin.x + (size.x - (bounds.x1 - bounds.x0) * scale) / 2,
                       origin.y + (size.y - (bounds.y1 - bounds.y0) * scale) / 2);
    const auto to_screen = [&](const float x, const float y) {
        return ImVec2(shift.x + (x - bounds.x0) * scale, shift.y + (y - bounds.y0) * scale);
    };

    ImGui::InvisibleButton("layout", size);
    const bool dragging = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), look::paper,
                        look::px(8));

    for (const Monitor& monitor : app.monitors) {
        const Box box = box_of(monitor.rect);
        const ImVec2 a = to_screen(box.x0, box.y0);
        const ImVec2 b = to_screen(box.x1, box.y1);
        draw->AddRectFilled(a, b, look::light, look::px(4));
        draw->AddRect(a, b, look::ink, look::px(4));
        const std::string pixels =
            std::to_string(monitor.rect.w) + " x " + std::to_string(monitor.rect.h);
        const ImVec2 name_size = ImGui::CalcTextSize(monitor.name.c_str());
        const ImVec2 pixels_size = ImGui::CalcTextSize(pixels.c_str());
        const ImVec2 centre((a.x + b.x) / 2, (a.y + b.y) / 2);
        draw->AddText(ImVec2(centre.x - name_size.x / 2, centre.y - name_size.y), look::ink,
                      monitor.name.c_str());
        draw->AddText(ImVec2(centre.x - pixels_size.x / 2, centre.y + look::px(2)), look::grey,
                      pixels.c_str());
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float px = (mouse.x - shift.x) / scale + bounds.x0;
    const float py = (mouse.y - shift.y) / scale + bounds.y0;
    if (ImGui::IsItemActivated() && !devices.empty()) {
        // A press on a device picks it and takes hold of it at that point;
        // anywhere else moves the chosen device with its middle at the mouse.
        bool on_one = false;
        for (size_t index = 0; index < placed.size(); ++index) {
            const Box& box = placed[index].box;
            if (px < box.x0 || px > box.x1 || py < box.y0 || py > box.y1)
                continue;
            app.device = index;
            app.computer = false;
            on_one = true;
        }
        if (has_selection(app)) {
            const Placed& chosen = placed[app.device];
            const float along = vertical(chosen.side) ? py : px;
            app.drag_offset = on_one
                                  ? static_cast<int>(along - edge_origin(chosen.box, chosen.side))
                                  : static_cast<int>(edge_length(chosen.box, chosen.side) / 2);
        }
    }

    const bool moving =
        dragging && has_selection(app) &&
        (ImGui::GetIO().MouseDelta.x != 0.0F || ImGui::GetIO().MouseDelta.y != 0.0F);
    if (moving) {
        // Follow the mouse to the nearest free edge: of a monitor, or of
        // another device. Along it the device goes wherever it is put, past
        // the corners too, as long as part of it still touches.
        DeviceConfig& moved = devices[app.device];
        const Placed& self = placed[app.device];
        const float held = static_cast<float>(app.drag_offset);
        const float own_length = edge_length(self.box, self.side);
        struct Target {
            bool on_monitor;
            size_t host;
            Side side;
            float start;
        };
        Target target{};
        float best = 1e30F;
        const auto offer = [&](const bool on_monitor, const size_t host, const Side side,
                               const Box& box, const float lo, const float hi) {
            const bool is_vertical = vertical(side);
            const float along = is_vertical ? py : px;
            const float across = (is_vertical ? px : py) - edge_at(box, side);
            const float off = std::max({0.0F, lo - along, along - hi});
            const float distance = std::hypot(across, off);
            if (distance >= best)
                return;
            best = distance;
            // On the edge it is already at, it stays held where it was
            // taken; on another it is held by its middle.
            const bool same =
                self.on_monitor == on_monitor && self.host == host && self.side == side;
            target = {on_monitor, host, side, along - (same ? held : own_length / 2)};
        };
        for (size_t index = 0; index < app.monitors.size(); ++index) {
            const Box box = box_of(app.monitors[index].rect);
            for (const Side side : {Side::left, Side::right, Side::top, Side::bottom}) {
                for (const Span& span : outer_spans(app.monitors, index, side))
                    offer(true, index, side, box, static_cast<float>(span.lo),
                          static_cast<float>(span.hi));
            }
        }
        for (size_t index = 0; index < placed.size(); ++index) {
            // Not itself, and not a device that hangs from it.
            if (index == app.device || hangs_from(app, index, app.device) ||
                devices[index].serial.empty())
                continue;
            const Box& box = placed[index].box;
            for (const Side side : {Side::left, Side::right, Side::top, Side::bottom}) {
                // The edge it is fastened by is taken.
                if (side == opposite(placed[index].side))
                    continue;
                offer(false, index, side, box, edge_origin(box, side),
                      edge_origin(box, side) + edge_length(box, side));
            }
        }
        if (best < 1e30F) {
            const bool kind_changed = target.on_monitor != moved.beside.empty();
            if (target.on_monitor) {
                const Rect& r = app.monitors[target.host].rect;
                const int edge_lo = vertical(target.side) ? r.y : r.x;
                const int edge_len = vertical(target.side) ? r.h : r.w;
                // Only the daemon's names are ones it will recognise.
                if (app.daemon_running)
                    moved.monitor = app.monitors[target.host].name;
                moved.beside.clear();
                // A length is in its host's pixels: it does not carry over.
                if (kind_changed)
                    moved.segment_length = -1;
                const int length =
                    kind_changed ? static_cast<int>(own_length) : self.anchor.length();
                moved.segment_start =
                    std::clamp(static_cast<int>(target.start) - edge_lo, 2 - length, edge_len - 2);
            } else {
                const Placed& host = placed[target.host];
                const bool same_host = !self.on_monitor && self.host == target.host;
                moved.beside = devices[target.host].serial;
                if (!same_host)
                    moved.segment_length = -1;
                const float in_host = (target.start - edge_origin(host.box, target.side)) /
                                      static_cast<float>(host.scale);
                const int host_len = vertical(target.side) ? host.view.h : host.view.w;
                const int length =
                    same_host ? self.anchor.length() : static_cast<int>(own_length / host.scale);
                moved.segment_start =
                    std::clamp(static_cast<int>(in_host), 2 - length, host_len - 2);
            }
            moved.side = target.side;
            edited_directly(app, moved.serial, "placement");
            placed = place_all(app);
        }
    }
    if (hovered && has_selection(app) && ImGui::GetIO().KeyCtrl &&
        ImGui::GetIO().MouseWheel != 0.0F) {
        // Ctrl and the wheel make the device longer or shorter against its host,
        // about its middle: how much of the edge maps onto it.
        DeviceConfig& moved = devices[app.device];
        const Span anchor = placed[app.device].anchor;
        const int length =
            std::clamp(static_cast<int>(std::lround(static_cast<float>(anchor.length()) *
                                                    std::pow(1.06F, ImGui::GetIO().MouseWheel))),
                       20, 20000);
        const int base = placed[app.device].on_monitor
                             ? (vertical(moved.side) ? app.monitors[placed[app.device].host].rect.y
                                                     : app.monitors[placed[app.device].host].rect.x)
                             : 0;
        moved.segment_start = anchor.lo + (anchor.length() - length) / 2 - base;
        moved.segment_length = length;
        edited_directly(app, moved.serial, "placement");
        placed = place_all(app);
    }

    for (size_t index = 0; index < placed.size(); ++index) {
        const Placed& each = placed[index];
        const bool selected = has_selection(app) && index == app.device;
        const bool off = !devices[index].enabled;
        const ImVec2 a = to_screen(each.box.x0, each.box.y0);
        const ImVec2 b = to_screen(each.box.x1, each.box.y1);
        const ImU32 line = !each.error.empty() ? look::ink
                           : selected          ? look::ink
                                               : look::ink;
        draw->AddRectFilled(a, b, selected ? look::light : look::paper, look::px(5));
        draw->AddRect(a, b, line, look::px(5), 0, selected ? look::px(2) : 1.0F);
        const std::string& name = label_of(devices[index]);
        look::draw_text_ellipsized(draw,
                                 ImVec2(a.x + look::px(6), (a.y + b.y - ImGui::GetFontSize()) / 2),
                                 off        ? look::grey
                                 : selected ? look::ink
                                            : look::grey,
                                 name.c_str(), b.x - a.x - look::px(12));

        // The part of the edge the cursor crosses at.
        if (each.cross.hi >= each.cross.lo) {
            const bool is_vertical = vertical(each.side);
            const Box host =
                each.on_monitor ? box_of(app.monitors[each.host].rect) : placed[each.host].box;
            const float unit = each.on_monitor ? 1.0F : static_cast<float>(placed[each.host].scale);
            const float base = each.on_monitor ? 0.0F : edge_origin(host, each.side);
            const float fixed = edge_at(host, each.side);
            const float lo = base + static_cast<float>(each.cross.lo) * unit;
            const float hi = base + static_cast<float>(each.cross.hi + 1) * unit;
            draw->AddLine(is_vertical ? to_screen(fixed, lo) : to_screen(lo, fixed),
                          is_vertical ? to_screen(fixed, hi) : to_screen(hi, fixed),
                          selected ? look::ink : look::grey,
                          selected ? look::px(3) : look::px(2));
        }
    }
    if (hovered || dragging)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (!has_selection(app))
        return;
    const Placed& chosen = placed[app.device];
    if (!chosen.error.empty())
        ImGui::Text("ERR: %s", chosen.error.c_str());
    else if (!app.daemon_running)
        look::dim("Monitors as this window sees them. Start the daemon for its own.");
    else if (!chosen.sized)
        look::dim("This device's size is not known yet, so its shape is a guess.");
    else
        look::dim("Drag a device to a monitor's edge or another device's. Ctrl and scroll to resize "
                     "it. The bright line is where the cursor crosses.");
}

} // namespace gui
