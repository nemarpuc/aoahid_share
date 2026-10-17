// SPDX-License-Identifier: MIT
#include "actions.hpp"

#include "app.hpp"
#include "look.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace aoas;

namespace gui {
namespace {

std::string answer_text(const std::string& answer) {
    return answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer;
}

// Sends a command and says in the footer how it went.
void run(App& app, const std::string& line) {
    const std::string answer = command(app, line);
    app.failed = answer != "ok";
    app.message = app.failed ? answer_text(answer) : line;
    poll_daemon(app);
}

// A path with a button that opens its folder.
void path_row(const char* label, const std::string& path, const std::filesystem::path& folder) {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(look::px(150));
    ImGui::TextUnformatted(path.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open folder"))
        open_in_file_manager(folder);
    ImGui::PopID();
}

// Reads a file of the export format into `imported`. Empty on success, else why not.
std::string read_export(const App& app, Config& imported) {
    std::string text;
    if (!read_file(app.transfer_path, text))
        return "That file could not be read.";
    const std::string problem = parse_config(text, imported);
    return problem.empty() ? std::string() : "Not imported: " + problem;
}

void computer_actions(App& app) {
    look::heading("Actions");
    ImGui::BeginDisabled(!app.daemon_running);
    const auto state = app.status.find("state");
    const bool paused = state != app.status.end() && state->second == "paused";
    if (ImGui::Button(paused ? "Resume" : "Pause"))
        run(app, paused ? "resume" : "pause");
    ImGui::SameLine();
    if (ImGui::Button("Resync"))
        run(app, "resync");
    ImGui::SameLine();
    if (ImGui::Button("Bring the input back"))
        run(app, "release");
    ImGui::SameLine();
    if (ImGui::Button("Count mouse buttons")) {
        const std::string answer = command(app, "buttons");
        app.failed = answer.rfind("buttons=", 0) != 0;
        app.message =
            app.failed ? answer_text(answer) : "your mice have " + answer.substr(8) + " buttons";
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop the daemon"))
        app.quitting = true;
    ImGui::EndDisabled();
}

void files_block(App& app) {
    look::heading("Files");
    const std::filesystem::path state_folder =
        std::filesystem::path(state_file_path()).parent_path();
    path_row("settings", config_file_path(),
             std::filesystem::path(config_file_path()).parent_path());
    path_row("devices", devices_dir(), devices_dir());
    path_row("daemon log", (state_folder / "daemon.log").string(), state_folder);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("export / import");
    ImGui::SameLine(look::px(150));
    ImGui::SetNextItemWidth(look::px(320));
    ImGui::InputTextWithHint("##transfer", "path of a file", &app.transfer_path);
    ImGui::SameLine();
    if (ImGui::Button("Export everything")) {
        app.failed = !write_file(app.transfer_path, format_config(app.config));
        app.message = app.failed ? "That file could not be written." : "Exported.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
        Config imported;
        const std::string problem = read_export(app, imported);
        app.failed = !problem.empty();
        if (app.failed) {
            app.message = problem;
        } else {
            // adb is run by the daemon, so a file from elsewhere never chooses
            // it or what it is used for.
            keep_local_adb(imported, app.config);
            app.config = std::move(imported);
            app.device = 0;
            app.dirty = true;
            app.field_errors.clear();
            app.editing.clear();
            app.message = "Imported. Apply to use it.";
        }
    }
}

void device_buttons(App& app, DeviceConfig& device) {
    const std::string state = status_of(app, device, "state");
    const auto where = app.status.find("state");
    const auto active = app.status.find("active");
    const bool here = where != app.status.end() && where->second == "android" &&
                      active != app.status.end() && active->second == device.serial;
    ImGui::BeginDisabled(state != "ready");
    if (ImGui::Button(here ? "Bring the input back" : "Send the input here"))
        run(app, here ? std::string("release") : "enter " + device.serial);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Forget this device..."))
        app.forgetting = device.serial;
    ImGui::SameLine();
    if (ImGui::Button("Open folder"))
        open_in_file_manager(devices_dir());

    ImGui::BeginDisabled(state != "ready");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Media keys");
    for (const char* const key :
         {"previous", "play_pause", "next", "volume_down", "volume_up", "brightness_down",
          "brightness_up"}) {
        ImGui::SameLine();
        if (ImGui::Button(key))
            run(app, std::string("media ") + key + " " + device.serial);
    }
    ImGui::EndDisabled();
}

// Whether a device is open now, or wanted open (it is then opened again when
// it comes back).
bool is_open(const App& app, const DeviceConfig& device) {
    return status_of(app, device, "wanted") == "yes" || status_of(app, device, "state") == "ready";
}

void device_files(App& app, DeviceConfig& device) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("export / import");
    ImGui::SameLine(look::px(150));
    ImGui::SetNextItemWidth(look::px(320));
    ImGui::InputTextWithHint("##transfer", "path of a file", &app.transfer_path);
    ImGui::SameLine();
    if (ImGui::Button("Export this device")) {
        app.failed = !write_file(app.transfer_path, format_device(device));
        app.message = app.failed ? "That file could not be written." : "Exported.";
    }
    ImGui::SameLine();
    if (ImGui::Button("Import into this device")) {
        std::string text;
        DeviceConfig imported;
        std::string problem;
        if (!read_file(app.transfer_path, text))
            problem = "That file could not be read.";
        else if (const std::string objection = parse_device(text, imported); !objection.empty())
            problem = "Not imported: " + objection;
        app.failed = !problem.empty();
        if (app.failed) {
            app.message = problem;
        } else {
            // Which phone this is, and how its ADB proxy runs on this
            // computer, are not for a file from elsewhere to change.
            imported.serial = device.serial;
            imported.adb_proxy = device.adb_proxy;
            imported.adb_port = device.adb_port;
            device = std::move(imported);
            app.dirty = true;
            app.field_errors.clear();
            app.editing.clear();
            app.message = "Imported. Apply to use it.";
        }
    }
}

} // namespace

// The steps for a device that has no USB debugging: the cursor is stepped to
// the corner by raw counts, and the size follows from where it stops.
// Calibration wizard without ADB: measures exact gain by stepping to screen edges.
void draw_probe(App& app, DeviceConfig& device) {
    if (app.probing && app.probe_serial != device.serial)
        app.probing = false;

    look::heading("Screen Calibration Wizard (without ADB)");
    look::dim("Directly measures screen bounds by stepping the cursor to each edge.\n"
              "This sets Width, Height, and Gain = 1.0 without needing manual resolution lookup.\n"
              "Ensure pointer acceleration is OFF in Android settings. If you change pointer speed\n"
              "on your device later, counts will change and require re-calibration.");

    const std::string probe = "probe " + device.serial + " ";

    if (!app.probing) {
        ImGui::BeginDisabled(!app.daemon_running || device.serial.empty());
        if (ImGui::Button("Start Calibration Wizard")) {
            const std::string answer = command(app, probe + "corner");
            app.probing = answer == "ok";
            app.probe_step = 1;
            app.probe_serial = device.serial;
            app.failed = !app.probing;
            app.message = app.probing ? "probe corner" : answer_text(answer);
            app.probe_x = 0;
            app.probe_y = 0;
        }
        ImGui::EndDisabled();
        return;
    }

    if (app.probe_step == 1) {
        look::heading("Step 1/2: Measure Width (Horizontal)");
        look::dim("Tip: Pushing slightly past the edge (+5 to +10 counts) is recommended.\n"
                  "If counts are too few, the cursor may not reach the far edge to tap buttons.");

        ImGui::Spacing();
        look::dim("Move cursor down to see it clearly (recommended, does not affect measurement):");
        if (ImGui::Button("Down 100")) {
            command(app, probe + "move 0 100");
        }
        ImGui::SameLine();
        if (ImGui::Button("Up 100")) {
            command(app, probe + "move 0 -100");
        }

        ImGui::Spacing();
        look::dim("Move right until cursor reaches the right edge (+10 extra recommended):");

        for (const int step : {1, 10, 100}) {
            char label[16];
            std::snprintf(label, sizeof label, "%d", step);
            if (ImGui::Button(label, ImVec2(look::px(60), 0))) {
                if (command(app, probe + "move " + std::to_string(step) + " 0") == "ok")
                    app.probe_x += step;
            }
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Total counts: %d", app.probe_x);

        ImGui::Spacing();
        ImGui::BeginDisabled(app.probe_x <= 0);
        if (ImGui::Button("OK (Proceed to Height)##ok_step1")) {
            command(app, probe + "corner");
            app.probe_step = 2;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel##cancel_step1")) {
            app.probing = false;
        }
    } else if (app.probe_step == 2) {
        look::heading("Step 2/2: Measure Height (Vertical)");
        look::dim("Tip: Pushing slightly past the edge (+5 to +10 counts) is recommended.\n"
                  "If counts are too few, the cursor may not reach the bottom edge to tap buttons.");

        ImGui::Spacing();
        look::dim("Move cursor right to see it clearly (recommended, does not affect measurement):");
        if (ImGui::Button("Right 100")) {
            command(app, probe + "move 100 0");
        }
        ImGui::SameLine();
        if (ImGui::Button("Left 100")) {
            command(app, probe + "move -100 0");
        }

        ImGui::Spacing();
        look::dim("Move down until cursor reaches the bottom edge (+10 extra recommended):");

        for (const int step : {1, 10, 100}) {
            char label[16];
            std::snprintf(label, sizeof label, "%d", step);
            if (ImGui::Button(label, ImVec2(look::px(60), 0))) {
                if (command(app, probe + "move 0 " + std::to_string(step)) == "ok")
                    app.probe_y += step;
            }
            ImGui::SameLine();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Total counts: %d", app.probe_y);

        ImGui::Spacing();
        ImGui::BeginDisabled(app.probe_y <= 0);
        if (ImGui::Button("OK (Review and Save)##ok_step2")) {
            app.probe_step = 3;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel##cancel_step2")) {
            app.probing = false;
        }
    } else if (app.probe_step == 3) {
        look::heading("Calibration Result");

        if (device.rotation < 0)
            device.rotation = 0;

        const char* rotations[] = {"0° (Portrait)", "90° (Landscape)", "180° (Inverted)",
                                   "270° (Landscape flipped)"};
        int rot_idx = device.rotation == 90    ? 1
                      : device.rotation == 180 ? 2
                      : device.rotation == 270 ? 3
                                               : 0;
        ImGui::SetNextItemWidth(look::px(220));
        if (ImGui::Combo("Screen rotation", &rot_idx, rotations, IM_ARRAYSIZE(rotations))) {
            device.rotation = rot_idx == 1 ? 90 : rot_idx == 2 ? 180 : rot_idx == 3 ? 270 : 0;
        }

        const bool turned = device.rotation == 90 || device.rotation == 270;
        const int comp_w = (turned ? app.probe_y : app.probe_x) + 1;
        const int comp_h = (turned ? app.probe_x : app.probe_y) + 1;

        char buf[300];
        std::snprintf(buf, sizeof buf, "Measured counts: X = %d, Y = %d\n"
                                       "Calculated screen size: %d x %d (Gain = 1.0, Accel = off)\n\n"
                                       "Note: If you change pointer speed/settings on your device later,\n"
                                       "please re-run this calibration.",
                      app.probe_x, app.probe_y, comp_w, comp_h);
        look::dim(buf);

        ImGui::Spacing();
        if (ImGui::Button("Save and Apply")) {
            device.width = comp_w;
            device.height = comp_h;
            device.gain = 1.0;
            device.accel = AccelSetting::off;
            if (device.diagonal_inch <= 0.0)
                device.diagonal_inch = 6.5;
            app.probing = false;
            edited_directly(app, device.serial, "detected");
        }
        ImGui::SameLine();
        if (ImGui::Button("Restart Wizard")) {
            command(app, probe + "corner");
            app.probe_step = 1;
            app.probe_x = 0;
            app.probe_y = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##cancel_step3")) {
            app.probing = false;
        }
    }
}

void draw_actions(App& app) { computer_actions(app); }

void draw_files(App& app) { files_block(app); }

void draw_device_tools(App& app, DeviceConfig& device) {
    device_buttons(app, device);
    ImGui::Spacing();
    device_files(app, device);
}

float sidebar_buttons_height(const App& app) {
    const float one = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    if (!app.daemon_running)
        return one;
    return app.computer || !has_selection(app) ? one * 2.0F : one * 1.0F;
}

void draw_sidebar_buttons(App& app) {
    const ImVec2 full(-1.0F, 0.0F);
    if (!app.daemon_running) {
        if (ImGui::Button("Start the daemon", full))
            start_daemon(app);
        return;
    }
    if (app.computer || !has_selection(app)) {
        bool any_open = false;
        for (const DeviceConfig& device : app.config.devices)
            any_open = any_open || is_open(app, device);
        ImGui::BeginDisabled(!any_open);
        if (ImGui::Button("Disconnect all", full)) {
            int count = 0;
            for (const DeviceConfig& device : app.config.devices) {
                if (is_open(app, device) && command(app, "disconnect " + device.serial) == "ok")
                    ++count;
            }
            app.failed = false;
            app.message = "Disconnected " + std::to_string(count) + " device(s).";
            poll_daemon(app);
        }
        ImGui::EndDisabled();
        if (ImGui::Button("Stop the daemon", full))
            app.quitting = true;
        return;
    }
    const DeviceConfig& device = app.config.devices[app.device];
    const bool open = is_open(app, device);
    const bool can_connect = device.enabled && !device.serial.empty();
    // An open device can always be closed, whatever "In use" says now.
    ImGui::BeginDisabled(device.serial.empty() || (!open && !can_connect));
    if (ImGui::Button(open ? "Disconnect" : "Connect", full))
        run(app, std::string(open ? "disconnect " : "connect ") + device.serial);
    ImGui::EndDisabled();
}

void draw_confirmations(App& app) {
    if (!app.forgetting.empty()) {
        ImGui::OpenPopup("Forget this device?");
        ImGui::SetNextWindowSize(ImVec2(look::px(420), 0));
        if (ImGui::BeginPopupModal("Forget this device?", nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            const size_t index = find_device(app.config, app.forgetting);
            const std::string name = index < app.config.devices.size()
                                         ? label_of(app.config.devices[index])
                                         : app.forgetting;
            ImGui::PushTextWrapPos(0.0F);
            ImGui::Text("%s and its file are removed. Plugged in again it is a new device.",
                        name.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Forget it", ImVec2(look::px(120), 0))) {
                if (app.dirty) {
                    // The files are read again after it, which would drop the edits.
                    app.message = "Apply or revert your changes first.";
                    app.failed = true;
                } else if (!app.config_error.empty()) {
                    // What is shown is not what is on disk, so nothing is written over it.
                    app.message = "The settings files could not be read; nothing was changed.";
                    app.failed = true;
                } else if (index < app.config.devices.size()) {
                    const std::string serial = app.config.devices[index].serial;
                    if (app.daemon_running) {
                        const std::string answer = command(app, "forget " + serial);
                        app.failed = answer != "ok";
                        app.message = app.failed ? answer_text(answer) : "Device removed.";
                    } else {
                        app.config.devices.erase(app.config.devices.begin() +
                                                 static_cast<std::ptrdiff_t>(index));
                        // Whatever sat beside it goes back to a monitor's edge.
                        for (DeviceConfig& other : app.config.devices) {
                            if (other.beside == serial)
                                other.beside.clear();
                        }
                        app.failed = !save_store(app.config);
                        app.message =
                            app.failed ? "The config could not be written." : "Device removed.";
                    }
                    load_config(app);
                    poll_daemon(app);
                    app.device = 0;
                }
                app.forgetting.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep it", ImVec2(look::px(120), 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                app.forgetting.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    if (app.quitting) {
        ImGui::OpenPopup("Stop the daemon?");
        ImGui::SetNextWindowSize(ImVec2(look::px(420), 0));
        if (ImGui::BeginPopupModal("Stop the daemon?", nullptr,
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted("Every device is closed and the input stays on this computer "
                                   "until the daemon is started again.");
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Stop it", ImVec2(look::px(120), 0))) {
                const std::string answer = command(app, "quit");
                app.failed = answer != "ok";
                app.message = app.failed ? answer_text(answer) : "The daemon was stopped.";
                app.quitting = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(look::px(120), 0)) ||
                ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                app.quitting = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

} // namespace gui
