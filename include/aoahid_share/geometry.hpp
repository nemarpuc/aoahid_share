// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace aoas {

enum class Side { left, right, top, bottom };

struct Rect {
    int x{};
    int y{};
    int w{};
    int h{};
};

// One PC monitor in the compositor's (logical) pixel coordinates.
struct Monitor {
    std::string name;
    Rect rect;
    double width_mm{};
    double height_mm{};
};

// Inclusive range along an edge, in PC pixels.
struct Span {
    int lo{};
    int hi{};
    [[nodiscard]] int length() const noexcept { return hi - lo + 1; }
};

struct Size {
    int w{};
    int h{};
};

struct Delta {
    int x{};
    int y{};
};

[[nodiscard]] bool vertical(Side side) noexcept;

// The side across from it.
[[nodiscard]] constexpr Side opposite(const Side side) noexcept {
    switch (side) {
    case Side::left:
        return Side::right;
    case Side::right:
        return Side::left;
    case Side::top:
        return Side::bottom;
    case Side::bottom:
        return Side::top;
    }
    return side;
}

// The parts of one monitor edge no other monitor touches (docs/MATH.md,
// "Several monitors"). Ordered, non-overlapping; empty when fully covered.
[[nodiscard]] std::vector<Span> outer_spans(const std::vector<Monitor>& monitors, size_t index,
                                            Side side);

// Size of the Android display as the user sees it. rotation is Android's
// display rotation, mount is how far the content appears rotated clockwise.
[[nodiscard]] Size view_size(Size natural, int rotation, int mount) noexcept;

// A delta in view space to the delta Android has to receive for it.
[[nodiscard]] Delta to_device_delta(int mount, Delta view) noexcept;

struct TouchPoint {
    int x{};
    int y{};
};

// A point of view space (rotation and mount both applied, as the user sees the
// content) to the raw coordinates of a touchscreen in the device's natural
// orientation. The mount is undone first, then Android's display rotation.
// Rounded and clamped to the device (docs/MATH.md, "Touch").
[[nodiscard]] TouchPoint to_touch_point(Size natural, int rotation, int mount, double view_x,
                                        double view_y) noexcept;

// Pixels per millimetre along the given axis; 0 when the size is unknown.
[[nodiscard]] double monitor_density(const Monitor& monitor, bool along_x) noexcept;

// Where the cursor crosses: a span on one monitor edge, and the Android
// coordinate range it maps onto.
struct Portal {
    size_t monitor{};
    Side side{Side::right};
    // The Android rectangle's extent along the edge, in PC pixels.
    Span anchor;
    // The part of anchor the cursor can actually cross at.
    Span segment;
    // Length of the Android side that touches the edge, in Android pixels.
    int android_length{};

    // PC coordinate along the edge to Android coordinate along it, and back.
    [[nodiscard]] double to_android(int t) const noexcept;
    [[nodiscard]] int to_pc(double u) const noexcept;
};

struct PortalRequest {
    size_t monitor{};
    Side side{Side::right};
    // Centre the Android rectangle on the edge, ignoring anchor_start.
    bool centered{true};
    // Otherwise, where it starts along the edge, in PC coordinates.
    int anchor_start{};
    // Its length in PC pixels; must be at least 2.
    int anchor_length{};
    // Excluded at both ends of the monitor edge, as a fraction of its length.
    double corner_margin{0.05};
    int android_length{};
};

// Empty error on success. The segment is the longest overlap of the anchor
// with the edge's outer spans, minus the corner margin.
[[nodiscard]] std::string resolve_portal(const std::vector<Monitor>& monitors,
                                         const PortalRequest& request, Portal& out);

} // namespace aoas
