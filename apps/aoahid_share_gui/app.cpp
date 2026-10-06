// SPDX-License-Identifier: MIT
#include "app.hpp"

#include "control.hpp"
#include "paths.hpp"
#include "process.hpp"
#include "store.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace aoas;

namespace gui {

void load_config(App& app) {
    // The page that is open is kept by its serial: the files are sorted, so
    // a device added or removed before it moves its index.
    const std::string open_serial =
        app.device < app.config.devices.size() ? app.config.devices[app.device].serial : "";
    Config loaded;
    app.config_error = load_store(loaded);
    if (app.config_error.empty())
        app.config = std::move(loaded);
    app.dirty = false;
    app.stale = false;
    app.field_errors.clear();
    app.editing.clear();
    const size_t again =
        open_serial.empty() ? app.config.devices.size() : find_device(app.config, open_serial);
    if (again < app.config.devices.size())
        app.device = again;
    else if (app.device >= app.config.devices.size())
        app.device = 0;
}

// The monitors as the toolkit sees them, for the picture while no daemon is
// there to report its own. Their names need not be the daemon's.
std::vector<Monitor> toolkit_monitors() {
    std::vector<Monitor> result;
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    for (int index = 0; index < count; ++index) {
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[index]);
        if (mode == nullptr)
            continue;
        Monitor monitor;
        const char* name = glfwGetMonitorName(monitors[index]);
        monitor.name = name != nullptr ? name : "";
        glfwGetMonitorPos(monitors[index], &monitor.rect.x, &monitor.rect.y);
        // A Wayland video mode is in device pixels; positions are logical.
        float scale_x = 1.0F;
        float scale_y = 1.0F;
        if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
            glfwGetMonitorContentScale(monitors[index], &scale_x, &scale_y);
        monitor.rect.w = static_cast<int>(
            std::lround(static_cast<float>(mode->width) / std::max(scale_x, 0.25F)));
        monitor.rect.h = static_cast<int>(
            std::lround(static_cast<float>(mode->height) / std::max(scale_y, 0.25F)));
        int width_mm = 0;
        int height_mm = 0;
        glfwGetMonitorPhysicalSize(monitors[index], &width_mm, &height_mm);
        monitor.width_mm = width_mm;
        monitor.height_mm = height_mm;
        if (monitor.rect.w > 0 && monitor.rect.h > 0)
            result.push_back(std::move(monitor));
    }
    return result;
}

void poll_daemon(App& app) {
    std::string answer;
    const bool was_running = app.daemon_running;
    app.daemon_running = control_request("status", answer);
    // Without the daemon the devices are not listed, so what was refused in
    // their rows cannot be reached to be corrected; those values stay as the
    // config has them.
    if (was_running && !app.daemon_running) {
        const auto device_row = [](const auto& row) { return row.first.rfind("*/", 0) != 0; };
        std::erase_if(app.field_errors, device_row);
        std::erase_if(app.editing, device_row);
    }
    // The "starting" note has served once it is up.
    if (!was_running && app.daemon_running && !app.failed)
        app.message.clear();
    app.status.clear();
    app.monitors.clear();
    app.fresh.clear();
    if (!app.daemon_running) {
        app.monitors = toolkit_monitors();
        return;
    }
    std::istringstream lines(answer);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        app.status[key] = value;
        if (key.rfind("monitor.", 0) == 0) {
            Monitor monitor;
            std::istringstream fields(value);
            fields >> monitor.name >> monitor.rect.x >> monitor.rect.y >> monitor.rect.w >>
                monitor.rect.h >> monitor.width_mm >> monitor.height_mm;
            if (monitor.rect.w > 0 && monitor.rect.h > 0)
                app.monitors.push_back(std::move(monitor));
        } else if (key.rfind("new.", 0) == 0) {
            app.fresh.push_back({key.substr(4), value});
        }
    }
    // The daemon wrote a device's file (a fill): what the window shows is
    // read again, unless it holds edits of its own, which are not overwritten.
    const std::string version =
        app.status.count("files_version") != 0 ? app.status["files_version"] : std::string();
    if (!version.empty() && version != app.files_version) {
        const bool first = app.files_version.empty();
        if (!app.dirty) {
            app.files_version = version;
            if (!first)
                load_config(app);
        } else if (!first) {
            app.files_version = version;
            app.failed = true;
            app.stale = true;
            app.message =
                "A device's file was changed by the daemon. Discard your changes to read it.";
        } else {
            app.files_version = version;
        }
    }
    // Still starting: it has no monitors to report yet.
    if (app.monitors.empty())
        app.monitors = toolkit_monitors();
}

