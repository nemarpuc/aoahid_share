// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/geometry.hpp"
#include "aoahid_share/keymap.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aoas {

enum class AccelSetting { automatic, off, on };

// segment_start when the device is to be centred on its edge. Any other
// value counts from the start of the edge and may be negative: the device
// then reaches past the corner.
inline constexpr int centred_start = std::numeric_limits<int>::min();

// More devices than this are refused wherever a config is read.
inline constexpr size_t max_devices = 64;

// How the cursor enters a device. Parked: while the input is on the PC the
// device's cursor waits in its bottom right corner, which is known exactly, and
// enters from there. Aligned: it is left where it was and moved to the new
// height. Corner: every entry starts with the corner reference. (docs/MATH.md,
// "Entering")
enum class EntryMode { parked, aligned, corner };

// How the cursor moves and crosses. config.ini's [motion] holds one for
// every device; a device's own file may replace any part of it.
struct Motion {
    double sensitivity{1.0};
    // 0 follows sensitivity.
    double sensitivity_y{};
    double scroll_sensitivity{1.0};
    EntryMode entry{EntryMode::parked};
    double resync_width{48.0};
    double enter_push{};
    // Negative selects the mode's value.
    double return_push{-1.0};
    bool no_cross_while_button{true};
};

// How often a phone is sent reports: `hz` reports a second while only motion
// is pending (0: as fast as the previous one completes), or, with `every`,
// each movement and scroll step as a report of its own.
struct ReportRate {
    unsigned hz{};
    bool every{};
};

// The parts of Motion a device's file sets for itself, and its own report rate.
struct MotionPatch {
    std::optional<double> sensitivity;
    std::optional<double> sensitivity_y;
    std::optional<double> scroll_sensitivity;
    std::optional<EntryMode> entry;
    std::optional<double> resync_width;
    std::optional<double> enter_push;
    std::optional<double> return_push;
    std::optional<bool> no_cross_while_button;
    std::optional<ReportRate> report_rate;
};

// Touchscreen mode (docs/CONFIG.md, "[touch]").
struct Touch {
    bool enabled{};
    // The wheel swipes a finger. Off leaves it a mouse wheel.
    bool swipe{};
    // Pixels a wheel notch moves the finger; 0 leaves that wheel a mouse wheel.
    int scroll{100};
    int scroll_pan{100};
    // From the last notch to lifting the finger.
    int release_ms{200};
    // From the swiping finger being put down to its move.
    double start_ms{1.0};
    // The moves a notch is divided into, the time from the first to the last
    // of them, and what a notch does while a swipe goes on.
    int steps{4};
    double total_ms{8.0};
    bool restart{};
    // The mouse button that becomes a tap (1-based).
    unsigned button{1};
};

struct TouchPatch {
    std::optional<bool> enabled;
    std::optional<bool> swipe;
    std::optional<int> scroll;
    std::optional<int> scroll_pan;
    std::optional<int> release_ms;
    std::optional<double> start_ms;
    std::optional<int> steps;
    std::optional<double> total_ms;
    std::optional<bool> restart;
    std::optional<unsigned> button;
};

// One device, identified by its USB serial: one file under device/. A value
// the file does not give is stored as its "unset" value here (-1 for the
// integers, 0 for the sizes); the daemon then says what is missing and does
// not guess it.
struct DeviceConfig {
    // Empty only for a profile that names no device;
    // the next device that is connected by hand takes it.
    std::string serial;
    // What it is called everywhere; the serial when empty.
    std::string name;
    // False: connected or not, it is left alone.
    bool enabled{true};
    // Moves the input straight to this device.
    std::string hotkey;

    // Where it sits: on a side of a monitor (empty: the first), or of
    // another device, named by its serial.
    std::string monitor;
    std::string beside;
    Side side{Side::right};
    int segment_start{centred_start};
    int segment_length{-1};
    double corner_margin{0.05};
    double monitor_diagonal_inch{};
    int rotation{-1};
    int mount_rotation{};

    // What "fill" read from the device; any of it may be set by hand.
    int width{};
    int height{};
    double diagonal_inch{};
    int pointer_speed{};
    // The density Android lays out with (`wm density`), in dpi; 0 when not
    // read. With the pointer speed it gives the gain at low speed.
    int density_dpi{};
    AccelSetting accel{AccelSetting::automatic};
    // Pixels the cursor moves per count at low speed, when it is typed or
    // measured; 0 follows from the pointer speed and the density.
    double gain{};
    // The day it was read, as text; empty when it never was.
    std::string read_at;

    MotionPatch motion;
    std::optional<bool> mouse;
    std::optional<bool> keyboard;
    std::optional<bool> media;
    TouchPatch touch;
    // 0: the next free port from [adb] first_port, in the devices' order.
    uint16_t adb_port{};
    // Serve its ADB interface on adb_port (see docs/CONFIG.md). Off until it
    // is switched on by hand.
    bool adb_proxy{};
};

