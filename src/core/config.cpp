// SPDX-License-Identifier: MIT
#include "aoahid_share/config.hpp"

#include "aoahid_share/accel_model.hpp"

#include "aoahid_share/keymap.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace aoas {
namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

bool to_bool(const std::string_view text, bool& out) noexcept {
    if (text == "true") {
        out = true;
        return true;
    }
    if (text == "false") {
        out = false;
        return true;
    }
    return false;
}

bool to_int(const std::string_view text, const int low, const int high, int& out) noexcept {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || value < low || value > high)
        return false;
    out = value;
    return true;
}

// strtod rather than std::from_chars: Apple's libc++ has no floating-point
// from_chars for the deployment targets this builds for. Only plain decimal
// numbers are accepted, so the C locale's parsing is all that is used.
bool to_double(const std::string_view text, const double low, const double high, double& out) {
    if (text.empty() || text.find_first_not_of("0123456789.") != std::string_view::npos)
        return false;
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !(value >= low) || !(value <= high))
        return false;
    out = value;
    return true;
}

bool to_side(const std::string_view text, Side& out) noexcept {
    if (text == "left")
        out = Side::left;
    else if (text == "right")
        out = Side::right;
    else if (text == "top")
        out = Side::top;
    else if (text == "bottom")
        out = Side::bottom;
    else
        return false;
    return true;
}

const char* side_name(const Side side) noexcept {
    switch (side) {
    case Side::left:
        return "left";
    case Side::right:
        return "right";
    case Side::top:
        return "top";
    case Side::bottom:
        return "bottom";
    }
    return "right";
}

bool to_rotation(const std::string_view text, int& out) noexcept {
    int value = 0;
    if (!to_int(text, 0, 270, value) || value % 90 != 0)
        return false;
    out = value;
    return true;
}

bool to_hotkey(const std::string_view text, std::string& out) {
    Hotkey parsed;
    if (!parse_hotkey(text, parsed))
        return false;
    out = text;
    return true;
}

bool to_button_map(std::string_view text, std::vector<unsigned>& out) {
    std::vector<unsigned> map;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        int value = 0;
        if (!to_int(trim(text.substr(0, comma)), 1, 8, value) || map.size() == 8)
            return false;
        map.push_back(static_cast<unsigned>(value));
        if (comma == std::string_view::npos)
            break;
        text.remove_prefix(comma + 1);
        if (text.empty())
            return false;
    }
    out = std::move(map);
    return true;
}

bool set_daemon(Config& c, const std::string_view key, const std::string_view value) {
    if (key == "backend") {
        static constexpr std::string_view known[] = {"auto",  "x11",     "portal", "layer_shell",
                                                     "evdev", "windows", "macos"};
        if (std::find(std::begin(known), std::end(known), value) == std::end(known))
            return false;
        c.backend = value;
        return true;
    }
    // Read and dropped: until 0.3.4 the hotkeys were here.
    return key == "toggle_hotkey" || key == "panic_hotkey" || key == "pause_hotkey" ||
           key == "resync_hotkey";
}

// "max", "every", or reports a second.
bool to_report_rate(const std::string_view value, ReportRate& out) {
    if (value == "max" || value == "every") {
        out = {0, value == "every"};
        return true;
    }
    int rate = 0;
    if (!to_int(value, 10, 8000, rate))
        return false;
    out = {static_cast<unsigned>(rate), false};
    return true;
}

bool set_touch_patch(TouchPatch& p, const std::string_view key, const std::string_view value) {
    int number = 0;
    if (key == "enabled") {
        bool on = false;
        if (!to_bool(value, on))
            return false;
        p.enabled = on;
        return true;
    }
    if (key == "swipe") {
        bool on = false;
        if (!to_bool(value, on))
            return false;
        p.swipe = on;
        return true;
    }
    if (key == "scroll" || key == "scroll_pan") {
        if (!to_int(value, -2000, 2000, number))
            return false;
        (key == "scroll" ? p.scroll : p.scroll_pan) = number;
        return true;
    }
    if (key == "scroll_release_ms") {
        if (!to_int(value, 0, 2000, number))
            return false;
        p.release_ms = number;
        return true;
    }
    if (key == "scroll_start_ms") {
        double ms = 0.0;
        if (!to_double(value, 0.0, 2000.0, ms))
            return false;
        p.start_ms = ms;
        return true;
    }
    if (key == "scroll_steps") {
        if (!to_int(value, 1, 64, number))
            return false;
        p.steps = number;
        return true;
    }
    if (key == "scroll_total_ms") {
        double ms = 0.0;
        if (!to_double(value, 0.0, 2000.0, ms))
            return false;
        p.total_ms = ms;
        return true;
    }
    if (key == "scroll_overlap") {
        if (value != "add" && value != "restart")
            return false;
        p.restart = value == "restart";
        return true;
    }
    if (key == "tap_button") {
        if (!to_int(value, 1, 8, number))
            return false;
        p.button = static_cast<unsigned>(number);
        return true;
    }
    return false;
}

