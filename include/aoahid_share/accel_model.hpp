// SPDX-License-Identifier: MIT
#pragma once

namespace aoas {

// How much is known about Android's pointer acceleration (docs/MATH.md).
enum class AccelMode {
    exact,  // acceleration is off: every count moves the same known distance
    curve,  // acceleration is on and follows AOSP's curve for pointer_speed
    unknown // only the configured bounds are known
};

// Android scales mouse movement by the display density over this baseline.
inline constexpr double density_baseline = 320.0;

struct GainRange {
    double lo{1.0};
    double hi{1.0};
};

// AOSP's mouse acceleration gain at a pointer speed in millimetres per second
// (frameworks/native AccelerationCurve.cpp, VelocityControl.cpp). pointer_speed
// is Android's setting, -7..7. A speed of zero or less means "not known yet".
[[nodiscard]] double curve_gain(int pointer_speed, double speed_mm_per_s) noexcept;

// What the pointer speed setting multiplies the curve by: 0.64 * S[speed] / 10
// (docs/MATH.md, "Gain"), and the gain of the curve's first segment.
[[nodiscard]] double pointer_speed_factor(int pointer_speed) noexcept;
inline constexpr double first_segment_gain = 3.19;

// Pixels one count moves Android's cursor at low speed: the first segment of
// the curve at that pointer speed, scaled by the density (docs/MATH.md,
// "Gain"). 0 when the density is not known.
[[nodiscard]] double low_speed_gain(int pointer_speed, int density_dpi) noexcept;

class AccelModel {
  public:
    AccelModel() = default;
    // density_scale multiplies the curve (display density over
    // density_baseline). exact_gain is the pixels one count moves in the
    // exact mode.
    AccelModel(AccelMode mode, int pointer_speed, double gain_min, double gain_max,
               double density_scale = 1.0, double exact_gain = 1.0) noexcept;

    [[nodiscard]] AccelMode mode() const noexcept { return mode_; }
    // The gain Android may apply to a report of this many counts, sent this
    // many seconds after the previous one (zero or less when there was none).
    [[nodiscard]] GainRange range(double counts, double seconds) const noexcept;
    // The smallest gain range() can return, for sizing the corner move.
    [[nodiscard]] double floor() const noexcept;
    // Pixels one count moves at low speed, for turning a wanted distance
    // into counts.
    [[nodiscard]] double slow_gain() const noexcept;

  private:
    AccelMode mode_{AccelMode::exact};
    int pointer_speed_{};
    double gain_min_{1.0};
    double gain_max_{1.0};
    double density_scale_{1.0};
    double exact_gain_{1.0};
};

} // namespace aoas
