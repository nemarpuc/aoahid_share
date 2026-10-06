// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/config.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aoas {

// `wm size` output to the size in effect ("Override size" wins over
// "Physical size"). False when neither line is there.
[[nodiscard]] bool parse_wm_size(std::string_view text, int& width, int& height) noexcept;

// The "Physical size" line of `wm size` alone: the panel's pixels, which the
// physical dpi of `dumpsys display` belongs to. False when it is absent.
[[nodiscard]] bool parse_wm_physical_size(std::string_view text, int& width, int& height) noexcept;

// `wm density` output to the density in effect ("Override density" wins).
[[nodiscard]] bool parse_wm_density(std::string_view text, int& density) noexcept;

// The first "<x> x <y> dpi" pair of `dumpsys display`. False when absent.
[[nodiscard]] bool parse_display_dpi(std::string_view text, double& x, double& y) noexcept;

// The display rotation in degrees from a line holding
// "mCurrentOrientation=N", or -1.
[[nodiscard]] int parse_rotation(std::string_view text) noexcept;

// What the phone reported.
struct PhoneFacts {
    // False when adb could not talk to the phone; note says why.
    bool reachable{};
    std::string note;

    // What the phone calls itself (ro.product.model); empty when unknown.
    std::string model;
    // The size in effect (an override wins) and the panel's own.
    int width{};
    int height{};
    int physical_width{};
    int physical_height{};
    double dpi_x{};
    double dpi_y{};
    // The density Android lays out with (`wm density`); 0 when unknown.
    int density{};
    int rotation{-1};
    bool has_pointer_speed{};
    int pointer_speed{};

    // The settings key that switches pointer acceleration on this phone
    // (AOSP's, or the vendor's own), and its value as found: "0", "1", or
    // empty when it was not set. Empty key: this phone has no such switch.
    std::string accel_key;
    std::string accel_before;
    // True when the setting reads as off.
    bool accel_off{};
    // True when the gain with acceleration off has been measured for this
    // kind of switch (docs/MATH.md).
    bool accel_gain_known{};
};

// Writes into a device's settings what the phone said: the resolution, the
// rotation, the diagonal (from the panel's pixels and the physical dpi of
// each axis), the pointer speed, whether
// pointer acceleration is on, and the gain at low speed (docs/MATH.md,
// "Gain"). A value the phone did not give is left as it was and is named in
// the result; nothing is made up.
[[nodiscard]] std::vector<std::string> fill_device(const PhoneFacts& facts, DeviceConfig& device);

class Adb {
  public:
    // An empty path looks adb up on PATH.
    explicit Adb(std::string path);

    // True when the program runs at all.
    [[nodiscard]] bool available() const;
    void kill_server() const;
    // `adb connect` to the proxy, then waits until the phone is usable (the
    // permission dialog included). `cancel` ends the wait early when set.
    // `created` is set when this call made the connection; false when adb
    // already had it (the user's own `adb connect`), which is then not ours to
    // end. A connection that never became usable is ended here, so it does
    // not stay half open for the next try.
    [[nodiscard]] bool connect(uint16_t port, std::string& note,
                               const std::atomic<bool>* cancel = nullptr,
                               bool* created = nullptr) const;
    void disconnect(uint16_t port) const;

    // Reads the display facts and the pointer settings. Nothing on the phone
    // is changed.
    [[nodiscard]] PhoneFacts inspect(uint16_t port) const;

  private:
    [[nodiscard]] std::string shell(uint16_t port, const std::string& command, bool& ok) const;

    std::string path_;
};

} // namespace aoas