void apply_touch(Touch& touch, const TouchPatch& p) noexcept {
    touch.enabled = p.enabled.value_or(touch.enabled);
    touch.swipe = p.swipe.value_or(touch.swipe);
    touch.scroll = p.scroll.value_or(touch.scroll);
    touch.scroll_pan = p.scroll_pan.value_or(touch.scroll_pan);
    touch.release_ms = p.release_ms.value_or(touch.release_ms);
    touch.start_ms = p.start_ms.value_or(touch.start_ms);
    touch.steps = p.steps.value_or(touch.steps);
    touch.total_ms = p.total_ms.value_or(touch.total_ms);
    touch.restart = p.restart.value_or(touch.restart);
    touch.button = p.button.value_or(touch.button);
}

bool set_mouse(Config& c, const std::string_view key, const std::string_view value) {
    if (key == "enabled")
        return to_bool(value, c.mouse);
    if (key == "buttons") {
        int buttons = 0;
        if (!to_int(value, 1, 8, buttons))
            return false;
        c.mouse_buttons = static_cast<unsigned>(buttons);
        return true;
    }
    if (key == "button_map")
        return to_button_map(value, c.button_map);
    return false;
}
bool set_motion(Motion& m, const std::string_view key, const std::string_view value) {
    if (key == "sensitivity")
        return to_double(value, 0.05, 20.0, m.sensitivity);
    if (key == "sensitivity_y") {
        if (value.empty()) {
            m.sensitivity_y = 0.0;
            return true;
        }
        return to_double(value, 0.05, 20.0, m.sensitivity_y);
    }
    if (key == "scroll_sensitivity")
        return to_double(value, 0.05, 20.0, m.scroll_sensitivity);
    if (key == "entry") {
        if (value == "parked") {
            m.entry = EntryMode::parked;
        } else if (value == "aligned") {
            m.entry = EntryMode::aligned;
        } else if (value == "corner") {
            m.entry = EntryMode::corner;
        } else if (value == "align") {
            // Earlier: not the corner; whether to park was a key of its own.
            if (m.entry == EntryMode::corner)
                m.entry = EntryMode::parked;
        } else {
            return false;
        }
        return true;
    }
    if (key == "park") {
        // Earlier: whether to park was a key of its own.
        bool park = true;
        if (!to_bool(value, park))
            return false;
        if (park && m.entry == EntryMode::aligned)
            m.entry = EntryMode::parked;
        else if (!park && m.entry == EntryMode::parked)
            m.entry = EntryMode::aligned;
        return true;
    }
    if (key == "resync_width")
        return to_double(value, 0.0, 100000.0, m.resync_width);
    if (key == "enter_push")
        return to_double(value, 0.0, 100000.0, m.enter_push);
    if (key == "return_push") {
        if (value.empty()) {
            m.return_push = -1.0;
            return true;
        }
        return to_double(value, 0.0, 100000.0, m.return_push);
    }
    if (key == "no_cross_while_button")
        return to_bool(value, m.no_cross_while_button);
    return false;
}

// A device's own [motion]: the same keys and ranges, kept only where given.
bool set_motion_patch(MotionPatch& p, const std::string_view key, const std::string_view value) {
    if (key == "report_rate_hz") {
        ReportRate rate;
        if (!to_report_rate(value, rate))
            return false;
        p.report_rate = rate;
        return true;
    }
    Motion m;
    if (!set_motion(m, key, value))
        return false;
    if (key == "match_physical" || key == "counts_per_pixel")
        return true;
    if (key == "sensitivity")
        p.sensitivity = m.sensitivity;
    else if (key == "sensitivity_y")
        p.sensitivity_y = m.sensitivity_y;
    else if (key == "scroll_sensitivity")
        p.scroll_sensitivity = m.scroll_sensitivity;
    else if (key == "entry" || key == "park")
        p.entry = m.entry;
    else if (key == "resync_width")
        p.resync_width = m.resync_width;
    else if (key == "enter_push")
        p.enter_push = m.enter_push;
    else if (key == "return_push")
        p.return_push = m.return_push;
    else
        p.no_cross_while_button = m.no_cross_while_button;
    return true;
}

bool set_global_motion(Config& c, const std::string_view key, const std::string_view value) {
    if (key == "report_rate_hz") {
        ReportRate rate;
        if (!to_report_rate(value, rate))
            return false;
        c.report_rate_hz = rate.hz;
        c.report_every = rate.every;
        return true;
    }
    return set_motion(c.motion, key, value);
}

// 5555-5585 is the range adb scans for emulators.
bool to_port(const std::string_view value, uint16_t& out) noexcept {
    int port = 0;
    if (!to_int(value, 1024, 65535, port) || (port >= 5555 && port <= 5585))
        return false;
    out = static_cast<uint16_t>(port);
    return true;
}

