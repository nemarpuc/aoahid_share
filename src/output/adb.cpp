// SPDX-License-Identifier: MIT
#include "adb.hpp"

#include "process.hpp"

#include "aoahid_share/accel_model.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>

namespace aoas {
namespace {

constexpr int command_timeout_ms = 4000;
constexpr int connect_timeout_ms = 6000;
// How long a phone may take to go from "offline" through "unauthorized" (the
// permission dialog) to "device" after `adb connect`.
constexpr int usable_timeout_ms = 15000;
constexpr int usable_poll_ms = 500;
constexpr const char* accel_key = "mouse_pointer_acceleration_enabled";
// Samsung's own switch for the same thing ("Enhance pointer precision").
constexpr const char* vendor_accel_key = "enhance_pointer_precision";
// The first API level whose AOSP source has accel_key (Android 16).
constexpr int aosp_accel_sdk = 36;

std::string serial(const uint16_t port) { return "127.0.0.1:" + std::to_string(port); }

std::string_view trimmed(std::string_view text) noexcept {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.remove_suffix(1);
    while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' '))
        text.remove_prefix(1);
    return text;
}

// Reads an unsigned integer at the start of text and steps past it.
bool take_int(std::string_view& text, int& out) noexcept {
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end == text.data())
        return false;
    text.remove_prefix(static_cast<size_t>(end - text.data()));
    out = value;
    return true;
}

bool size_after(const std::string_view text, const std::string_view label, int& width,
                int& height) noexcept {
    const size_t at = text.find(label);
    if (at == std::string_view::npos)
        return false;
    std::string_view rest = text.substr(at + label.size());
    while (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);
    int w = 0;
    int h = 0;
    if (!take_int(rest, w))
        return false;
    while (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);
    if (rest.empty() || rest.front() != 'x')
        return false;
    rest.remove_prefix(1);
    while (!rest.empty() && rest.front() == ' ')
        rest.remove_prefix(1);
    if (!take_int(rest, h) || w < 2 || h < 2)
        return false;
    width = w;
    height = h;
    return true;
}

} // namespace

bool parse_wm_size(const std::string_view text, int& width, int& height) noexcept {
    return size_after(text, "Override size:", width, height) ||
           size_after(text, "Physical size:", width, height);
}

bool parse_wm_physical_size(const std::string_view text, int& width, int& height) noexcept {
    return size_after(text, "Physical size:", width, height);
}

bool parse_wm_density(const std::string_view text, int& density) noexcept {
    for (const std::string_view label : {"Override density:", "Physical density:"}) {
        const size_t at = text.find(label);
        if (at == std::string_view::npos)
            continue;
        std::string_view rest = text.substr(at + label.size());
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        int value = 0;
        if (take_int(rest, value) && value >= 40) {
            density = value;
            return true;
        }
    }
    return false;
}

bool parse_display_dpi(const std::string_view text, double& x, double& y) noexcept {
    size_t at = 0;
    while ((at = text.find(" dpi", at)) != std::string_view::npos) {
        // Walk back over "<x> x <y>".
        size_t begin = at;
        while (begin > 0 &&
               (text[begin - 1] == '.' || text[begin - 1] == ' ' || text[begin - 1] == 'x' ||
                (text[begin - 1] >= '0' && text[begin - 1] <= '9')))
            --begin;
        const std::string_view pair = trimmed(text.substr(begin, at - begin));
        const size_t middle = pair.find(" x ");
        at += 4;
        if (middle == std::string_view::npos)
            continue;
        const std::string first(pair.substr(0, middle));
        const std::string second(pair.substr(middle + 3));
        char* end = nullptr;
        const double dx = std::strtod(first.c_str(), &end);
        if (end == first.c_str())
            continue;
        const double dy = std::strtod(second.c_str(), &end);
        if (end == second.c_str() || dx < 10.0 || dy < 10.0)
            continue;
        x = dx;
        y = dy;
        return true;
    }
    return false;
}

