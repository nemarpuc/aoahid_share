// SPDX-License-Identifier: MIT
#include "page.hpp"

#include "actions.hpp"
#include "aoahid_share/accel_model.hpp"
#include "aoahid_share/keys.hpp"
#include "app.hpp"
#include "field.hpp"
#include "look.hpp"
#include "placement.hpp"
#include "status_view.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

using namespace aoas;

namespace gui {
namespace {

constexpr const char* const computer_tabs[] = {"General", "Motion", "ADB", "Status", "Tools"};
constexpr const char* const device_tabs[] = {"Device", "HID",    "Placement", "Screen",
                                             "Motion", "Status", "Reports",   "Tools"};

// Every row of a heading of the tables that belongs on this page.
void draw_group(App& app, const char* const group, DeviceConfig* const device) {
    for (const ConfigKey& key : config_keys()) {
        if (std::strcmp(key.group, group) != 0)
            continue;
        if (device != nullptr ? key.scope == KeyScope::computer : key.scope == KeyScope::device)
            continue;
        field(app, key, device);
    }
}

// One row by its place in the files, under a name that says what it is.
void draw_row(App& app, const KeyScope scope, const char* const section, const char* const key,
              DeviceConfig* const device, const char* const label) {
    if (const ConfigKey* const found = find_config_key(scope, section, key))
        field(app, *found, device, label);
}

// How to reach the proxy once it runs.
void draw_proxy_command(App& app, const DeviceConfig& device) {
    const std::string port = status_of(app, device, "proxy");
    // Only a plain port number goes into a command line to copy.
    if (port.empty() || port.size() > 5 ||
        port.find_first_not_of("0123456789") != std::string::npos)
        return;
    const std::string line = "adb connect 127.0.0.1:" + port;
    look::row("Reach it with", nullptr, look::px(260));
    ImGui::TextUnformatted(line.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy"))
        ImGui::SetClipboardText(line.c_str());
    look::row_end();
}

// What the gain in use comes to, with the numbers it is made of.
void draw_gain_line(const DeviceConfig& device) {
    const double in_use = effective_gain(device);
    if (in_use <= 0.0) {
        look::dim("Gain in use: not known yet (enter the density, or fill from adb).");
        return;
    }
    char text[240];
    if (device.gain > 0.0) {
        std::snprintf(text, sizeof text, "Gain in use: %.4g px per count (typed).", in_use);
    } else {
        std::snprintf(text, sizeof text, "Gain in use: %d / 320 x %.4g x %.4g = %.4g px per count.",
                      device.density_dpi, pointer_speed_factor(device.pointer_speed),
                      first_segment_gain, in_use);
    }
    look::dim(text);
}

void draw_device_screen(App& app, DeviceConfig& device) {
    const std::string proxy = status_of(app, device, "proxy");
    const bool proxy_ready = !proxy.empty() && proxy != "off";
    ImGui::BeginDisabled(!app.daemon_running || !proxy_ready || !app.filling.empty());
    if (ImGui::Button("Fill from ADB"))
        start_fill(app, device.serial);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!proxy_ready)
        look::dim("Connect the device and turn its ADB proxy on first.");
    else
        look::dim("Reads these from the device through its ADB proxy.");
    ImGui::Spacing();
    draw_group(app, "Android screen", &device);
    draw_gain_line(device);
    draw_probe(app, device);
}

void draw_device_page(App& app, DeviceConfig& device) {
    switch (app.device_tab) {
    case 0:
        draw_row(app, KeyScope::device, "device", "serial", &device, "Serial number");
        draw_row(app, KeyScope::device, "device", "name", &device, "Name");
        draw_row(app, KeyScope::device, "device", "enabled", &device, "In use");
        draw_row(app, KeyScope::device, "device", "hotkey", &device, "Hotkey to this device");
        draw_row(app, KeyScope::device, "adb", "proxy", &device, "ADB proxy");
        draw_row(app, KeyScope::device, "adb", "port", &device, "ADB proxy port");
        draw_proxy_command(app, device);
        break;
    case 1:
        draw_row(app, KeyScope::device, "mouse", "enabled", &device, "Mouse");
        draw_row(app, KeyScope::device, "keyboard", "enabled", &device, "Keyboard");
        draw_row(app, KeyScope::device, "media", "enabled", &device, "Media keys");
        look::heading("Touchscreen");
        look::dim("The left click becomes a tap where the cursor is, and the wheel swipes.");
        look::dim("Needs the width, height and rotation of the device (Screen tab).");
        look::dim(
            "Swipe rows: the wheel swipes a second finger; they do not change the mouse scroll.");
        draw_row(app, KeyScope::device, "touch", "enabled", &device, "Touch mode");
        draw_row(app, KeyScope::device, "touch", "tap_button", &device, "Tap button");
        draw_row(app, KeyScope::device, "touch", "swipe", &device, "Swipe with the wheel");
        if (touch_of(app.config, device).swipe) {
            draw_row(app, KeyScope::device, "touch", "scroll", &device, "Swipe distance per notch");
            draw_row(app, KeyScope::device, "touch", "scroll_pan", &device,
                     "Swipe distance per notch, sideways");
            draw_row(app, KeyScope::device, "touch", "scroll_start_ms", &device,
                     "Swipe start delay (0 = next report)");
            draw_row(app, KeyScope::device, "touch", "scroll_steps", &device,
                     "Swipe steps per notch");
            draw_row(app, KeyScope::device, "touch", "scroll_total_ms", &device,
                     "Swipe time, first to last step");
            draw_row(app, KeyScope::device, "touch", "scroll_overlap", &device,
                     "Next notch during a swipe");
            draw_row(app, KeyScope::device, "touch", "scroll_release_ms", &device,
                     "Swipe lift delay (0 = next report)");
        }
        break;
    case 2:
        draw_placement(app, device);
        break;
    case 3:
        draw_device_screen(app, device);
        break;
    case 4:
        draw_group(app, "Motion", &device);
        break;
    case 5:
        draw_status(app, &device);
        break;
    case 6:
        draw_reports(app, device);
        break;
    default:
        draw_device_tools(app, device);
        break;
    }
}

void draw_computer_page(App& app) {
    switch (app.computer_tab) {
    case 0:
        draw_group(app, "General", nullptr);
        draw_row(app, KeyScope::computer, "mouse", "buttons", nullptr, "Mouse buttons");
        draw_row(app, KeyScope::computer, "mouse", "button_map", nullptr, "Button map");
        draw_row(app, KeyScope::computer, "mouse", "enabled", nullptr, "Mouse");
        draw_row(app, KeyScope::computer, "keyboard", "enabled", nullptr, "Keyboard");
        draw_row(app, KeyScope::computer, "media", "enabled", nullptr, "Media keys");
        look::heading("Touchscreen");
        draw_row(app, KeyScope::computer, "touch", "enabled", nullptr, "Touch mode");
        draw_row(app, KeyScope::computer, "touch", "tap_button", nullptr, "Tap button");
        draw_row(app, KeyScope::computer, "touch", "swipe", nullptr, "Swipe with the wheel");
        if (app.config.touch.swipe) {
            draw_row(app, KeyScope::computer, "touch", "scroll", nullptr,
                     "Swipe distance per notch");
            draw_row(app, KeyScope::computer, "touch", "scroll_pan", nullptr,
                     "Swipe distance per notch, sideways");
            draw_row(app, KeyScope::computer, "touch", "scroll_start_ms", nullptr,
                     "Swipe start delay (0 = next report)");
            draw_row(app, KeyScope::computer, "touch", "scroll_steps", nullptr,
                     "Swipe steps per notch");
            draw_row(app, KeyScope::computer, "touch", "scroll_total_ms", nullptr,
                     "Swipe time, first to last step");
            draw_row(app, KeyScope::computer, "touch", "scroll_overlap", nullptr,
                     "Next notch during a swipe");
            draw_row(app, KeyScope::computer, "touch", "scroll_release_ms", nullptr,
                     "Swipe lift delay (0 = next report)");
        }
        look::heading("Media");
        draw_group(app, "Media", nullptr);
        break;
    case 1:
        draw_group(app, "Motion", nullptr);
        break;
    case 2:
        draw_row(app, KeyScope::computer, "adb", "kill_server", nullptr,
                 "Stop a running adb server");
        draw_row(app, KeyScope::computer, "adb", "path", nullptr, "The adb program");
        draw_row(app, KeyScope::computer, "adb", "first_port", nullptr, "First proxy port");
        break;
    case 3:
        draw_status(app, nullptr);
        break;
    default:
        draw_actions(app);
        draw_files(app);
        break;
    }
}

void draw_main(App& app) {
    if (!app.config_error.empty()) {
        ImGui::TextWrapped("ERR: the settings files could not be read: %s. What is shown is not "
                           "what is on disk, so nothing is saved over it.",
                           app.config_error.c_str());
    }
    const bool device_page = !app.computer && has_selection(app);
    DeviceConfig* const device = device_page ? &app.config.devices[app.device] : nullptr;
    ImGui::TextUnformatted(device == nullptr        ? "Computer"
                           : device->serial.empty() ? "A profile that waits for a device"
                                                    : label_of(*device).c_str());
    draw_summary(app, device);
    ImGui::Spacing();
    ImGui::PushID("tabs");
    if (device != nullptr) {
        app.device_tab =
            look::choose(device_tabs, static_cast<int>(std::size(device_tabs)), app.device_tab);
    } else {
        app.computer_tab = look::choose(computer_tabs, static_cast<int>(std::size(computer_tabs)),
                                        app.computer_tab);
    }
    ImGui::PopID();
    ImGui::Spacing();
    if (device != nullptr)
        draw_device_page(app, *device);
    else
        draw_computer_page(app);
}

void draw_header(const App& app) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(app.daemon_running ? "daemon: running" : "daemon: not running");
    const auto backend = app.status.find("backend");
    if (backend != app.status.end()) {
        ImGui::SameLine();
        ImGui::Text("   backend: %s", backend->second.c_str());
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(app.dirty ? "   * unsaved changes" : "");
}

void draw_list(App& app) {
    if (ImGui::Selectable("Computer", app.computer || !has_selection(app)))
        app.computer = true;
    ImGui::Separator();
    if (!app.daemon_running) {
        look::dim("Start the daemon to see the devices.");
        return;
    }
    const float side = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();
    look::dim("devices");
    ImGui::SameLine();
    ImGui::SetCursorPosX(
        std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - side));
    if (look::icon_button_refresh("##rescan_devices", "Search for devices"))
        rescan_devices(app);
    std::vector<size_t> connected_indices;
    std::vector<size_t> disconnected_indices;
    for (size_t index = 0; index < app.config.devices.size(); ++index) {
        const DeviceConfig& device = app.config.devices[index];
        const bool plugged = status_of(app, device, "plugged") == "yes" ||
                             status_of(app, device, "state") == "ready" ||
                             status_of(app, device, "state") == "error";
        if (plugged)
            connected_indices.push_back(index);
        else
            disconnected_indices.push_back(index);
    }