namespace {

void drop_rows_with(App& app, const std::string& prefix) {
    const auto matches = [&](const auto& row) { return row.first.rfind(prefix, 0) == 0; };
    std::erase_if(app.editing, matches);
    std::erase_if(app.field_errors, matches);
}

} // namespace

void edited_directly(App& app, const std::string& serial, const std::string& section) {
    drop_rows_with(app, serial + "/" + section + ".");
    app.dirty = true;
}

void apply(App& app) {
    app.failed = true;
    if (!app.config_error.empty()) {
        app.message = "Not saved: the files on disk could not be read (" + app.config_error + ").";
        return;
    }
    if (app.stale) {
        app.message = "Not saved: a file was changed meanwhile. Discard changes to read it.";
        return;
    }
    if (!app.field_errors.empty()) {
        // A row's id starts with the serial of its device, or "*" for the computer's.
        const std::string where =
            app.field_errors.begin()->first.substr(0, app.field_errors.begin()->first.find('/'));
        app.message = "Not saved: a value is not valid. See the rows marked ERR on " +
                      (where == "*" ? std::string("the Computer page") : "device " + where) + ".";
        return;
    }
    const std::string problem = validate_config(app.config);
    if (!problem.empty()) {
        app.message = "Not saved: " + problem;
        return;
    }
    if (!save_store(app.config)) {
        app.message = "The config could not be written.";
        return;
    }
    app.dirty = false;
    app.failed = false;
    app.editing.clear();
    std::string answer;
    if (!control_request("reload", answer)) {
        app.message = "Saved. The daemon is not running.";
    } else if (answer.rfind("error=", 0) == 0) {
        app.message = answer.substr(6);
        app.failed = true;
    } else {
        app.message = "Saved and applied.";
    }
    app.next_poll = 0.0;
}

std::string command(App& app, const std::string& line) {
    std::string answer;
    if (!control_request(line, answer))
        return "The daemon is not running.";
    app.next_poll = 0.0;
    while (!answer.empty() && answer.back() == '\n')
        answer.pop_back();
    return answer;
}

void start_daemon(App& app) {
#ifdef _WIN32
    const std::filesystem::path program = app.program_dir / "aoahid_share_daemon.exe";
#else
    const std::filesystem::path program = app.program_dir / "aoahid_share_daemon";
#endif
    // The daemon outlives this window and writes its own log.
    app.failed = app.program_dir.empty() || !spawn_detached({program.string()});
    app.message = app.failed ? "Could not start " + program.string() : "Starting the daemon...";
    app.next_poll = ImGui::GetTime() + 1.0;
}

void rescan_devices(App& app) {
    const std::string answer = command(app, "rescan");
    app.failed = answer != "ok";
    app.message = app.failed ? (answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer)
                             : "Searched for devices.";
    poll_daemon(app);
}

// "Connect" on a plugged-in device: it gets a file of its own and is opened.
// Nothing is read from it and no other device is touched; its values are
// entered, or filled from adb once its proxy is on.
void connect_device(App& app, const std::string& serial) {
    app.failed = true;
    if (!app.daemon_running) {
        app.message = "Start the daemon first.";
        return;
    }
    // The daemon rewrites the config; nothing unsaved may be lost to that.
    if (app.dirty) {
        app.message = "Apply or revert your changes first.";
        return;
    }
    const std::string answer = command(app, "connect " + serial);
    if (answer != "ok") {
        app.message = answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer;
        return;
    }
    load_config(app);
    poll_daemon(app);
    app.failed = false;
    app.message = "Added. Enter its values, or turn its ADB proxy on and fill them from adb.";
    const size_t index = find_device(app.config, serial);
    if (index < app.config.devices.size()) {
        app.device = index;
        app.computer = false;
    }
}