int parse_rotation(const std::string_view text) noexcept {
    // Only the field docs/MATH.md measured: the other orientation fields
    // of dumpsys (the display's natural one, the window service's) are not
    // the current rotation.
    constexpr std::string_view label = "mCurrentOrientation=";
    const size_t at = text.find(label);
    if (at == std::string_view::npos || at + label.size() >= text.size())
        return -1;
    const char digit = text[at + label.size()];
    return digit >= '0' && digit <= '3' ? (digit - '0') * 90 : -1;
}

std::vector<std::string> fill_device(const PhoneFacts& facts, DeviceConfig& device) {
    std::vector<std::string> missing;
    if (facts.width >= 2 && facts.height >= 2) {
        device.width = facts.width;
        device.height = facts.height;
    } else {
        missing.emplace_back("the resolution");
    }
    if (facts.rotation >= 0)
        device.rotation = facts.rotation;
    else
        missing.emplace_back("the rotation");
    // The dpi belongs to the panel's own pixels: a size the user overrode
    // does not change the glass. Each axis has its own dpi.
    if (facts.physical_width >= 2 && facts.physical_height >= 2 && facts.dpi_x > 0.0)
        device.diagonal_inch =
            std::hypot(facts.physical_width / facts.dpi_x,
                       facts.physical_height / (facts.dpi_y > 0.0 ? facts.dpi_y : facts.dpi_x));
    else
        missing.emplace_back("the physical size");
    if (facts.has_pointer_speed)
        device.pointer_speed = facts.pointer_speed;
    if (!facts.accel_key.empty() && (facts.accel_before == "0" || facts.accel_before == "1"))
        device.accel = facts.accel_off ? AccelSetting::off : AccelSetting::on;
    else
        missing.emplace_back("whether pointer acceleration is on");
    // The density Android lays out with, and the first segment of its
    // acceleration curve at this pointer speed.
    if (facts.density > 0)
        device.density_dpi = facts.density;
    // The gain follows from those two; a gain typed earlier would keep the
    // old pointer speed's.
    if (facts.density > 0 && facts.has_pointer_speed)
        device.gain = 0.0;
    else
        missing.emplace_back("the gain");
    return missing;
}

Adb::Adb(std::string path) : path_(path.empty() ? std::string("adb") : std::move(path)) {}

bool Adb::available() const {
    const RunResult result = run_process({path_, "version"}, command_timeout_ms);
    return result.started && result.exit_code == 0;
}

void Adb::kill_server() const {
    static_cast<void>(run_process({path_, "kill-server"}, command_timeout_ms));
}

bool Adb::connect(const uint16_t port, std::string& note, const std::atomic<bool>* cancel,
                  bool* created) const {
    using clock = std::chrono::steady_clock;
    const clock::time_point deadline = clock::now() + std::chrono::milliseconds(usable_timeout_ms);
    const auto wait_more = [&] {
        if (clock::now() >= deadline || (cancel != nullptr && cancel->load()))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(usable_poll_ms));
        return true;
    };
    // The first time, the phone shows its "Allow USB debugging" dialog and
    // adb gives up on the handshake until it is answered: connecting is
    // tried again for as long as that can still be what is wanted.
    for (;;) {
        const RunResult connected =
            run_process({path_, "connect", serial(port)}, connect_timeout_ms);
        if (!connected.started) {
            note = "adb could not be run";
            return false;
        }
        // "connected to" and "already connected to" are the two successes;
        // adb exits 0 for a refused connection as well.
        if (connected.output.find("connected to") != std::string::npos) {
            if (created != nullptr)
                *created = connected.output.find("already connected") == std::string::npos;
            break;
        }
        const bool refused = connected.output.find("authenticate") != std::string::npos ||
                             connected.output.find("unauthorized") != std::string::npos;
        if (!refused) {
            note =
                connected.timed_out ? "adb did not answer" : std::string(trimmed(connected.output));
            return false;
        }
        note = "the phone did not accept this computer: allow USB debugging on its screen "
               "(tick Always allow), then fill again";
        if (!wait_more())
            return false;
    }
    // The handshake takes a moment: wait for the state to become "device".
    std::string answer;
    for (;;) {
        const RunResult state =
            run_process({path_, "-s", serial(port), "get-state"}, command_timeout_ms);
        if (!state.started) {
            note = "adb could not be run";
            return false;
        }
        answer = std::string(trimmed(state.output));
        if (answer == "device")
            return true;
        if (!wait_more())
            break;
    }
    // Never usable: leave no half-open connection behind for the next try.
    disconnect(port);
    if (answer.find("unauthorized") != std::string::npos)
        note = "allow USB debugging on the phone, then fill again";
    else
        note = answer.empty() ? "the phone did not answer through adb" : answer;
    return false;
}