bool set_adb(Config& c, const std::string_view key, const std::string_view value) {
    if (key == "kill_server")
        return to_bool(value, c.adb_kill_server);
    if (key == "path") {
        c.adb_path = value;
        return true;
    }
    if (key == "first_port")
        return to_port(value, c.adb_first_port);
    return false;
}

bool set_auto_int(const std::string_view value, const int low, const int high, const int unset,
                  int& out) noexcept {
    // Empty is a value not given.
    if (value.empty()) {
        out = unset;
        return true;
    }
    return to_int(value, low, high, out);
}

bool set_auto_double(const std::string_view value, const double low, const double high,
                     double& out) noexcept {
    if (value.empty()) {
        out = 0.0;
        return true;
    }
    return to_double(value, low, high, out);
}

bool to_margin(std::string_view value, double& out) {
    if (value.empty() || value.back() != '%')
        return false;
    value.remove_suffix(1);
    double percent = 0.0;
    if (!to_double(value, 0.0, 49.0, percent))
        return false;
    out = percent / 100.0;
    return true;
}

bool to_accel(const std::string_view value, AccelSetting& out) noexcept {
    if (value.empty() || value == "auto")
        out = AccelSetting::automatic;
    else if (value == "off")
        out = AccelSetting::off;
    else if (value == "on")
        out = AccelSetting::on;
    else
        return false;
    return true;
}

// What a name may hold: visible characters, but none that the file's own
// syntax gives a meaning to. `;` and `#` start a comment after a space, so a
// name with one would read back shorter than it was written.
bool name_character(const char c) noexcept {
    return static_cast<unsigned char>(c) >= 0x20 && c != 0x7F && c != '=' && c != ';' && c != '#';
}

bool to_name(const std::string_view value, std::string& out) {
    if (!valid_name(value))
        return false;
    out = value;
    return true;
}

// Empty is allowed here: a profile that waits for its device has none.
bool to_serial(const std::string_view value, std::string& out) {
    if (!value.empty() && !valid_serial(value))
        return false;
    out = value;
    return true;
}

char upper(const char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

bool same_ignoring_case(const std::string_view a, const std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](const char x, const char y) { return upper(x) == upper(y); });
}

bool set_placement(DeviceConfig& d, const std::string_view key, const std::string_view value) {
    if (key == "monitor")
        return to_name(value, d.monitor);
    if (key == "beside")
        return to_serial(value, d.beside);
    if (key == "side")
        return to_side(value, d.side);
    if (key == "start")
        return set_auto_int(value, -1000000, 1000000, centred_start, d.segment_start);
    if (key == "length")
        return set_auto_int(value, 2, 1000000, -1, d.segment_length);
    if (key == "corner_margin")
        return to_margin(value, d.corner_margin);
    if (key == "monitor_diagonal_inch")
        return set_auto_double(value, 1.0, 1000.0, d.monitor_diagonal_inch);
    if (key == "rotation") {
        if (value.empty()) {
            d.rotation = -1;
            return true;
        }
        return to_rotation(value, d.rotation);
    }
    if (key == "turn_input")
        return to_rotation(value, d.mount_rotation);
    return false;
}

bool set_detected(DeviceConfig& d, const std::string_view key, const std::string_view value) {
    if (key == "width")
        return set_auto_int(value, 2, 100000, 0, d.width);
    if (key == "height")
        return set_auto_int(value, 2, 100000, 0, d.height);
    if (key == "diagonal_inch")
        return set_auto_double(value, 1.0, 1000.0, d.diagonal_inch);
    if (key == "pointer_speed")
        return to_int(value, -7, 7, d.pointer_speed);
    if (key == "density")
        return set_auto_int(value, 40, 2000, 0, d.density_dpi);
    if (key == "accel")
        return to_accel(value, d.accel);
    if (key == "gain")
        return set_auto_double(value, 0.05, 50.0, d.gain);
    if (key == "read_at")
        return to_name(value, d.read_at);
    return false;
}

std::string line_error(const size_t line, const std::string& what) {
    return "line " + std::to_string(line) + ": " + what;
}

std::string number(const double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.6g", value);
    return buffer;
}

const char* yes_no(const bool value) noexcept { return value ? "true" : "false"; }

std::string rate_text(const bool every, const unsigned hz) {
    return every ? "every" : hz == 0 ? "max" : std::to_string(hz);
}

// Walks an ini text. on_section(name) returns what is wrong with the name,
// or nothing; on_value(key, value) returns whether the setting was taken.
template <typename OnSection, typename OnValue>
std::string read_ini(std::string_view text, OnSection&& on_section, OnValue&& on_value) {
    size_t line_number = 0;
    bool in_section = false;
    // A file saved by an editor that marks UTF-8 starts with these bytes.
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        ++line_number;

        // A comment starts at ';' or '#' at the start of the line or after
        // white space, so a value may contain either character.
        for (size_t i = 0; i < line.size(); ++i) {
            if ((line[i] == ';' || line[i] == '#') &&
                (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line = line.substr(0, i);
                break;
            }
        }
        line = trim(line);
        if (line.empty())
            continue;

        if (line.front() == '[') {
            if (line.back() != ']')
                return line_error(line_number, "the section name is not closed");
            const std::string problem = on_section(line.substr(1, line.size() - 2));
            if (!problem.empty())
                return line_error(line_number, problem);
            in_section = true;
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos)
            return line_error(line_number, "expected key = value");
        if (!in_section)
            return line_error(line_number, "the setting is outside any section");
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1));
        if (!on_value(key, value)) {
            return line_error(line_number, "unknown setting or invalid value: " + std::string(key) +
                                               " = " + std::string(value));
        }
    }
    return {};
}

