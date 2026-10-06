// SPDX-License-Identifier: MIT
#include "placement.hpp"

#include "aoahid_share/keys.hpp"
#include "app.hpp"
#include "field.hpp"
#include "look.hpp"

#include <string>
#include <vector>

using namespace aoas;

namespace gui {
namespace {

const ConfigKey& key_of(const char* const name) {
    // These keys are in the table; a missing one is a mistake in this file.
    return *find_config_key(KeyScope::device, "placement", name);
}

// What a device sits beside: a monitor, or another device.
struct Place {
    std::string label;
    bool is_device{};
    std::string value;
};

std::vector<Place> places_for(const App& app, const DeviceConfig& device) {
    std::vector<Place> places;
    for (const Monitor& monitor : app.monitors)
        places.push_back({"monitor " + monitor.name, false, monitor.name});
    for (const DeviceConfig& other : app.config.devices) {
        if (!other.serial.empty() && other.serial != device.serial)
            places.push_back({"device " + label_of(other), true, other.serial});
    }
    return places;
}

std::string current_place(const App& app, const DeviceConfig& device) {
    if (!device.beside.empty()) {
        const size_t index = find_device(app.config, device.beside);
        return "device " + (index < app.config.devices.size() ? label_of(app.config.devices[index])
                                                              : device.beside);
    }
    if (!device.monitor.empty())
        return "monitor " + device.monitor;
    return app.monitors.empty() ? "the first monitor" : "monitor " + app.monitors.front().name;
}

void draw_side(App& app, DeviceConfig& device) {
    static const char* const names[] = {"left", "right", "top", "bottom"};
    const std::string now = key_text(app.config, &device, key_of("side")).value_or("right");
    look::row("Side", "Which side of the monitor or of the other device it sits on.",
              look::px(260));
    ImGui::SetNextItemWidth(look::px(260));
    if (ImGui::BeginCombo("##side", now.c_str())) {
        for (const char* const name : names) {
            if (ImGui::Selectable(name, now == name))
                set_field(app, key_of("side"), &device, name);
        }
        ImGui::EndCombo();
    }
    look::row_end();
}

void draw_beside(App& app, DeviceConfig& device) {
    look::row("Next to", "The monitor, or the other device, whose edge it shares.", look::px(260));
    ImGui::SetNextItemWidth(look::px(260));
    if (ImGui::BeginCombo("##place", current_place(app, device).c_str())) {
        for (const Place& place : places_for(app, device)) {
            if (!ImGui::Selectable(place.label.c_str()))
                continue;
            if (place.is_device) {
                set_field(app, key_of("beside"), &device, place.value);
            } else if (set_field(app, key_of("monitor"), &device, place.value)) {
                // A monitor's edge, not another device's.
                set_field(app, key_of("beside"), &device, "");
            }
        }
        ImGui::EndCombo();
    }
    look::row_end();
    const auto error = app.field_errors.find(device.serial + "/placement.beside");
    if (error != app.field_errors.end())
        ImGui::TextUnformatted(("ERR: " + error->second).c_str());
}

// What the picture and the rows above already say.
bool shown_above(const ConfigKey& key) {
    for (const char* const name : {"side", "monitor", "beside", "start", "length"}) {
        if (std::string(key.key) == name)
            return true;
    }
    return false;
}

} // namespace

void draw_placement(App& app, DeviceConfig& device) {
    draw_layout(app);
    ImGui::Spacing();
    draw_side(app, device);
    draw_beside(app, device);
    field(app, key_of("start"), &device);
    field(app, key_of("length"), &device);
    // The rest of the placement, in the order of the table.
    for (const ConfigKey& key : config_keys()) {
        if (std::string(key.group) == "Placement" && key.scope == KeyScope::device &&
            !shown_above(key))
            field(app, key, &device);
    }
    if (ImGui::Button("Centre it on the edge")) {
        clear_field(app, key_of("start"), &device);
        clear_field(app, key_of("length"), &device);
    }
    ImGui::SameLine();
    look::dim("Start and length empty: the middle of the edge, as long as the real sizes say.");
}

} // namespace gui
