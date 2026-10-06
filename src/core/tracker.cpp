// SPDX-License-Identifier: MIT
#include "aoahid_share/tracker.hpp"

#include <algorithm>

namespace aoas {
namespace {

void advance(Interval& range, const int counts, const GainRange g, const int limit) noexcept {
    const double a = g.lo * counts;
    const double b = g.hi * counts;
    const double top = static_cast<double>(limit);
    range.lo = std::clamp(range.lo + std::min(a, b), 0.0, top);
    range.hi = std::clamp(range.hi + std::max(a, b), 0.0, top);
}

} // namespace

void Tracker::reset(const Size view) noexcept {
    size_ = view;
    x_ = {0.0, static_cast<double>(view.w - 1)};
    y_ = {0.0, static_cast<double>(view.h - 1)};
}

void Tracker::sent(const Delta counts, const GainRange g) noexcept {
    advance(x_, counts.x, g, size_.w - 1);
    advance(y_, counts.y, g, size_.h - 1);
}

} // namespace aoas