struct Writer {
    std::string text;
    void section(const char* name) {
        if (!text.empty())
            text += '\n';
        text += '[';
        text += name;
        text += "]\n";
    }
    void put(const std::string_view key, const std::string_view value) {
        text += key;
        text += " = ";
        text += value;
        text += '\n';
    }
};

const char* entry_name(const EntryMode mode) noexcept {
    return mode == EntryMode::parked ? "parked" : mode == EntryMode::aligned ? "aligned" : "corner";
}

void put_motion(Writer& out, const Motion& m) {
    out.put("sensitivity", number(m.sensitivity));
    out.put("sensitivity_y", m.sensitivity_y > 0.0 ? number(m.sensitivity_y) : std::string());
    out.put("scroll_sensitivity", number(m.scroll_sensitivity));
    out.put("entry", entry_name(m.entry));
    out.put("resync_width", number(m.resync_width));
    out.put("enter_push", number(m.enter_push));
    out.put("return_push", m.return_push >= 0.0 ? number(m.return_push) : std::string());
    out.put("no_cross_while_button", yes_no(m.no_cross_while_button));
}

bool empty(const MotionPatch& p) noexcept {
    return !p.sensitivity && !p.sensitivity_y && !p.scroll_sensitivity && !p.entry &&
           !p.resync_width && !p.enter_push && !p.return_push && !p.no_cross_while_button &&
           !p.report_rate;
}

constexpr std::string_view device_mark = "--- device ---";

} // namespace

bool valid_serial(const std::string_view text) noexcept {
    if (text.empty() || text.size() > 64 || text.front() == '.')
        return false;
    for (const char c : text) {
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.';
        if (!plain)
            return false;
    }
    // Names Windows keeps for devices, with or without an extension.
    static constexpr std::string_view kept[] = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const std::string_view stem = text.substr(0, text.find('.'));
    for (const std::string_view name : kept) {
        if (same_ignoring_case(stem, name))
            return false;
    }
    return true;
}

bool valid_name(const std::string_view text) noexcept {
    if (text.size() > 48)
        return false;
    for (const char c : text) {
        if (!name_character(c))
            return false;
    }
    return text.empty() || (text.front() != ' ' && text.back() != ' ');
}