// "Fill from adb": the daemon reads the device through its ADB proxy (which
// has to be on) and writes what the phone says into its file. This window
// only asks, waits, and reads the files again once that is done.
void start_fill(App& app, const std::string& serial) {
    app.failed = true;
    if (!app.daemon_running) {
        app.message = "Start the daemon first.";
        return;
    }
    // The daemon rewrites the device's file; nothing unsaved may be lost to
    // that.
    if (app.dirty) {
        app.message = "Apply or revert your changes first.";
        return;
    }
    const std::string answer = command(app, "fill " + serial);
    if (answer != "ok") {
        app.message = answer.rfind("error=", 0) == 0 ? answer.substr(6) : answer;
        return;
    }
    app.failed = false;
    app.filling = serial;
    app.filling_until = ImGui::GetTime() + 60.0;
    // The reading is already under way: the status that says so is read now,
    // not a second from now, or the old one would look like an ended reading.
    poll_daemon(app);
    app.message = "Reading " + serial + " through adb...";
}

void continue_fill(App& app) {
    if (app.filling.empty())
        return;
    const double now = ImGui::GetTime();
    app.next_poll = std::min(app.next_poll, now + 0.4);
    const std::string prefix = "device." + app.filling + ".";
    const auto value = [&](const char* key) {
        const auto found = app.status.find(prefix + key);
        return found != app.status.end() ? found->second : std::string();
    };
    const bool reading = value("reading") == "1";
    if (reading && now < app.filling_until)
        return;
    const std::string serial = app.filling;
    app.filling.clear();
    // What the daemon wrote is the config now, unless the window holds edits
    // made meanwhile: those are not overwritten.
    if (!app.dirty)
        load_config(app);
    const std::string result = value("fill");
    if (reading || result.empty()) {
        app.failed = true;
        app.message = "The daemon gave no result for the reading (is it the current version?).";
        return;
    }
    const bool ok = result.rfind("ok ", 0) == 0;
    app.failed = !ok;
    // The text after "ok " or "error ".
    app.message = (ok ? "" : "Not filled: ") + result.substr(result.find(' ') + 1);
    if (!ok && app.message.find("Not filled: could not") == 0)
        app.message = "Not filled: " + result.substr(result.find(' ') + 1);
}

std::string status_of(const App& app, const DeviceConfig& device, const char* key) {
    const auto found = app.status.find("device." + device.serial + "." + std::string(key));
    return found != app.status.end() ? found->second : std::string();
}

bool has_selection(const App& app) {
    return app.daemon_running && !app.computer && app.device < app.config.devices.size();
}

// The folder this program is in, asked from the system. argv[0] would be
// resolved against the working directory, which is not ours to trust when
// the daemon is started from here.
std::filesystem::path program_directory() {
    std::error_code ignored;
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0)
        return {};
    return std::filesystem::canonical(buffer, ignored).parent_path();
#else
    return std::filesystem::read_symlink("/proc/self/exe", ignored).parent_path();
#endif
}

// Opens a folder in the desktop environment's file manager (Explorer, Finder,
// or the user's default desktop file manager via xdg-open).
void open_in_file_manager(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_directories(path, error);
#if defined(_WIN32)
    static_cast<void>(
        spawn_detached({"explorer.exe", std::filesystem::path(path).make_preferred().string()}));
#elif defined(__APPLE__)
    static_cast<void>(spawn_detached({"open", path.string()}));
#else
    static_cast<void>(spawn_detached({"xdg-open", path.string()}));
#endif
}

} // namespace gui
