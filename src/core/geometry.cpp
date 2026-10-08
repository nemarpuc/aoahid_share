// SPDX-License-Identifier: MIT
#include "aoahid_share/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace aoas {
namespace {

Span edge_span(const Rect& rect, const Side side) noexcept {
    return vertical(side) ? Span{rect.y, rect.y + rect.h - 1} : Span{rect.x, rect.x + rect.w - 1};
}

bool touches(const Rect& self, const Rect& other, const Side side) noexcept {
    switch (side) {
    case Side::left:
        return other.x + other.w == self.x;
    case Side::right:
        return other.x == self.x + self.w;
    case Side::top:
        return other.y + other.h == self.y;
    case Side::bottom:
        return other.y == self.y + self.h;
    }
    return false;
}

} // namespace

bool vertical(const Side side) noexcept { return side == Side::left || side == Side::right; }

std::vector<Span> outer_spans(const std::vector<Monitor>& monitors, const size_t index,
                              const Side side) {
    std::vector<Span> result;
    if (index >= monitors.size())
        return result;
    const Rect& self = monitors[index].rect;
    const Span edge = edge_span(self, side);

    std::vector<Span> covered;
    for (size_t other = 0; other < monitors.size(); ++other) {
        if (other == index || !touches(self, monitors[other].rect, side))
            continue;
        const Span span = edge_span(monitors[other].rect, side);
        const Span overlap{std::max(span.lo, edge.lo), std::min(span.hi, edge.hi)};
        if (overlap.lo <= overlap.hi)
            covered.push_back(overlap);
    }
    std::sort(covered.begin(), covered.end(),
              [](const Span& a, const Span& b) { return a.lo < b.lo; });

    int next = edge.lo;
    for (const Span& span : covered) {
        if (span.lo > next)
            result.push_back({next, span.lo - 1});
        next = std::max(next, span.hi + 1);
    }
    if (next <= edge.hi)
        result.push_back({next, edge.hi});
    return result;
}

Size view_size(const Size natural, const int rotation, const int mount) noexcept {
    Size size = natural;
    if (rotation == 90 || rotation == 270)
        std::swap(size.w, size.h);
    if (mount == 90 || mount == 270)
        std::swap(size.w, size.h);
    return size;
}

Delta to_device_delta(const int mount, const Delta view) noexcept {
    switch (mount) {
    case 90:
        return {view.y, -view.x};
    case 180:
        return {-view.x, -view.y};
    case 270:
        return {-view.y, view.x};
    default:
        return view;
    }
}

TouchPoint to_touch_point(const Size natural, const int rotation, const int mount,
                          const double view_x, const double view_y) noexcept {
    const Size view = view_size(natural, rotation, mount);
    // View space to display space: undo the mount.
    double dx = view_x;
    double dy = view_y;
    switch (mount) {
    case 90:
        dx = view_y;
        dy = view.w - 1.0 - view_x;
        break;
    case 180:
        dx = view.w - 1.0 - view_x;
        dy = view.h - 1.0 - view_y;
        break;
    case 270:
        dx = view.h - 1.0 - view_y;
        dy = view_x;
        break;
    default:
        break;
    }
    // Display space to the natural orientation: undo the display rotation.
    double rx = dx;
    double ry = dy;
    switch (rotation) {
    case 90:
        rx = natural.w - 1.0 - dy;
        ry = dx;
        break;
    case 180:
        rx = natural.w - 1.0 - dx;
        ry = natural.h - 1.0 - dy;
        break;
    case 270:
        rx = dy;
        ry = natural.h - 1.0 - dx;
        break;
    default:
        break;
    }
    return {static_cast<int>(std::clamp(std::lround(rx), 0L, static_cast<long>(natural.w) - 1)),
            static_cast<int>(std::clamp(std::lround(ry), 0L, static_cast<long>(natural.h) - 1))};
}

double monitor_density(const Monitor& monitor, const bool along_x) noexcept {
    const double mm = along_x ? monitor.width_mm : monitor.height_mm;
    const int pixels = along_x ? monitor.rect.w : monitor.rect.h;
    return mm > 0.0 ? pixels / mm : 0.0;
}

double Portal::to_android(const int t) const noexcept {
    const double u =
        static_cast<double>(t - anchor.lo) * (android_length - 1) / (anchor.length() - 1);
    return std::clamp(u, 0.0, static_cast<double>(android_length - 1));
}

int Portal::to_pc(const double u) const noexcept {
    const double t = anchor.lo + u * (anchor.length() - 1) / (android_length - 1);
    return std::clamp(static_cast<int>(std::lround(t)), segment.lo, segment.hi);
}

std::string resolve_portal(const std::vector<Monitor>& monitors, const PortalRequest& request,
                           Portal& out) {
    if (request.monitor >= monitors.size())
        return "the monitor does not exist";
    if (request.anchor_length < 2)
        return "the Android rectangle must be at least 2 pixels long on the PC side";
    if (request.android_length < 2)
        return "the Android display must be at least 2 pixels long";
    if (request.corner_margin < 0.0 || request.corner_margin >= 0.5)
        return "the corner margin must be at least 0 and below 50%";

    const Span edge = edge_span(monitors[request.monitor].rect, request.side);
    const int start = request.centered ? edge.lo + (edge.length() - request.anchor_length) / 2
                                       : request.anchor_start;
    const Span anchor{start, start + request.anchor_length - 1};
    const int margin = static_cast<int>(std::lround(request.corner_margin * edge.length()));

    Span best{0, -1};
    for (const Span& outer : outer_spans(monitors, request.monitor, request.side)) {
        const Span part{std::max({outer.lo, anchor.lo, edge.lo + margin}),
                        std::min({outer.hi, anchor.hi, edge.hi - margin})};
        if (part.length() > best.length())
            best = part;
    }
    if (best.length() < 2)
        return "the Android rectangle does not touch an outer edge of that monitor";

    out.monitor = request.monitor;
    out.side = request.side;
    out.anchor = anchor;
    out.segment = best;
    out.android_length = request.android_length;
    return {};
}

} // namespace aoas
