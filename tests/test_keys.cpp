// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "aoahid_share/config.hpp"
#include "aoahid_share/ini_edit.hpp"
#include "aoahid_share/keys.hpp"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>

using namespace aoas;

namespace {

const ConfigKey& find_key(const KeyScope scope, const std::string_view section,
                          const std::string_view key) {
    for (const ConfigKey& each : config_keys()) {
        if ((each.scope == scope || each.scope == KeyScope::both) && section == each.section &&
            key == each.key)
            return each;
    }
    FAIL("no such key: " << section << "." << key);
    return config_keys()[0];
}

} // namespace

TEST_CASE("IniText reads and writes one key at a time") {
    IniText ini("[daemon]\nbackend = x11\n\n[motion]\nsensitivity = 1.5\n");
    CHECK(ini.get("daemon", "backend") == "x11");
    CHECK(ini.get("motion", "sensitivity") == "1.5");
    CHECK_FALSE(ini.get("motion", "entry").has_value());
    CHECK_FALSE(ini.get("nowhere", "x").has_value());

    ini.set("motion", "entry", "aligned");
    CHECK(ini.get("motion", "entry") == "aligned");
    ini.set("motion", "sensitivity", "2");
    CHECK(ini.get("motion", "sensitivity") == "2");
    ini.set("adb", "path", "/usr/bin/adb");
    CHECK(ini.get("adb", "path") == "/usr/bin/adb");

    ini.erase("motion", "entry");
    CHECK_FALSE(ini.get("motion", "entry").has_value());
    ini.erase("motion", "missing");
}

TEST_CASE("IniText keeps an empty value as present and empty") {
    IniText ini("[daemon]\npause_hotkey = \n");
    CHECK(ini.get("daemon", "pause_hotkey") == "");
    CHECK(IniText(ini.str()).get("daemon", "pause_hotkey") == "");
}

TEST_CASE("IniText round-trips its own output") {
    IniText ini("[a]\nx = 1\n[b]\ny = 2\n");
    CHECK(IniText(ini.str()).str() == ini.str());
}

TEST_CASE("a computer-wide edit keeps the devices") {
    Config config;
    config.devices.emplace_back();
    config.devices.back().serial = "ABC1";
    CHECK(set_key_text(config, nullptr, find_key(KeyScope::computer, "daemon", "backend"), "x11")
              .empty());
    CHECK(config.backend == "x11");
    REQUIRE(config.devices.size() == 1);
    CHECK(config.devices[0].serial == "ABC1");
}

TEST_CASE("a bad value is refused with the parser's reason and changes nothing") {
    Config config;
    const std::string problem =
        set_key_text(config, nullptr, find_key(KeyScope::computer, "mouse", "buttons"), "99");
    CHECK_FALSE(problem.empty());
    CHECK(config.mouse_buttons == 5);
}

TEST_CASE("a device's motion override can be set and cleared") {
    Config config;
    DeviceConfig device;
    device.serial = "ABC1";
    const ConfigKey& key = find_key(KeyScope::device, "motion", "sensitivity");
    CHECK_FALSE(key_text(config, &device, key).has_value());
    CHECK(set_key_text(config, &device, key, "2.5").empty());
    CHECK(key_text(config, &device, key) == "2.5");
    CHECK(device.motion.sensitivity == doctest::Approx(2.5));
    CHECK(clear_key(config, &device, key).empty());
    CHECK_FALSE(key_text(config, &device, key).has_value());
    CHECK_FALSE(device.motion.sensitivity.has_value());
}

TEST_CASE("an empty text unsets a key that can be unset and is refused for one that cannot") {
    Config config;
    config.pause_hotkey = "ctrl+alt+p";
    CHECK(set_key_text(config, nullptr, find_key(KeyScope::computer, "daemon", "pause_hotkey"), "")
              .empty());
    CHECK(config.pause_hotkey.empty());
    CHECK_FALSE(set_key_text(config, nullptr, find_key(KeyScope::computer, "mouse", "buttons"), "")
                    .empty());
    CHECK(config.mouse_buttons == 5);
}

namespace {

std::set<std::string> table_names(const KeyScope scope) {
    std::set<std::string> names;
    for (const ConfigKey& key : config_keys()) {
        if (key.scope == scope || key.scope == KeyScope::both)
            names.insert(std::string(key.section) + "." + key.key);
    }
    return names;
}

// Every "section.key" in an ini text.
std::set<std::string> written(const std::string& text) {
    std::set<std::string> names;
    std::string section;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const std::string line = text.substr(at, end == std::string::npos ? end : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.front() == '[')
            section = line.substr(1, line.size() - 2);
        else if (const size_t equals = line.find(" = "); equals != std::string::npos)
            names.insert(section + "." + line.substr(0, equals));
    }
    return names;
}

