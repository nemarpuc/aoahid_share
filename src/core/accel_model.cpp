// SPDX-License-Identifier: MIT
#include "aoahid_share/accel_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aoas {
namespace {

struct Segment {
    double max_speed;
    double base_gain;
    double reciprocal;
};

constexpr Segment segments[] = {
    {32.002, 3.19, 0.0},
    {52.83, 4.79, -51.254},
    {119.124, 7.28, -182.737},
    {std::numeric_limits<double>::infinity(), 15.04, -1107.556},
};

constexpr double sensitivity_factors[] = {1, 2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 18, 20};

// AOSP assumes an 800 CPI mouse when it turns counts into millimetres.
constexpr double counts_per_mm = 800.0 / 25.4;

// Android measures the speed itself; this is how far its figure may be from
// the one computed here from the send times.
constexpr double speed_tolerance = 2.0;

// A longer gap than this starts a new movement for Android's velocity tracker.
constexpr double movement_gap_seconds = 0.1;

double common_factor(const int pointer_speed) noexcept {
    const int index = std::clamp(pointer_speed, -7, 7) + 7;
    return 0.64 * sensitivity_factors[index] / 10.0;
}

} // namespace

double curve_gain(const int pointer_speed, const double speed_mm_per_s) noexcept {
    const double factor = common_factor(pointer_speed);
    if (speed_mm_per_s <= 0.0)
        return factor * segments[0].base_gain;
    for (const Segment& segment : segments) {
        if (speed_mm_per_s <= segment.max_speed)
            return factor * (segment.base_gain + segment.reciprocal / speed_mm_per_s);
    }
    return factor * segments[3].base_gain;
}

double pointer_speed_factor(const int pointer_speed) noexcept {
    return common_factor(pointer_speed);
}

double low_speed_gain(const int pointer_speed, const int density_dpi) noexcept {
    return density_dpi > 0 ? density_dpi / density_baseline * curve_gain(pointer_speed, 0.0) : 0.0;
}

AccelModel::AccelModel(const AccelMode mode, const int pointer_speed, const double gain_min,
                       const double gain_max, const double density_scale,
                       const double exact_gain) noexcept
    : mode_(mode), pointer_speed_(pointer_speed), gain_min_(gain_min), gain_max_(gain_max),
      density_scale_(density_scale), exact_gain_(exact_gain) {}

GainRange AccelModel::range(const double counts, const double seconds) const noexcept {
    switch (mode_) {
    case AccelMode::exact:
        return {exact_gain_, exact_gain_};
    case AccelMode::unknown:
        return {gain_min_, gain_max_};
    case AccelMode::curve:
        break;
    }
    const double lowest = density_scale_ * curve_gain(pointer_speed_, 0.0);
    if (seconds <= 0.0 || seconds > movement_gap_seconds) {
        // No velocity yet on Android's side either, unless it kept one from
        // before; allow for that with the fastest gain the curve reaches.
        return {lowest, density_scale_ * common_factor(pointer_speed_) * segments[3].base_gain};
    }
    const double speed = std::fabs(counts) / seconds / counts_per_mm;
    // The curve dips by a hair where its segments meet, hence the min/max.
    const double slow = density_scale_ * curve_gain(pointer_speed_, speed / speed_tolerance);
    const double fast = density_scale_ * curve_gain(pointer_speed_, speed * speed_tolerance);
    return {std::min({lowest, slow, fast}), std::max(slow, fast)};
}

double AccelModel::floor() const noexcept {
    switch (mode_) {
    case AccelMode::exact:
        return exact_gain_;
    case AccelMode::unknown:
        return gain_min_;
    case AccelMode::curve:
        break;
    }
    // Just under the base gain: the first segment boundary dips below it.
    return density_scale_ * curve_gain(pointer_speed_, 0.0) * 0.999;
}

double AccelModel::slow_gain() const noexcept {
    switch (mode_) {
    case AccelMode::exact:
        return exact_gain_;
    case AccelMode::unknown:
        // Nothing is known about the phone; counts are sent as they come.
        return 1.0;
    case AccelMode::curve:
        break;
    }
    return density_scale_ * curve_gain(pointer_speed_, 0.0);
}

} // namespace aoas
