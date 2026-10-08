// SPDX-License-Identifier: MIT
#include "aoahid_share/keys.hpp"

#include "aoahid_share/ini_edit.hpp"

namespace aoas {
namespace {

using S = KeyScope;
using K = KeyKind;

constexpr ConfigKey table[] = {
    // config.ini [daemon]
    {S::computer, "daemon", "backend", "General", K::choice,
     "auto|x11|portal|layer_shell|evdev|windows|macos", "", "auto",
     "How input is captured; auto picks the one this desktop can use."},
    {S::computer, "daemon", "toggle_hotkey", "General", K::hotkey, "", "", "ctrl+alt+s",
     "Moves the input to the last used device and back."},
    {S::computer, "daemon", "panic_hotkey", "General", K::hotkey, "", "", "ctrl+alt+shift+escape",
     "Always brings the input back to this computer."},
    {S::computer, "daemon", "pause_hotkey", "General", K::hotkey, "", "", "",
     "Stops and restarts crossing; empty turns it off."},
    {S::computer, "daemon", "resync_hotkey", "General", K::hotkey, "", "", "",
     "Takes the corner reference on the next crossing; empty turns it off."},

    // [mouse] and [keyboard]
    {S::both, "mouse", "enabled", "Mouse and keyboard", K::boolean, "", "", "true",
     "false does not register the mouse on the device."},
    {S::computer, "mouse", "buttons", "Mouse and keyboard", K::integer, "1..8", "", "5",
     "How many mouse buttons are sent; `aoahid_share buttons` reports what your mice have."},
    {S::computer, "mouse", "button_map", "Mouse and keyboard", K::list, "1..8, comma-separated", "",
     "",
     "HID button for PC button 1, 2, ...; empty keeps them in order, 1,3,2 swaps right and "
     "middle."},
    {S::both, "keyboard", "enabled", "Mouse and keyboard", K::boolean, "", "", "true",
     "false does not register the keyboard on the device."},

    // [media]
    {S::both, "media", "enabled", "Mouse and keyboard", K::boolean, "", "", "true",
     "false does not register the media keys on the device."},
    {S::computer, "media", "target", "Media", K::text, "", "", "active",
     "The device that receives media keys, by name or serial; active means the one that has the "
     "input."},
    {S::computer, "media", "previous", "Media", K::hotkey, "", "", "",
     "Hotkey for the previous-track key; empty turns it off."},
    {S::computer, "media", "play_pause", "Media", K::hotkey, "", "", "",
     "Hotkey for the play/pause key; empty turns it off."},
    {S::computer, "media", "next", "Media", K::hotkey, "", "", "",
     "Hotkey for the next-track key; empty turns it off."},
    {S::computer, "media", "brightness_up", "Media", K::hotkey, "", "", "",
     "Hotkey for the brightness-up key; empty turns it off."},
    {S::computer, "media", "brightness_down", "Media", K::hotkey, "", "", "",
     "Hotkey for the brightness-down key; empty turns it off."},

    // [touch]
    {S::both, "touch", "enabled", "Touchscreen", K::boolean, "", "", "false",
     "The left click becomes a tap at the tracked position."},
    {S::both, "touch", "scroll", "Touchscreen", K::integer, "-2000..2000", "device px", "0",
     "Touchscreen swipe: how far a wheel notch moves the swiping finger up or down, in pixels "
     "of the device's screen (times the scroll sensitivity). The finger moves that far at "
     "once. 0 keeps the wheel a mouse wheel, a negative value reverses it."},
    {S::both, "touch", "scroll_pan", "Touchscreen", K::integer, "-2000..2000", "device px", "0",
     "Touchscreen swipe: the same for the horizontal wheel (sideways)."},
    {S::both, "touch", "scroll_release_ms", "Touchscreen", K::integer, "0..2000", "ms", "200",
     "Touchscreen swipe: milliseconds from the swiping finger's move to its lift, for every "
     "wheel notch; 0 lifts it in the report after the move."},
    {S::both, "touch", "scroll_start_ms", "Touchscreen", K::real, "0..2000", "ms", "1",
     "Touchscreen swipe: milliseconds from the swiping finger being put down to its move, "
     "for every wheel notch; 0 moves it in the very next report."},
    {S::both, "touch", "tap_button", "Touchscreen", K::integer, "1..8", "", "1",
     "The mouse button that becomes a tap."},

    // [motion]: the computer's values, which a device may replace
    {S::both, "motion", "sensitivity", "Motion", K::real, "0.05..20", "", "1",
     "The mouse's movement is sent as it comes, times this."},
    {S::both, "motion", "sensitivity_y", "Motion", K::real, "0.05..20", "", "",
     "Vertical only; empty follows sensitivity."},
    {S::both, "motion", "scroll_sensitivity", "Motion", K::real, "0.05..20", "", "1",
     "Scrolling, times this."},
    {S::both, "motion", "entry", "Motion", K::choice, "parked|aligned|corner", "", "parked",
     "How the cursor enters a device: parked waits in the device's bottom right corner, aligned "
     "moves it from where it was, corner takes the corner reference every time."},
    {S::both, "motion", "resync_width", "Motion", K::real, "0..100000", "px", "48",
     "Pixels of uncertainty above which the corner reference is taken again."},
    {S::both, "motion", "enter_push", "Motion", K::real, "0..100000", "px", "0",
     "Pixels to push against the edge before crossing."},
    {S::both, "motion", "return_push", "Motion", K::real, "0..100000", "px", "",
     "Pixels past the device's edge before returning; empty is 1, or 16 when the gain is "
     "uncertain."},
    {S::both, "motion", "no_cross_while_button", "Motion", K::boolean, "", "", "true",
     "Do not cross an edge while a mouse button is held."},
    {S::both, "motion", "report_rate_hz", "Motion", K::text, "max|every|10..8000", "Hz", "max",
     "max sends a report as soon as the previous one completes; every sends each movement as a "
     "report of its own; 10 to 8000 paces motion at that many reports a second."},

    // [adb]
    {S::computer, "adb", "kill_server", "Adb", K::boolean, "", "", "true",
     "Stop a running adb server that holds the ADB interface of a phone whose proxy is to start."},
    {S::computer, "adb", "path", "Adb", K::text, "", "", "",
     "The adb program, run only by fill; empty uses PATH."},
    {S::computer, "adb", "first_port", "Adb", K::integer, "1024..65535", "", "6555",
     "Where the proxy ports start (not 5555 to 5585)."},

    // device file [device]
    {S::device, "device", "serial", "Device", K::text, "", "", "",
     "USB serial number; the file is named after it.", true},
    {S::device, "device", "name", "Device", K::text, "", "", "",
     "Display name, at most 48 bytes, without = ; #."},
    {S::device, "device", "enabled", "Device", K::boolean, "", "", "true",
     "false keeps the device configured but left alone."},
    {S::device, "device", "hotkey", "Device", K::hotkey, "", "", "",
     "Takes the input straight to this device."},

    // [detected]
    {S::device, "detected", "width", "Android screen", K::integer, "2..100000", "px", "",
     "Resolution in the natural orientation."},
    {S::device, "detected", "height", "Android screen", K::integer, "2..100000", "px", "",
     "Resolution in the natural orientation."},
    {S::device, "detected", "diagonal_inch", "Android screen", K::real, "1..1000", "in", "",
     "Screen diagonal."},
    {S::device, "detected", "pointer_speed", "Android screen", K::integer, "-7..7", "", "0",
     "Android's pointer speed setting."},
    {S::device, "detected", "density", "Android screen", K::integer, "40..2000", "dpi", "",
     "The density Android lays out with (wm density)."},
    {S::device, "detected", "accel", "Android screen", K::choice, "on|off", "", "",
     "Whether the device's pointer acceleration is on."},
    {S::device, "detected", "gain", "Android screen", K::real, "0.05..50", "px/count", "",
     "Pixels the cursor moves per count at low speed, typed or measured; empty is calculated from "
     "pointer_speed and density."},
    {S::device, "detected", "read_at", "Android screen", K::text, "", "", "",
     "The date the values were filled from adb.", true},

    // [placement]
    {S::device, "placement", "monitor", "Placement", K::text, "", "", "",
     "Monitor name as status shows it; empty is the first monitor."},
    {S::device, "placement", "beside", "Placement", K::text, "", "", "",
     "Serial of another device to sit next to; empty sits beside a monitor."},
    {S::device, "placement", "side", "Placement", K::choice, "left|right|top|bottom", "", "right",
     "Which side of the monitor or neighbour the device sits on."},
    {S::device, "placement", "start", "Placement", K::integer, "-1000000..1000000", "px", "",
     "Where the device starts, in PC pixels from the start of that edge; empty centres it."},
    {S::device, "placement", "length", "Placement", K::integer, "2..1000000", "px", "",
     "Its length in PC pixels; empty follows the real sizes."},
    {S::device, "placement", "corner_margin", "Placement", K::text, "0%..49%", "", "5%",
     "Dead zone at each corner of a monitor's edge; not used beside a device."},
    {S::device, "placement", "monitor_diagonal_inch", "Placement", K::real, "1..1000", "in", "",
     "For a monitor that reports no physical size."},
    {S::device, "placement", "rotation", "Placement", K::integer, "0, 90, 180 or 270", "deg", "",
     "How the device is turned now; it is not followed, so set it again when the device is "
     "turned."},
    {S::device, "placement", "turn_input", "Placement", K::integer, "0, 90, 180 or 270", "deg", "0",
     "How far the picture on the device is turned for the person looking at it; leave at 0 unless "
     "the device is mounted turned."},

    // device file [adb]
    {S::device, "adb", "proxy", "Adb", K::boolean, "", "", "false",
     "Serve this device's ADB interface on its port."},
    {S::device, "adb", "port", "Adb", K::integer, "1024..65535", "", "",
     "Local TCP port of this device's ADB proxy; empty takes the next free one."},
};

constexpr StatusKey status_table[] = {
    // The top level.
    {false, "protocol", "Host", "", "", "The version of the status format."},
    {false, "state", "Host", "", "",
     "Where the input is: pc, android, paused, or starting while no capture backend is up."},
    {false, "active", "Host", "", "", "The serial of the device that has the input."},
    {false, "backend", "Host", "", "", "The capture backend in use."},
    {false, "last_used", "Host", "", "", "The device that had the input last."},
    {false, "media_target", "Host", "", "", "The device media keys go to, as configured."},
    {false, "config_error", "Host", "", "",
     "Why the configuration could not be read; the daemon keeps the last good one."},
    {false, "capture_error", "Host", "", "",
     "Why no capture backend could start; the daemon tries again every 15 seconds."},
    {false, "local_hotkeys", "Host", "", "",
     "yes when hotkeys work while the input is on the computer; no on portal and layer_shell."},
    {false, "monitor.#", "Host", "", "name|x|y|width|height|mm wide|mm high",
     "A monitor as the daemon sees it: name, position and size in pixels, physical size in mm."},
    {false, "new.#", "Host", "", "",
     "A plugged-in device that has no file yet, with its product name."},
    {false, "files_version", "Files", "", "", "Counts the times the daemon wrote a device's file."},

    // Per device.
    {true, "name", "Device", "", "", "What it is called."},
    {true, "state", "Device", "", "", "ready, error or absent."},
    {true, "status", "Device", "", "", "Why it is not ready."},
    {true, "fill", "Device", "", "", "The last fill: ok or error, with what it said."},
    {true, "reading", "Device", "", "", "1 while a fill is under way."},
    {true, "wanted", "Device", "", "",
     "yes when connect was asked for it and has not been undone."},
    {true, "plugged", "Device", "", "", "yes when the last scan saw it plugged in."},
    {true, "serial", "Device", "", "", "Its serial."},
    {true, "proxy", "Device", "", "", "The ADB proxy port, or off."},
    {true, "size", "Device", "px", "", "The screen size in use (width x height)."},
    {true, "rotation", "Device", "deg", "", "The rotation in use."},
    {true, "density", "Device", "px/mm", "", "Android pixels per millimetre, in use."},
    {true, "mode", "Device", "", "", "exact or curve."},
    {true, "gain", "Device", "px/count", "", "Pixels per count at low speed."},
    {true, "segment", "Device", "px",
     "monitor|side|crossing from|crossing to|device from|device to",
     "Where it sits on a monitor: the monitor, the side, the crossing span and the device span (PC "
     "pixels)."},
    {true, "beside", "Device", "px", "device|side|crossing from|crossing to|device from|device to",
     "Where it sits next to another device, as for segment."},
    {true, "position", "Position", "px", "x low|x high|y low|y high",
     "The tracked cursor range in the display's view space."},
    {true, "touch", "Device", "", "",
     "off, on, or not set: followed by what the touchscreen still needs."},
    {true, "touch_error", "Position", "px", "x|y",
     "How far a tap can be from the cursor: the width of the tracked range."},
    {true, "reports", "Link", "", "", "USB reports sent."},
    {true, "merged", "Link", "", "", "Inputs summed into a report that was already waiting."},
    {true, "depth", "Link", "", "", "Reports waiting now."},
    {true, "realtime", "Link", "", "",
     "Whether the sender thread got a real-time priority (needs RLIMIT_RTPRIO)."},
    {true, "samples", "Timing", "", "", "How many of the last 256 reports the delays are over."},
    {true, "queue_us", "Timing", "us", "typical|99 of 100|worst",
     "From an input reaching the daemon to the start of its USB call."},
    {true, "total_us", "Timing", "us", "typical|99 of 100|worst",
     "From an input reaching the daemon to the report's completion; the device's own delay is not "
     "included."},
};

// The text a scope's file holds, as the library writes it.
std::string file_text(const Config& config, const DeviceConfig* device) {
    return device != nullptr ? format_device(*device) : format_globals(config);
}

// Parses `ini` back into the object it came from. The globals parser replaces
// the device list, so the devices are put back.
std::string take(Config& config, DeviceConfig* device, const IniText& ini) {
    if (device != nullptr) {
        DeviceConfig changed = *device;
        const std::string problem = parse_device(ini.str(), changed);
        if (problem.empty())
            *device = std::move(changed);
        return problem;
    }
    Config changed;
    const std::string problem = parse_globals(ini.str(), changed);
    if (!problem.empty())
        return problem;
    changed.devices = std::move(config.devices);
    config = std::move(changed);
    return {};
}

} // namespace

std::span<const ConfigKey> config_keys() { return table; }

const ConfigKey* find_config_key(const KeyScope scope, const std::string_view section,
                                 const std::string_view key) {
    for (const ConfigKey& each : table) {
        if ((each.scope == scope || each.scope == KeyScope::both) && section == each.section &&
            key == each.key)
            return &each;
    }
    return nullptr;
}

std::optional<std::string> key_text(const Config& config, const DeviceConfig* device,
                                    const ConfigKey& key) {
    return IniText(file_text(config, device)).get(key.section, key.key);
}

std::string set_key_text(Config& config, DeviceConfig* device, const ConfigKey& key,
                         const std::string_view text) {
    IniText ini(file_text(config, device));
    ini.set(key.section, key.key, text);
    return take(config, device, ini);
}

std::string clear_key(Config& config, DeviceConfig* device, const ConfigKey& key) {
    IniText ini(file_text(config, device));
    ini.erase(key.section, key.key);
    return take(config, device, ini);
}

std::span<const StatusKey> status_keys() { return status_table; }

const StatusKey* find_status_key(const bool per_device, const std::string_view key) {
    for (const StatusKey& each : status_table) {
        if (each.per_device != per_device)
            continue;
        const std::string_view name(each.key);
        if (name.back() == '#'
                ? key.substr(0, name.size() - 1) == name.substr(0, name.size() - 1) &&
                      key.size() >= name.size()
                : key == name)
            return &each;
    }
    return nullptr;
}

} // namespace aoas