void Adb::disconnect(const uint16_t port) const {
    static_cast<void>(run_process({path_, "disconnect", serial(port)}, command_timeout_ms));
}

std::string Adb::shell(const uint16_t port, const std::string& command, bool& ok) const {
    const RunResult result =
        run_process({path_, "-s", serial(port), "shell", command}, command_timeout_ms);
    ok = result.started && !result.timed_out && result.exit_code == 0;
    return result.output;
}

PhoneFacts Adb::inspect(const uint16_t port) const {
    PhoneFacts facts;
    bool ok = false;
    const std::string size = shell(port, "wm size", ok);
    if (!ok) {
        facts.note = "adb shell did not answer";
        return facts;
    }
    facts.reachable = true;
    static_cast<void>(parse_wm_size(size, facts.width, facts.height));
    static_cast<void>(parse_wm_physical_size(size, facts.physical_width, facts.physical_height));
    const std::string model = shell(port, "getprop ro.product.model", ok);
    if (ok)
        facts.model = std::string(trimmed(model));

    const std::string display = shell(port, "dumpsys display", ok);
    if (ok) {
        static_cast<void>(parse_display_dpi(display, facts.dpi_x, facts.dpi_y));
        facts.rotation = parse_rotation(display);
    }

    std::string speed = shell(port, "settings get system pointer_speed", ok);
    std::string_view speed_text = trimmed(speed);
    bool negative = false;
    if (!speed_text.empty() && speed_text.front() == '-') {
        negative = true;
        speed_text.remove_prefix(1);
    }
    int value = 0;
    if (ok && take_int(speed_text, value) && value <= 7) {
        facts.has_pointer_speed = true;
        facts.pointer_speed = negative ? -value : value;
    }

    static_cast<void>(parse_wm_density(shell(port, "wm density", ok), facts.density));

    int sdk = 0;
    std::string sdk_text = shell(port, "getprop ro.build.version.sdk", ok);
    std::string_view sdk_view = trimmed(sdk_text);
    static_cast<void>(take_int(sdk_view, sdk));

    // A vendor switch, where there is one, is what the phone obeys; the AOSP
    // key can be written on any phone but only Android 16 reads it.
    const std::string vendor(
        trimmed(shell(port, std::string("settings get system ") + vendor_accel_key, ok)));
    const bool has_vendor = ok && (vendor == "0" || vendor == "1");
    facts.accel_key = has_vendor ? vendor_accel_key : accel_key;
    facts.accel_gain_known = has_vendor;
    const std::string get = "settings get system " + facts.accel_key;
    const std::string before(trimmed(shell(port, get, ok)));
    if (ok && (before == "0" || before == "1"))
        facts.accel_before = before;
    if (!has_vendor && sdk < aosp_accel_sdk) {
        facts.accel_key.clear();
        return facts;
    }
    facts.accel_off = before == "0";
    return facts;
}

} // namespace aoas
