// SPDX-License-Identifier: MIT
#include "status_view.hpp"

#include "aoahid_share/keys.hpp"
#include "app.hpp"
#include "look.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gui {
namespace {

using Rows = std::vector<std::pair<std::string, std::string>>;

std::vector<std::string> split(const std::string_view text, const char separator) {
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = std::min(text.find(separator, at), text.size());
        parts.emplace_back(text.substr(at, end - at));
        at = end + 1;
    }
    return parts;
}

// A value that is several numbers is shown with the name of each; a value
// that does not split into as many as the table names is shown as it is.
std::string described(const std::string& value, const aoas::StatusKey* known) {
    if (known == nullptr || known->parts[0] == '\0')
        return value;
    const std::vector<std::string> names = split(known->parts, '|');
    std::vector<std::string> numbers;
    std::istringstream words(value);
    for (std::string word; words >> word;)
        numbers.push_back(word);
    if (numbers.size() != names.size())
        return value;
    std::string text;
    for (size_t index = 0; index < names.size(); ++index)
        text += (index == 0 ? "" : "   ") + names[index] + " " + numbers[index];
    return text;
}

// Three bars: the typical delay, the one that 99 of 100 stay within, and the
// worst, in proportion to the worst.
void draw_delay_bars(const char* label, const std::string& value) {
    std::istringstream words(value);
    double numbers[3] = {};
    for (double& number : numbers) {
        std::string word;
        if (!(words >> word))
            return;
        number = std::atof(word.c_str());
    }
    const double worst = std::max(numbers[2], 1.0);
    ImGui::TextUnformatted(label);
    static const char* const names[3] = {"typical", "99 of 100", "worst"};
    for (int index = 0; index < 3; ++index) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float full = look::px(300);
        const float height = ImGui::GetTextLineHeight();
        ImDrawList* const draw = ImGui::GetWindowDrawList();
        draw->AddRect(origin, ImVec2(origin.x + full, origin.y + height), look::ink);
        draw->AddRectFilled(
            origin,
            ImVec2(origin.x + full * static_cast<float>(numbers[index] / worst), origin.y + height),
            look::ink);
        ImGui::Dummy(ImVec2(full, height));
        ImGui::SameLine();
        ImGui::Text("%s %.0f us", names[index], numbers[index]);
    }
}

} // namespace

void draw_summary(App& app, const aoas::DeviceConfig* device) {
    if (!app.daemon_running) {
        ImGui::TextUnformatted("The daemon is not running.");
        look::dim("Press \"Start the daemon\" at the bottom left. The computer's settings can be "
                  "edited meanwhile; the devices appear once it runs.");
        return;
    }
    const auto value = [&](const char* const key) {
        const auto found = app.status.find(key);
        return found != app.status.end() ? found->second : std::string();
    };
    if (device == nullptr) {
        const std::string state = value("state");
        const std::string active = value("active");
        if (state == "android")
            ImGui::Text("The input is on %s.", active.c_str());
        else if (state == "paused")
            ImGui::TextUnformatted("Crossing is paused.");
        else if (state == "starting")
            ImGui::TextUnformatted("The daemon is starting.");
        else
            ImGui::TextUnformatted("The input is on this computer.");
        return;
    }
    const std::string state = status_of(app, *device, "state");
    const std::string reason = status_of(app, *device, "status");
    ImGui::Text("%s %s", look::mark(state, device->enabled),
                !device->enabled   ? "left alone"
                : state == "ready" ? "ready"
                : state == "error" ? "not ready"
                                   : "not connected");
    if (state == "ready") {
        if (status_of(app, *device, "locked") == "yes") {
            ImGui::SameLine();
            ImGui::TextUnformatted("locked");
        }
        ImGui::SameLine();
        look::dim((status_of(app, *device, "mode") + " tracking, " +
                   status_of(app, *device, "gain") + " px per count")
                      .c_str());
        std::istringstream words(status_of(app, *device, "total_us"));
        double typical = 0.0;
        double p99 = 0.0;
        if (words >> typical >> p99 && p99 > 0.0) {
            char text[120];
            std::snprintf(text, sizeof text, "delay %.2f ms typical, %.2f ms for 99 of 100",
                          typical / 1000.0, p99 / 1000.0);
            look::dim(text);
        }
    } else if (!reason.empty() && device->enabled && reason != "not connected") {
        look::dim(reason.c_str());
    }
}