std::string clean_name(const std::string_view text) {
    std::string name;
    for (const char c : text) {
        if (name_character(c) && name.size() < 48)
            name += c;
    }
    // The limit counts bytes: a character it cut through is dropped whole.
    size_t lead = name.size();
    while (lead > 0 && (static_cast<unsigned char>(name[lead - 1]) & 0xC0) == 0x80)
        --lead;
    if (lead > 0 && static_cast<unsigned char>(name[lead - 1]) >= 0xC0) {
        const unsigned char first_byte = static_cast<unsigned char>(name[lead - 1]);
        const size_t need = first_byte >= 0xF0 ? 4 : first_byte >= 0xE0 ? 3 : 2;
        if (name.size() - (lead - 1) < need)
            name.erase(lead - 1);
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    const size_t first = name.find_first_not_of(' ');
    return first == std::string::npos ? std::string() : name.substr(first);
}

Motion motion_of(const Config& config, const DeviceConfig& device) noexcept {
    Motion m = config.motion;
    const MotionPatch& p = device.motion;
    m.sensitivity = p.sensitivity.value_or(m.sensitivity);
    // A device's own sensitivity is for both axes unless it also gives a
    // vertical one; the global vertical value belongs to the global one.
    if (p.sensitivity)
        m.sensitivity_y = 0.0;
    m.sensitivity_y = p.sensitivity_y.value_or(m.sensitivity_y);
    m.scroll_sensitivity = p.scroll_sensitivity.value_or(m.scroll_sensitivity);
    m.entry = p.entry.value_or(m.entry);
    m.resync_width = p.resync_width.value_or(m.resync_width);
    m.enter_push = p.enter_push.value_or(m.enter_push);
    m.return_push = p.return_push.value_or(m.return_push);
    m.no_cross_while_button = p.no_cross_while_button.value_or(m.no_cross_while_button);
    return m;
}

bool has_own_motion(const MotionPatch& patch) noexcept { return !empty(patch); }

ReportRate report_rate_of(const Config& config, const DeviceConfig& device) noexcept {
    return device.motion.report_rate.value_or(
        ReportRate{config.report_rate_hz, config.report_every});
}

MotionPatch motion_patch_of(const Motion& m) noexcept {
    MotionPatch p;
    p.sensitivity = m.sensitivity;
    p.sensitivity_y = m.sensitivity_y;
    p.scroll_sensitivity = m.scroll_sensitivity;
    p.entry = m.entry;
    p.resync_width = m.resync_width;
    p.enter_push = m.enter_push;
    p.return_push = m.return_push;
    p.no_cross_while_button = m.no_cross_while_button;
    return p;
}

double effective_gain(const DeviceConfig& device) noexcept {
    return device.gain > 0.0 ? device.gain
                             : low_speed_gain(device.pointer_speed, device.density_dpi);
}

bool mouse_of(const Config& config, const DeviceConfig& device) noexcept {
    return device.mouse.value_or(config.mouse);
}

bool keyboard_of(const Config& config, const DeviceConfig& device) noexcept {
    return device.keyboard.value_or(config.keyboard);
}

bool media_of(const Config& config, const DeviceConfig& device) noexcept {
    return device.media.value_or(config.media);
}

Touch touch_of(const Config& config, const DeviceConfig& device) noexcept {
    Touch touch = config.touch;
    apply_touch(touch, device.touch);
    return touch;
}

uint16_t adb_port_of(const Config& config, const size_t index) noexcept {
    if (index >= config.devices.size())
        return 0;
    if (config.devices[index].adb_port != 0)
        return config.devices[index].adb_port;
    // The devices without a port of their own take the free ones upward
    // from first_port, in their order.
    const auto taken = [&](const unsigned port) {
        if (port >= 5555 && port <= 5585)
            return true;
        for (const DeviceConfig& device : config.devices) {
            if (device.adb_port == port)
                return true;
        }
        return false;
    };
    unsigned port = config.adb_first_port;
    for (size_t before = 0; before <= index; ++before) {
        if (config.devices[before].adb_port != 0)
            continue;
        while (port < 65535 && taken(port))
            ++port;
        if (before == index)
            break;
        ++port;
    }
    return static_cast<uint16_t>(std::min(port, 65535U));
}

const std::string& label_of(const DeviceConfig& device) noexcept {
    return device.name.empty() ? device.serial : device.name;
}

size_t find_device(const Config& config, const std::string_view name_or_serial) noexcept {
    if (name_or_serial.empty())
        return config.devices.size();
    for (size_t index = 0; index < config.devices.size(); ++index) {
        if (config.devices[index].serial == name_or_serial)
            return index;
    }
    for (size_t index = 0; index < config.devices.size(); ++index) {
        if (config.devices[index].name == name_or_serial)
            return index;
    }
    return config.devices.size();
}

std::string parse_globals(const std::string_view text, Config& out) {
    Config config;
    enum class Section { daemon, mouse, keyboard, media, touch, motion, adb };
    Section section = Section::daemon;
    const std::string error = read_ini(
        text,
        [&](const std::string_view name) -> std::string {
            if (name == "daemon") {
                section = Section::daemon;
            } else if (name == "mouse") {
                section = Section::mouse;
            } else if (name == "keyboard") {
                section = Section::keyboard;
            } else if (name == "media") {
                section = Section::media;
            } else if (name == "touch") {
                section = Section::touch;
            } else if (name == "motion") {
                section = Section::motion;
            } else if (name == "adb") {
                section = Section::adb;
            } else {
                return "unknown section [" + std::string(name) + "]";
            }
            return {};
        },
        [&](const std::string_view key, const std::string_view value) {
            switch (section) {
            case Section::daemon:
                return set_daemon(config, key, value);
            case Section::mouse:
                return set_mouse(config, key, value);
            case Section::keyboard:
                return key == "enabled" && to_bool(value, config.keyboard);
            case Section::media:
                if (key == "enabled")
                    return to_bool(value, config.media);
                // Read and dropped: until 0.3.4 the media hotkeys were here.
                if (key == "target")
                    return true;
                for (const MediaKey& each : media_keys) {
                    if (key == each.name)
                        return true;
                }
                return false;
            case Section::touch: {
                TouchPatch patch;
                if (!set_touch_patch(patch, key, value))
                    return false;
                apply_touch(config.touch, patch);
                return true;
            }
            case Section::motion:
                return set_global_motion(config, key, value);
            case Section::adb:
                return set_adb(config, key, value);
            }
            return false;
        });
    if (!error.empty())
        return error;
    out = std::move(config);
    return {};
}

std::string format_globals(const Config& c) {
    Writer out;
    out.section("daemon");
    out.put("backend", c.backend);

    out.section("mouse");
    out.put("enabled", yes_no(c.mouse));
    out.put("buttons", std::to_string(c.mouse_buttons));
    std::string map;
    for (const unsigned button : c.button_map)
        map += (map.empty() ? "" : ",") + std::to_string(button);
    out.put("button_map", map);

    out.section("keyboard");
    out.put("enabled", yes_no(c.keyboard));

    out.section("media");
    out.put("enabled", yes_no(c.media));

    out.section("touch");
    out.put("enabled", yes_no(c.touch.enabled));
    out.put("swipe", yes_no(c.touch.swipe));
    out.put("scroll", std::to_string(c.touch.scroll));
    out.put("scroll_pan", std::to_string(c.touch.scroll_pan));
    out.put("scroll_release_ms", std::to_string(c.touch.release_ms));
    out.put("scroll_start_ms", number(c.touch.start_ms));
    out.put("scroll_steps", std::to_string(c.touch.steps));
    out.put("scroll_total_ms", number(c.touch.total_ms));
    out.put("scroll_overlap", c.touch.restart ? "restart" : "add");
    out.put("tap_button", std::to_string(c.touch.button));

    out.section("motion");
    put_motion(out, c.motion);
    out.put("report_rate_hz", rate_text(c.report_every, c.report_rate_hz));

    out.section("adb");
    out.put("kill_server", yes_no(c.adb_kill_server));
    out.put("path", c.adb_path);
    out.put("first_port", std::to_string(c.adb_first_port));
    return out.text;
}

std::string parse_device(const std::string_view text, DeviceConfig& out) {
    DeviceConfig device;
    enum class Section {
        device,
        keys,
        placement,
        detected,
        motion,
        mouse,
        keyboard,
        media,
        touch,
        adb
    };
    Section section = Section::device;
    const auto set_optional = [](const std::string_view value, std::optional<bool>& slot) {
        bool parsed = false;
        if (!to_bool(value, parsed))
            return false;
        slot = parsed;
        return true;
    };
    const std::string error = read_ini(
        text,
        [&](const std::string_view name) -> std::string {
            if (name == "device")
                section = Section::device;
            else if (name == "keys")
                section = Section::keys;
            else if (name == "placement")
                section = Section::placement;
            else if (name == "detected")
                section = Section::detected;
            else if (name == "motion")
                section = Section::motion;
            else if (name == "mouse")
                section = Section::mouse;
            else if (name == "keyboard")
                section = Section::keyboard;
            else if (name == "media")
                section = Section::media;
            else if (name == "touch")
                section = Section::touch;
            else if (name == "adb")
                section = Section::adb;
            else
                return "unknown section [" + std::string(name) + "]";
            return {};
        },
        [&](const std::string_view key, const std::string_view value) {
            switch (section) {
            case Section::device:
                if (key == "serial")
                    return to_serial(value, device.serial);
                if (key == "name")
                    return to_name(value, device.name);
                if (key == "enabled")
                    return to_bool(value, device.enabled);
                // Read and dropped: [keys] switch since 0.3.5.
                return key == "hotkey";
            case Section::keys:
                if (key == "switch")
                    return to_hotkey(value, device.keys.switch_key);
                if (key == "lock")
                    return to_hotkey(value, device.keys.lock);
                if (key == "resync")
                    return to_hotkey(value, device.keys.resync);
                for (size_t index = 0; index < media_key_count; ++index) {
                    if (key == media_keys[index].name)
                        return to_hotkey(value, device.keys.media[index]);
                }
                return false;
            case Section::placement:
                return set_placement(device, key, value);
            case Section::detected:
                return set_detected(device, key, value);
            case Section::motion:
                return set_motion_patch(device.motion, key, value);
            case Section::mouse:
                return key == "enabled" && set_optional(value, device.mouse);
            case Section::keyboard:
                return key == "enabled" && set_optional(value, device.keyboard);
            case Section::media:
                return key == "enabled" && set_optional(value, device.media);
            case Section::touch:
                return set_touch_patch(device.touch, key, value);
            case Section::adb:
                if (key == "port")
                    return to_port(value, device.adb_port);
                if (key == "proxy")
                    return to_bool(value, device.adb_proxy);
                return false;
            }
            return false;
        });
    if (!error.empty())
        return error;
    // A gain that is only what the pointer speed and the density give (as an
    // earlier "fill" wrote it) is not a value of its own: it would stay behind
    // when the pointer speed is changed.
    const double calculated = low_speed_gain(device.pointer_speed, device.density_dpi);
    if (device.gain > 0.0 && calculated > 0.0 &&
        std::fabs(device.gain - calculated) <= 0.005 * calculated)
        device.gain = 0.0;
    out = std::move(device);
    return {};
}

std::string format_device(const DeviceConfig& d) {
    Writer out;
    out.section("device");
    out.put("serial", d.serial);
    out.put("name", d.name);
    out.put("enabled", yes_no(d.enabled));

    out.section("keys");
    out.put("switch", d.keys.switch_key);
    out.put("lock", d.keys.lock);
    out.put("resync", d.keys.resync);
    for (size_t index = 0; index < media_key_count; ++index)
        out.put(media_keys[index].name, d.keys.media[index]);

    out.section("placement");
    out.put("monitor", d.monitor);
    out.put("beside", d.beside);
    out.put("side", side_name(d.side));
    if (!(d.segment_start == centred_start))
        out.put("start", std::to_string(d.segment_start));
    if (!(d.segment_length < 0))
        out.put("length", std::to_string(d.segment_length));
    char margin[32];
    std::snprintf(margin, sizeof margin, "%.6g%%", d.corner_margin * 100.0);
    out.put("corner_margin", margin);
    if (!(d.monitor_diagonal_inch <= 0.0))
        out.put("monitor_diagonal_inch", number(d.monitor_diagonal_inch));
    if (!(d.rotation < 0))
        out.put("rotation", std::to_string(d.rotation));
    out.put("turn_input", std::to_string(d.mount_rotation));

    out.section("detected");
    if (!(d.width <= 0))
        out.put("width", std::to_string(d.width));
    if (!(d.height <= 0))
        out.put("height", std::to_string(d.height));
    if (!(d.diagonal_inch <= 0.0))
        out.put("diagonal_inch", number(d.diagonal_inch));
    if (!(d.gain <= 0.0))
        out.put("gain", number(d.gain));
    if (d.accel != AccelSetting::automatic)
        out.put("accel", d.accel == AccelSetting::off ? "off" : "on");
    out.put("pointer_speed", std::to_string(d.pointer_speed));
    if (d.density_dpi > 0)
        out.put("density", std::to_string(d.density_dpi));
    out.put("read_at", d.read_at);

    // Only what the device sets for itself is written below; the rest
    // follows config.ini.
    if (!empty(d.motion)) {
        out.section("motion");
        const MotionPatch& p = d.motion;
        if (p.sensitivity)
            out.put("sensitivity", number(*p.sensitivity));
        if (p.sensitivity_y)
            out.put("sensitivity_y", *p.sensitivity_y > 0.0 ? number(*p.sensitivity_y) : "");
        if (p.scroll_sensitivity)
            out.put("scroll_sensitivity", number(*p.scroll_sensitivity));
        if (p.entry)
            out.put("entry", entry_name(*p.entry));
        if (p.resync_width)
            out.put("resync_width", number(*p.resync_width));
        if (p.enter_push)
            out.put("enter_push", number(*p.enter_push));
        if (p.return_push)
            out.put("return_push", *p.return_push >= 0.0 ? number(*p.return_push) : "");
        if (p.no_cross_while_button)
            out.put("no_cross_while_button", yes_no(*p.no_cross_while_button));
        if (p.report_rate)
            out.put("report_rate_hz", rate_text(p.report_rate->every, p.report_rate->hz));
    }
    if (d.mouse) {
        out.section("mouse");
        out.put("enabled", yes_no(*d.mouse));
    }
    if (d.keyboard) {
        out.section("keyboard");
        out.put("enabled", yes_no(*d.keyboard));
    }
    if (d.media) {
        out.section("media");
        out.put("enabled", yes_no(*d.media));
    }
    const TouchPatch& t = d.touch;
    if (t.enabled || t.swipe || t.scroll || t.scroll_pan || t.release_ms || t.start_ms || t.steps ||
        t.total_ms || t.restart || t.button) {
        out.section("touch");
        if (t.enabled)
            out.put("enabled", yes_no(*t.enabled));
        if (t.swipe)
            out.put("swipe", yes_no(*t.swipe));
        if (t.scroll)
            out.put("scroll", std::to_string(*t.scroll));
        if (t.scroll_pan)
            out.put("scroll_pan", std::to_string(*t.scroll_pan));
        if (t.release_ms)
            out.put("scroll_release_ms", std::to_string(*t.release_ms));
        if (t.start_ms)
            out.put("scroll_start_ms", number(*t.start_ms));
        if (t.steps)
            out.put("scroll_steps", std::to_string(*t.steps));
        if (t.total_ms)
            out.put("scroll_total_ms", number(*t.total_ms));
        if (t.restart)
            out.put("scroll_overlap", *t.restart ? "restart" : "add");
        if (t.button)
            out.put("tap_button", std::to_string(*t.button));
    }
    if (d.adb_proxy || d.adb_port != 0) {
        out.section("adb");
        if (d.adb_proxy)
            out.put("proxy", yes_no(true));
        if (d.adb_port != 0)
            out.put("port", std::to_string(d.adb_port));
    }
    return out.text;
}

std::string parse_config(std::string_view text, Config& out) {
    // Split at the lines that are exactly the device mark.
    std::vector<std::string_view> parts;
    size_t begin = 0;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t newline = std::min(text.find('\n', at), text.size());
        if (trim(text.substr(at, newline - at)) == device_mark) {
            parts.push_back(text.substr(begin, at - begin));
            begin = newline + 1;
        }
        at = newline + 1;
    }
    parts.push_back(begin <= text.size() ? text.substr(begin) : std::string_view());

    Config config;
    std::string error = parse_globals(parts[0], config);
    if (!error.empty())
        return error;
    if (config.devices.size() + parts.size() - 1 > max_devices)
        return "there are more than " + std::to_string(max_devices) + " devices";
    for (size_t index = 1; index < parts.size(); ++index) {
        DeviceConfig device;
        error = parse_device(parts[index], device);
        if (!error.empty())
            return "device " + std::to_string(index) + ", " + error;
        config.devices.push_back(std::move(device));
    }
    error = validate_config(config);
    if (!error.empty())
        return error;
    out = std::move(config);
    return {};
}