    if (connected_indices.empty() && app.fresh.empty())
        look::dim("No devices connected.");

    for (const size_t index : connected_indices) {
        const DeviceConfig& device = app.config.devices[index];
        const std::string label =
            std::string(look::mark(status_of(app, device, "state"), device.enabled)) + " " +
            (device.serial.empty() ? std::string("(waits for a device)") : label_of(device));
        ImGui::PushID(static_cast<int>(index));
        if (ImGui::Selectable(label.c_str(), has_selection(app) && app.device == index)) {
            app.computer = false;
            app.device = index;
        }
        ImGui::PopID();
    }
    if (!app.fresh.empty()) {
        if (!connected_indices.empty())
            ImGui::Separator();
        look::dim("plugged in, not set up");
        for (const NewDevice& fresh : app.fresh) {
            const std::string label = "+ " + (fresh.product.empty() ? fresh.serial : fresh.product);
            ImGui::PushID(fresh.serial.c_str());
            if (ImGui::Selectable(label.c_str()))
                connect_device(app, fresh.serial);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s: add it and open it", fresh.serial.c_str());
            ImGui::PopID();
        }
    }
    if (!disconnected_indices.empty()) {
        ImGui::Spacing();
        if (ImGui::TreeNode("Disconnected")) {
            for (const size_t index : disconnected_indices) {
                const DeviceConfig& device = app.config.devices[index];
                const std::string label =
                    std::string(look::mark(status_of(app, device, "state"), device.enabled)) + " " +
                    (device.serial.empty() ? std::string("(waits for a device)") : label_of(device));
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(label.c_str(), has_selection(app) && app.device == index)) {
                    app.computer = false;
                    app.device = index;
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
    }
}

void draw_left(App& app) {
    const float buttons = sidebar_buttons_height(app);
    ImGui::BeginChild("list", ImVec2(0, -buttons));
    draw_list(app);
    ImGui::EndChild();
    draw_sidebar_buttons(app);
}

void draw_footer(App& app) {
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    if (app.message.empty())
        look::dim("ready");
    else
        ImGui::TextUnformatted(((app.failed ? "ERR: " : "OK: ") + app.message).c_str());

    // Saving is at the bottom right, where a form's buttons are.
    const float padding = ImGui::GetStyle().FramePadding.x * 2.0F;
    const float discard_width = ImGui::CalcTextSize("Discard changes").x + padding;
    const float apply_width = ImGui::CalcTextSize("Apply").x + padding;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - discard_width - apply_width -
                    ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Discard changes")) {
        load_config(app);
        app.failed = false;
        app.message = "Changes discarded.";
    }
    ImGui::SameLine();
    // Not held back by "unsaved": the first edit of a row is committed in the same
    // frame a click on this button starts, after this has been drawn.
    ImGui::BeginDisabled(!app.config_error.empty() || !app.field_errors.empty() || app.stale);
    if (ImGui::Button("Apply"))
        apply(app);
    ImGui::EndDisabled();
}

} // namespace

void draw_window(App& app) {
    continue_fill(app);
    const ImGuiViewport* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_header(app);
    ImGui::Separator();
    const float footer = ImGui::GetFrameHeightWithSpacing() + look::px(10);
    ImGui::BeginChild("left", ImVec2(look::px(190), -footer), ImGuiChildFlags_Borders);
    draw_left(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("main", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    draw_main(app);
    ImGui::EndChild();
    draw_footer(app);
    draw_confirmations(app);
    ImGui::End();
}

} // namespace gui