// A device with every optional value given, so that format_device writes every line.
DeviceConfig full_device() {
    DeviceConfig d;
    d.serial = "ABC1";
    d.name = "Tab";
    d.segment_start = 10;
    d.segment_length = 500;
    d.monitor_diagonal_inch = 24.0;
    d.rotation = 0;
    d.width = 1000;
    d.height = 2000;
    d.diagonal_inch = 11.0;
    d.density_dpi = 320;
    d.gain = 1.0;
    d.accel = AccelSetting::on;
    d.motion = motion_patch_of(Motion{});
    d.keyboard = true;
    d.adb_proxy = true;
    d.adb_port = 6555;
    return d;
}

// A value each kind of key accepts; the keys that need another are in `special`.
std::string sample_for(const ConfigKey& key) {
    switch (key.kind) {
    case KeyKind::boolean:
        return std::string(key.initial) == "true" ? "false" : "true";
    case KeyKind::integer:
        return "3";
    case KeyKind::real:
        return "1.25";
    case KeyKind::choice: {
        const std::string_view all(key.choices);
        return std::string(all.substr(0, all.find('|')));
    }
    case KeyKind::hotkey:
        return "ctrl+alt+h";
    case KeyKind::list:
        return "1,2,3";
    case KeyKind::text:
        return "x";
    }
    return {};
}

} // namespace

TEST_CASE("every key the library writes is in the table") {
    Config config;
    config.media_hotkeys[0] = "ctrl+alt+left";
    config.button_map = {1, 2};
    for (const std::string& name : written(format_globals(config)))
        CHECK_MESSAGE(table_names(KeyScope::computer).count(name) == 1, name);
    for (const std::string& name : written(format_device(full_device())))
        CHECK_MESSAGE(table_names(KeyScope::device).count(name) == 1, name);
}

TEST_CASE("every key in the table is accepted by the parser and read back") {
    const std::map<std::string, std::string> special = {
        {"mouse.buttons", "3"},
        {"adb.first_port", "7000"},
        {"adb.port", "7001"},
        {"device.serial", "XYZ9"},
        {"placement.rotation", "90"},
        {"placement.side", "left"},
        {"detected.accel", "off"},
        {"detected.density", "320"},
        {"motion.report_rate_hz", "500"},
        {"detected.read_at", "2026-10-06"},
        {"placement.turn_input", "90"},
        {"placement.corner_margin", "10%"},
        {"adb.path", "/usr/bin/adb"},
    };
    for (const ConfigKey& key : config_keys()) {
        const std::string id = std::string(key.section) + "." + key.key;
        const std::string value = special.count(id) != 0 ? special.at(id) : sample_for(key);
        Config config;
        DeviceConfig device = full_device();
        DeviceConfig* target = key.scope == KeyScope::computer ? nullptr : &device;
        const std::string problem = set_key_text(config, target, key, value);
        CHECK_MESSAGE(problem.empty(), id << " = " << value << ": " << problem);
        CHECK_MESSAGE(key_text(config, target, key).has_value(), id);
    }
}

TEST_CASE("a key is in the table once for each place it lives") {
    std::set<std::string> seen;
    for (const ConfigKey& key : config_keys()) {
        const std::string id = std::string(key.section) + "." + key.key;
        const bool computer = key.scope != KeyScope::device;
        const bool device = key.scope != KeyScope::computer;
        if (computer)
            CHECK_MESSAGE(seen.insert("c:" + id).second, id);
        if (device)
            CHECK_MESSAGE(seen.insert("d:" + id).second, id);
    }
}

TEST_CASE("the status table names every key once and finds numbered ones") {
    std::set<std::string> seen;
    for (const StatusKey& key : status_keys())
        CHECK_MESSAGE(seen.insert(std::string(key.per_device ? "d:" : "t:") + key.key).second,
                      key.key);
    CHECK(find_status_key(false, "monitor.3") != nullptr);
    CHECK(find_status_key(false, "new.R5GL153Y5EX") != nullptr);
    CHECK(find_status_key(true, "queue_us") != nullptr);
    CHECK(find_status_key(true, "nonsense") == nullptr);
    CHECK(find_status_key(false, "queue_us") == nullptr);
    const StatusKey* total = find_status_key(true, "total_us");
    REQUIRE(total != nullptr);
    CHECK(std::string(total->parts) == "typical|99 of 100|worst");
}

namespace {

// The backticked words of a text.
void collect_words(const std::string& text, std::set<std::string>& out) {
    for (size_t at = text.find('`'); at != std::string::npos;) {
        const size_t close = text.find('`', at + 1);
        if (close == std::string::npos)
            break;
        out.insert(text.substr(at + 1, close - at - 1));
        at = text.find('`', close + 1);
    }
}

} // namespace