std::string format_config(const Config& config) {
    std::string text = format_globals(config);
    for (const DeviceConfig& device : config.devices) {
        text += '\n';
        text += device_mark;
        text += '\n';
        text += format_device(device);
    }
    return text;
}

std::string validate_config(const Config& config) {
    const std::vector<DeviceConfig>& devices = config.devices;
    if (devices.size() > max_devices)
        return "there are more than " + std::to_string(max_devices) + " devices";
    for (size_t a = 0; a < devices.size(); ++a) {
        for (size_t b = a + 1; b < devices.size(); ++b) {
            // One file each, on file systems that ignore case too.
            if (!devices[a].serial.empty() &&
                same_ignoring_case(devices[a].serial, devices[b].serial))
                return "two devices have the serial " + devices[a].serial;
            if (!label_of(devices[a]).empty() && label_of(devices[a]) == label_of(devices[b]))
                return "two devices are called " + label_of(devices[a]);
            // find_device() takes a serial before a name: a name that is
            // another device's serial could never be reached.
            if ((!devices[a].name.empty() && devices[a].name == devices[b].serial) ||
                (!devices[b].name.empty() && devices[b].name == devices[a].serial))
                return "a device is named after another device's serial";
            if (adb_port_of(config, a) == adb_port_of(config, b))
                return label_of(devices[a]) + " and " + label_of(devices[b]) +
                       " use the same adb port";
        }
    }

    // One key combination does one thing. A device's switch and media keys
    // work wherever the input is, so nothing may repeat them; its lock and
    // resync keys act only while it has the input, so two devices may share
    // them.
    struct Taken {
        Hotkey hotkey;
        size_t device;
        bool anywhere;
        std::string what;
    };
    std::vector<Taken> taken;
    const auto add = [&](const std::string& text, const size_t device, const bool anywhere,
                         const std::string_view name) -> std::string {
        Hotkey hotkey;
        if (!parse_hotkey(text, hotkey) || !hotkey.set())
            return {};
        const std::string what = label_of(devices[device]) + "'s " + std::string(name);
        for (const Taken& other : taken) {
            if (other.hotkey.usage == hotkey.usage && other.hotkey.modifiers == hotkey.modifiers &&
                (other.device == device || other.anywhere || anywhere))
                return text + " is the key of both " + other.what + " and " + what;
        }
        taken.push_back({hotkey, device, anywhere, what});
        return {};
    };
    std::string error;
    for (size_t index = 0; index < devices.size() && error.empty(); ++index) {
        const DeviceKeys& keys = devices[index].keys;
        error = add(keys.switch_key, index, true, "switch");
        for (size_t key = 0; key < media_key_count && error.empty(); ++key)
            error = add(keys.media[key], index, true, media_keys[key].name);
        if (error.empty())
            error = add(keys.lock, index, false, "lock");
        if (error.empty())
            error = add(keys.resync, index, false, "resync");
    }
    if (!error.empty())
        return error;

    // A device beside another has to lead, device by device, to a monitor.
    for (const DeviceConfig& device : devices) {
        const DeviceConfig* at = &device;
        for (size_t steps = 0; !at->beside.empty(); ++steps) {
            const size_t next = find_device(config, at->beside);
            if (next == devices.size() || devices[next].serial != at->beside)
                return label_of(*at) + " is beside " + at->beside + ", which is not a device";
            if (steps >= devices.size())
                return label_of(device) + " is beside itself, directly or through others";
            at = &devices[next];
        }
    }
    return {};
}

bool keep_local_adb(Config& imported, const Config& local) {
    bool changed = imported.adb_kill_server != local.adb_kill_server ||
                   imported.adb_path != local.adb_path ||
                   imported.adb_first_port != local.adb_first_port;
    imported.adb_kill_server = local.adb_kill_server;
    imported.adb_path = local.adb_path;
    imported.adb_first_port = local.adb_first_port;
    // Whether a device's ADB interface is served, and where, is decided here.
    for (DeviceConfig& device : imported.devices) {
        // By serial alone: find_device() also takes a name, and a name is
        // not what the file's serial is compared with.
        const auto own =
            std::find_if(local.devices.begin(), local.devices.end(), [&](const DeviceConfig& each) {
                return !device.serial.empty() && each.serial == device.serial;
            });
        const bool proxy = own != local.devices.end() && own->adb_proxy;
        const uint16_t port = own != local.devices.end() ? own->adb_port : 0;
        changed = changed || device.adb_proxy != proxy || device.adb_port != port;
        device.adb_proxy = proxy;
        device.adb_port = port;
    }
    return changed;
}

} // namespace aoas
