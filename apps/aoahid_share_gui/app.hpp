// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/config.hpp"
#include "aoahid_share/geometry.hpp"

#include <imgui.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

struct GLFWwindow;

namespace gui {

// How often the daemon's status is read while the window is shown. The
// daemon answers from a snapshot, so asking never holds up the input.
inline constexpr double poll_seconds = 0.25;

// A device that is plugged in but has no file yet.
struct NewDevice {
    std::string serial;
    std::string product;
};

// Everything the window shows: the config as it is being edited, and what
// the daemon last said. It holds no setting of its own; "Apply" writes the
// config and asks the daemon to read it again.
struct App {
    aoas::Config config;
    // Non-empty: the files on disk could not be read. What is shown is not
    // what is on disk, so nothing is saved over it.
    std::string config_error;
    // The daemon's count of the times it wrote a device's file, as last seen.
    std::string files_version;
    bool dirty{};
    // The daemon wrote a file that the window also holds edits of: saving them
    // would put the old ones back, so they have to be discarded first.
    bool stale{};

    // A row that was typed and refused, by row, with the parser's reason; and
    // the text being typed in a row.
    std::map<std::string, std::string> field_errors;
    std::map<std::string, std::string> editing;

    std::map<std::string, std::string> status;
    std::vector<aoas::Monitor> monitors;
    std::vector<NewDevice> fresh;
    bool daemon_running{};
    double next_poll{};

    // What is open: the computer's page, or the page of one device, and which
    // part of each page.
    bool computer{true};
    size_t device{};
    int computer_tab{};
    int device_tab{};
    // Asked once before a device and its file are removed, and before the
    // daemon is stopped.
    std::string forgetting;
    bool quitting{};

    // The line at the bottom, and whether it reports a failure.
    std::string message;
    bool failed{};

    // The device being filled from adb, and when to stop waiting for it.
    std::string filling;
    double filling_until{};

    // Setting a device up by eye: counts stepped from its top left corner.
    bool probing{};
    std::string probe_serial;
    int probe_x{};
    int probe_y{};
    double probe_diagonal{};

    // Dragging a device in the picture: where along it the mouse took hold.
    int drag_offset{};
    std::string transfer_path;

    std::filesystem::path program_dir;
};

// app.cpp
void load_config(App& app);
void poll_daemon(App& app);
void apply(App& app);
// Sends one command; the answer without its line end, or why none came.
std::string command(App& app, const std::string& line);
void start_daemon(App& app);
void rescan_devices(App& app);
void connect_device(App& app, const std::string& serial);
void start_fill(App& app, const std::string& serial);
void continue_fill(App& app);
// What the daemon reports about a device, by its serial.
[[nodiscard]] std::string status_of(const App& app, const aoas::DeviceConfig& device,
                                    const char* key);
// Whether a device's page is open. Devices are shown only while the daemon runs.
[[nodiscard]] bool has_selection(const App& app);
[[nodiscard]] std::vector<aoas::Monitor> toolkit_monitors();
[[nodiscard]] std::filesystem::path program_directory();
void open_in_file_manager(const std::filesystem::path& path);

// A value set by something other than its own row (the picture, the probe):
// what was typed in that device's rows of `section`, and a refusal they hold,
// no longer say what the config says. Marks the config as edited.
void edited_directly(App& app, const std::string& serial, const std::string& section);

// layout_view.cpp
void draw_layout(App& app);
// The start and length the picture works out for a device, in the pixels of
// what it sits beside: what an unset one comes to. False if it cannot be placed.
[[nodiscard]] bool effective_placement(const App& app, size_t device, int& start, int& length);

} // namespace gui