TEST_CASE("docs/CONFIG.md and the tables name the same keys") {
    std::ifstream file(std::string(AOAHID_SHARE_DOCS_DIR) + "/CONFIG.md");
    REQUIRE(file.good());
    // What the docs name in backticks before "## Commands": in the first cell
    // of a table row (a key's own row), and anywhere at all.
    std::set<std::string> rows;
    std::set<std::string> mentioned;
    std::string line;
    while (std::getline(file, line) && line != "## Commands") {
        collect_words(line, mentioned);
        if (line.rfind("| `", 0) == 0) {
            const size_t cell_end = line.find(" |", 3);
            collect_words(
                line.substr(2, cell_end == std::string::npos ? std::string::npos : cell_end - 2),
                rows);
        }
    }
    std::set<std::string> known;
    for (const ConfigKey& key : config_keys())
        known.insert(key.key);
    for (const StatusKey& key : status_keys())
        known.insert(key.key);
    // The docs write a numbered or serial-named key with a placeholder.
    known.insert("monitor.N");
    known.insert("new.<serial>");
    known.erase("monitor.#");
    known.erase("new.#");
    // A row that names a key together with its section.
    const std::set<std::string> with_section = {"[keyboard] enabled"};
    for (const std::string& name : rows) {
        const bool understood = known.count(name) == 1 || with_section.count(name) == 1;
        CHECK_MESSAGE(understood, name << " has a row in the docs but is not in a table");
    }
    for (const std::string& name : known)
        CHECK_MESSAGE(mentioned.count(name) == 1, name << " is in a table but not in the docs");
}

TEST_CASE("a key is found by its place, and the same name in two sections is told apart") {
    const ConfigKey* proxy = find_config_key(KeyScope::device, "adb", "proxy");
    REQUIRE(proxy != nullptr);
    CHECK(std::string(proxy->key) == "proxy");
    // [adb] first_port is the computer's, [adb] port is a device's.
    CHECK(find_config_key(KeyScope::computer, "adb", "first_port") != nullptr);
    CHECK(find_config_key(KeyScope::device, "adb", "first_port") == nullptr);
    // A key that both may set is found from either side.
    CHECK(find_config_key(KeyScope::computer, "keyboard", "enabled") != nullptr);
    CHECK(find_config_key(KeyScope::device, "keyboard", "enabled") != nullptr);
    CHECK(find_config_key(KeyScope::device, "placement", "nonsense") == nullptr);
}

TEST_CASE("only the keys that make no sense to type are read-only") {
    for (const ConfigKey& key : config_keys()) {
        const std::string id = std::string(key.section) + "." + key.key;
        // The serial names the file; read_at is the day the values were read from adb.
        const bool expected = id == "device.serial" || id == "detected.read_at";
        CHECK_MESSAGE(key.read_only == expected, id);
    }
}

TEST_CASE(
    "an old device file that has accessory_fallback still reads, and it is not written back") {
    DeviceConfig device;
    CHECK(parse_device("[device]\nserial = ABC1\naccessory_fallback = true\n", device).empty());
    CHECK(device.serial == "ABC1");
    CHECK(format_device(device).find("accessory") == std::string::npos);
    Config config;
    CHECK(parse_globals("[daemon]\nbackend = auto\n", config).empty());
}

TEST_CASE("a device may pace its reports on its own") {
    Config config;
    config.report_rate_hz = 500;
    DeviceConfig device;
    CHECK(report_rate_of(config, device).hz == 500);
    CHECK_FALSE(report_rate_of(config, device).every);

    REQUIRE(parse_device("[device]\nserial = ABC1\n[motion]\nreport_rate_hz = every\n", device)
                .empty());
    CHECK(report_rate_of(config, device).every);
    CHECK(format_device(device).find("report_rate_hz = every") != std::string::npos);
    DeviceConfig again;
    REQUIRE(parse_device(format_device(device), again).empty());
    CHECK(report_rate_of(config, again).every);

    REQUIRE(parse_device("[motion]\nreport_rate_hz = 250\n", device).empty());
    CHECK(report_rate_of(config, device).hz == 250);
    CHECK_FALSE(report_rate_of(config, device).every);
    CHECK_FALSE(parse_device("[motion]\nreport_rate_hz = 9\n", device).empty());

    // The line is the device's own value; without it the computer's applies.
    const ConfigKey* key = find_config_key(KeyScope::device, "motion", "report_rate_hz");
    REQUIRE(key != nullptr);
    CHECK(clear_key(config, &device, *key).empty());
    CHECK(report_rate_of(config, device).hz == 500);
}
