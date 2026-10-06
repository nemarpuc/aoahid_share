// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/accel_model.hpp"
#include "aoahid_share/geometry.hpp"

namespace aoas {

struct Interval {
    double lo{};
    double hi{};
    [[nodiscard]] double mid() const noexcept { return (lo + hi) / 2.0; }
    [[nodiscard]] double width() const noexcept { return hi - lo; }
};

// Where Android's cursor can be, in view space. Android clamps the cursor to
// its display, so pushing it against an edge collapses the range there.
class Tracker {
  public:
    // Forgets the position: the cursor can be anywhere on the display.
    void reset(Size view) noexcept;
    // Applies a report of view-space counts that Android scaled by a gain in g.
    void sent(Delta counts, GainRange g) noexcept;

    [[nodiscard]] const Interval& x() const noexcept { return x_; }
    [[nodiscard]] const Interval& y() const noexcept { return y_; }
    [[nodiscard]] Size size() const noexcept { return size_; }

  private:
    Size size_{};
    Interval x_{};
    Interval y_{};
};

} // namespace aoas
