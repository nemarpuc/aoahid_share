// SPDX-License-Identifier: MIT
#include "field.hpp"

#include "app.hpp"
#include "look.hpp"

#include <misc/cpp/imgui_stdlib.h>

#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace gui {
namespace {

// The parsers say "line N: what is wrong"; the line is one made up here, so
// only what is wrong is kept.
std::string without_line(const std::string& problem) {
    if (problem.rfind("line ", 0) == 0) {
        const size_t colon = problem.find(": ");
        if (colon != std::string::npos)
            return problem.substr(colon + 2);
    }
    return problem;
}

// Takes the text through the parsers. On an objection the row keeps the
// reason and nothing changes.
bool apply_text(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
                const std::string& text, const std::string& id) {
    const std::string problem = aoas::set_key_text(app.config, device, key, text);
    if (!problem.empty()) {
        app.field_errors[id] = without_line(problem);
        return false;
    }
    app.field_errors.erase(id);
    app.editing.erase(id);
    app.dirty = true;
    return true;
}

// Removes the line: the device follows the computer again, or the key its default.
bool clear_row(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
               const std::string& id) {
    const std::string problem = aoas::clear_key(app.config, device, key);
    app.editing.erase(id);
    if (!problem.empty()) {
        app.field_errors[id] = without_line(problem);
        return false;
    }
    app.field_errors.erase(id);
    app.dirty = true;
    return true;
}

// `follow` is what a device's own choice falls back to: the computer's value,
// or null when the row is not a device's override of the computer's.
bool choice_combo(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
                  const std::string& id, const std::optional<std::string>& now,
                  const std::string* follow) {
    bool changed = false;
    const std::string current = now.value_or(follow != nullptr ? std::string() : key.initial);
    const bool can_be_unset = key.initial[0] == '\0' && key.scope != aoas::KeyScope::both;
    const std::string follow_text =
        follow != nullptr ? "Same as the computer (" + *follow + ")" : std::string();
    const char* const preview = follow != nullptr && !now.has_value() ? follow_text.c_str()
                                : current.empty()                     ? "(not set)"
                                                                      : current.c_str();
    if (ImGui::BeginCombo("##choice", preview)) {
        if (follow != nullptr && ImGui::Selectable(follow_text.c_str(), !now.has_value()))
            changed = clear_row(app, key, device, id);
        if (can_be_unset && ImGui::Selectable("(not set)", current.empty()))
            changed = clear_row(app, key, device, id);
        std::string_view rest(key.choices);
        while (!rest.empty()) {
            const size_t bar = rest.find('|');
            const std::string option(rest.substr(0, bar));
            rest = bar == std::string_view::npos ? std::string_view() : rest.substr(bar + 1);
            if (ImGui::Selectable(option.c_str(), option == current))
                changed = apply_text(app, key, device, option, id);
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

namespace {

// A key's name for a person: the polling rate by its usual name, else the key
// with spaces for underscores.
std::string readable(const aoas::ConfigKey& key) {
    if (std::strcmp(key.key, "report_rate_hz") == 0)
        return "Polling rate";
    std::string name = key.key;
    for (char& c : name) {
        if (c == '_')
            c = ' ';
    }
    if (!name.empty())
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    return name;
}

// What a row is known by: its device (or the computer) and its place in the files.
std::string row_id(const aoas::ConfigKey& key, const aoas::DeviceConfig* device) {
    return (device != nullptr ? device->serial : std::string("*")) + "/" + key.section + "." +
           key.key;
}

} // namespace

bool set_field(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
               const std::string& text) {
    return apply_text(app, key, device, text, row_id(key, device));
}

bool clear_field(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device) {
    return clear_row(app, key, device, row_id(key, device));
}

bool field(App& app, const aoas::ConfigKey& key, aoas::DeviceConfig* device,
           const char* const label) {
    const std::string id = row_id(key, device);
    const std::optional<std::string> now = aoas::key_text(app.config, device, key);
    // A device's own value of a key the computer also has.
    const bool replaceable = key.scope == aoas::KeyScope::both && device != nullptr;
    const std::string computer_text =
        replaceable ? aoas::key_text(app.config, nullptr, key).value_or(key.initial)
                    : std::string();
    std::string name = label != nullptr ? label : readable(key);
    if (key.unit[0] != '\0')
        name += std::string(" (") + key.unit + ")";
    std::string tip = key.help;
    if (key.choices[0] != '\0' && key.kind != aoas::KeyKind::choice)
        tip += std::string("\nRange: ") + key.choices;
    if (key.initial[0] != '\0')
        tip += std::string("\nDefault: ") + key.initial;
    const float wide = look::px(260);
    bool changed = false;
    ImGui::PushID(id.c_str());

    if (key.read_only) {
        look::row(name.c_str(), tip.c_str(), wide);
        ImGui::TextUnformatted(now.value_or("").empty() ? "-" : now->c_str());
    } else if (key.kind == aoas::KeyKind::boolean && !replaceable) {
        look::row(name.c_str(), tip.c_str(), look::px(40));
        bool on = now.value_or(key.initial) == "true";
        if (look::toggle("value", &on))
            changed = apply_text(app, key, device, on ? "true" : "false", id);
    } else if (key.kind == aoas::KeyKind::boolean) {
        // A device's own value of a switch: on, off, or whatever the computer says.
        look::row(name.c_str(), tip.c_str(), wide);
        const std::string follow =
            std::string("Same as the computer (") + (computer_text == "true" ? "on" : "off") + ")";
        const char* const current = !now.has_value() ? follow.c_str()
                                    : *now == "true" ? "On"
                                                     : "Off";
        ImGui::SetNextItemWidth(wide);
        if (ImGui::BeginCombo("##value", current)) {
            if (ImGui::Selectable(follow.c_str(), !now.has_value()))
                changed = clear_row(app, key, device, id);
            if (ImGui::Selectable("On", now == "true"))
                changed = apply_text(app, key, device, "true", id);
            if (ImGui::Selectable("Off", now == "false"))
                changed = apply_text(app, key, device, "false", id);
            ImGui::EndCombo();
        }
    } else if (key.kind == aoas::KeyKind::choice) {
        look::row(name.c_str(), tip.c_str(), wide);
        ImGui::SetNextItemWidth(wide);
        changed = choice_combo(app, key, device, id, now, replaceable ? &computer_text : nullptr);
    } else {
        look::row(name.c_str(), tip.c_str(), wide);
        std::string& text = app.editing.try_emplace(id, now.value_or("")).first->second;
        // What is on screen follows the config, except while it is being
        // typed or holds a value that was refused.
        if (!ImGui::IsAnyItemActive() && app.field_errors.count(id) == 0 &&
            text != now.value_or(""))
            text = now.value_or("");
        const bool polling = std::strcmp(key.key, "report_rate_hz") == 0;
        const std::string hint =
            replaceable ? std::string(polling ? "computer: " : "same as the computer: ") +
                              (computer_text.empty() ? "not set" : computer_text)
                        : (key.initial[0] != '\0' ? std::string(key.initial) : "not set");
        // Room beside the input for what goes with it.
        ImGui::SetNextItemWidth(wide - (replaceable ? look::px(64) : 0.0F) -
                                (polling ? look::px(96) : 0.0F));
        ImGui::InputTextWithHint("##value", hint.c_str(), &text);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            changed = replaceable && text.empty() ? clear_row(app, key, device, id)
                                                  : apply_text(app, key, device, text, id);
        }
        if (replaceable && now.has_value()) {
            ImGui::SameLine();
            if (ImGui::Button("Reset"))
                changed = clear_row(app, key, device, id);
        }
        if (polling) {
            // The usual polling rates, for picking instead of typing.
            ImGui::SameLine();
            ImGui::SetNextItemWidth(look::px(88));
            if (ImGui::BeginCombo("##presets", "presets")) {
                for (const char* const preset :
                     {"max", "every", "125", "250", "500", "1000", "2000", "4000", "8000"}) {
                    if (ImGui::Selectable(preset))
                        changed = apply_text(app, key, device, preset, id);
                }
                ImGui::EndCombo();
            }
        }
    }
    look::row_end();
    const auto error = app.field_errors.find(id);
    if (error != app.field_errors.end())
        ImGui::TextUnformatted(("ERR: " + error->second).c_str());
    ImGui::PopID();
    return changed;
}

} // namespace gui