struct Config {
    std::string backend{"auto"};
    std::string toggle_hotkey{"ctrl+alt+s"};
    std::string panic_hotkey{"ctrl+alt+shift+escape"};
    std::string pause_hotkey;
    std::string resync_hotkey;

    unsigned mouse_buttons{5};
    // HID button for PC button 1, 2, ...; empty keeps them in order.
    std::vector<unsigned> button_map;
    bool mouse{true};
    bool keyboard{true};
    bool media{true};
    Touch touch;
    // A hotkey per entry of media_keys, empty for none.
    std::array<std::string, media_key_count> media_hotkeys;
    // The device media keys go to, by name or serial. Empty: the one that
    // has the input, or else the one that had it last.
    std::string media_target;

    Motion motion;
    // 0 sends each report as soon as the previous one completes.
    unsigned report_rate_hz{};
    // Every movement is a report of its own: nothing is summed or paced.
    bool report_every{};

    // Stop a running adb server that holds a phone's ADB interface, when a
    // device's proxy is to be started and cannot be.
    bool adb_kill_server{true};
    // The adb program, for "fill"; empty looks it up on PATH.
    std::string adb_path;
    uint16_t adb_first_port{6555};

    std::vector<DeviceConfig> devices;
};

// What a serial and a name may be. A serial names a file and a status key:
// letters, digits and `- _ .`, at most 64, not starting with a dot, and
// not a name the file system keeps for itself. A name is shown and typed:
// at most 48 bytes without `=`, `;` or `#`, which the file's
// syntax uses. Both come from outside (a USB
// descriptor, what a device calls itself), so they are checked wherever
// they enter.
[[nodiscard]] bool valid_serial(std::string_view text) noexcept;
[[nodiscard]] bool valid_name(std::string_view text) noexcept;
// A name made valid: what is not allowed is dropped, the rest cut to length.
[[nodiscard]] std::string clean_name(std::string_view text);

// config.ini: everything but the devices. Empty on success, else
// "line N: what is wrong"; out is unchanged on failure. It also reads an older
// config with its [device.N] sections, which become devices.
[[nodiscard]] std::string parse_globals(std::string_view text, Config& out);
[[nodiscard]] std::string format_globals(const Config& config);

// One file under devices/.
[[nodiscard]] std::string parse_device(std::string_view text, DeviceConfig& out);
[[nodiscard]] std::string format_device(const DeviceConfig& device);

// Everything in one text, for export and import: the globals, then each
// device after a line of its own reading `--- device ---`.
[[nodiscard]] std::string parse_config(std::string_view text, Config& out);
[[nodiscard]] std::string format_config(const Config& config);

// What cannot be seen in one file alone: two devices with one serial, name,
// port or hotkey, a device beside itself or beside one that is not there.
// Empty when all is well.
[[nodiscard]] std::string validate_config(const Config& config);

// Pixels one count moves the device's cursor at low speed: the typed gain,
// else the one the pointer speed and the density give; 0 when neither is
// known.
[[nodiscard]] double effective_gain(const DeviceConfig& device) noexcept;

// Whether a device has movement settings of its own, and a patch that holds
// every one of these values (what a device that takes its own starts from).
[[nodiscard]] bool has_own_motion(const MotionPatch& patch) noexcept;
[[nodiscard]] MotionPatch motion_patch_of(const Motion& motion) noexcept;

// How the device is paced: its own file's rate, else the computer's.
[[nodiscard]] ReportRate report_rate_of(const Config& config, const DeviceConfig& device) noexcept;

// The values a device runs with: the globals, with its own file's parts.
[[nodiscard]] Motion motion_of(const Config& config, const DeviceConfig& device) noexcept;
[[nodiscard]] bool mouse_of(const Config& config, const DeviceConfig& device) noexcept;
[[nodiscard]] bool keyboard_of(const Config& config, const DeviceConfig& device) noexcept;
[[nodiscard]] bool media_of(const Config& config, const DeviceConfig& device) noexcept;
[[nodiscard]] Touch touch_of(const Config& config, const DeviceConfig& device) noexcept;
[[nodiscard]] uint16_t adb_port_of(const Config& config, size_t index) noexcept;
// Its name, or its serial when it has none.
[[nodiscard]] const std::string& label_of(const DeviceConfig& device) noexcept;
// The device a name or a serial means, or the count of devices when none.
[[nodiscard]] size_t find_device(const Config& config, std::string_view name_or_serial) noexcept;

// For a config that came from somewhere else: the [adb] section, and each
// device's proxy switch and port, are taken from this computer's. They decide
// which program the daemon runs, whether it stops the adb server and which
// interface it serves, none of which a file from elsewhere gets to choose.
// True if that changed anything.
bool keep_local_adb(Config& imported, const Config& local);

} // namespace aoas