void draw_status(App& app, const aoas::DeviceConfig* device) {
    look::heading("Status");
    if (!app.daemon_running) {
        look::dim("daemon: not running");
        return;
    }
    const bool per_device = device != nullptr;
    const std::string prefix = per_device ? "device." + device->serial + "." : std::string();

    std::map<std::string, Rows> by_group;
    std::string queue;
    std::string total;
    for (const auto& [name, value] : app.status) {
        const bool mine = per_device ? name.rfind(prefix, 0) == 0 : name.rfind("device.", 0) != 0;
        if (!mine)
            continue;
        const std::string key = name.substr(prefix.size());
        if (key == "queue_us")
            queue = value;
        else if (key == "total_us")
            total = value;
        const aoas::StatusKey* known = aoas::find_status_key(per_device, key);
        by_group[known != nullptr ? known->group : "Other"].emplace_back(key, value);
    }
    if (by_group.empty()) {
        look::dim("the daemon reports nothing for this yet");
        return;
    }
    // What is wrong comes first.
    for (const std::string_view key : {"config_error", "capture_error", "status", "fill"}) {
        const auto found = app.status.find(prefix + std::string(key));
        if (found == app.status.end() || found->second.empty())
            continue;
        if (key == "fill" && found->second.rfind("error", 0) != 0)
            continue;
        if (key == "status" && found->second == "not connected")
            continue;
        ImGui::TextWrapped("ERR: %s", found->second.c_str());
    }
    look::dim("Click a value to copy it.");

    static const char* const order[] = {"Device", "Position", "Link", "Timing",
                                        "Host",   "Files",    "Other"};
    for (const char* const group : order) {
        const auto found = by_group.find(group);
        if (found == by_group.end())
            continue;
        ImGui::TextUnformatted(group);
        if (ImGui::BeginTable(group, 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, look::px(150));
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 3.0F);
            ImGui::TableSetupColumn("unit", ImGuiTableColumnFlags_WidthFixed, look::px(70));
            ImGui::TableSetupColumn("meaning", ImGuiTableColumnFlags_WidthStretch, 4.0F);
            for (const auto& [key, value] : found->second) {
                const aoas::StatusKey* known = aoas::find_status_key(per_device, key);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(key.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", described(value, known).c_str());
                if (ImGui::IsItemClicked())
                    ImGui::SetClipboardText(value.c_str());
                ImGui::TableNextColumn();
                look::dim(known != nullptr ? known->unit : "");
                ImGui::TableNextColumn();
                look::dim(known != nullptr ? known->help : "not known to this window");
            }
            ImGui::EndTable();
        }
        if (std::string_view(group) == "Timing") {
            if (!queue.empty())
                draw_delay_bars("Input to the start of its USB call", queue);
            if (!total.empty())
                draw_delay_bars("Input to the report's completion", total);
        }
    }
}

void draw_reports(App& app, const aoas::DeviceConfig& device) {
    look::heading("Reports");
    if (!app.daemon_running) {
        look::dim("daemon: not running");
        return;
    }
    if (app.reports_serial != device.serial) {
        app.reports.clear();
        app.reports_last = -1;
        app.reports_serial = device.serial;
        app.reports_next = 0.0;
    }
    look::dim("What was handed to the device, one line per call. Lines of one report share the "
              "report number. Recording runs only while this tab is open.");
    ImGui::Checkbox("Freeze", &app.reports_frozen);
    ImGui::SameLine();
    if (ImGui::Button("Clear"))
        app.reports.clear();
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
        ImGui::SetClipboardText(
            std::accumulate(
                app.reports.begin(), app.reports.end(), std::string(),
                [](const std::string& all, const std::string& line) { return all + line + "\n"; })
                .c_str());

    if (!app.reports_frozen && ImGui::GetTime() >= app.reports_next) {
        app.reports_next = ImGui::GetTime() + poll_seconds;
        const std::string answer = command(app, "reports " + device.serial);
        if (answer.rfind("ok", 0) != 0) {
            look::dim(answer.c_str());
        } else {
            std::istringstream lines(answer);
            std::string line;
            std::getline(lines, line);
            std::vector<std::string> fresh;
            while (std::getline(lines, line))
                fresh.push_back(line);
            // Numbers that fell back mean the device was opened again.
            if (!fresh.empty() && std::atoll(fresh.back().c_str()) < app.reports_last) {
                app.reports.clear();
                app.reports_last = -1;
            }
            for (const std::string& each : fresh) {
                const long long number = std::atoll(each.c_str());
                if (number > app.reports_last) {
                    app.reports_last = number;
                    app.reports.push_back(each);
                }
            }
            while (app.reports.size() > 1000)
                app.reports.pop_front();
        }
    }

    ImGui::BeginChild("reports", ImVec2(0, 0), true);
    for (const std::string& line : app.reports)
        ImGui::TextUnformatted(line.c_str());
    if (!app.reports_frozen && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F)
        ImGui::SetScrollHereY(1.0F);
    ImGui::EndChild();
}

} // namespace gui
