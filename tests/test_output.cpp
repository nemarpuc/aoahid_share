// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "adb.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace aoas;

namespace {

// A stand-in for adb: a shell script that answers the few commands the
// program sends, from the text given for each.
std::string write_fake_adb(const std::filesystem::path& folder, const std::string& body) {
    std::filesystem::create_directories(folder);
    const std::filesystem::path script = folder / "adb";
    std::ofstream(script) << "#!/bin/sh\n" << body;
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    return script.string();
}

// The settings folder is worked out once per run; whichever test asks first
// fixes it, so each one names the temporary folder itself and checks it got
// that, before anything is removed.
std::filesystem::path test_settings_folder() {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "aoahid_share_test_settings";
    setenv("AOAHID_SHARE_SETTINGS_DIR", folder.c_str(), 1);
    REQUIRE(settings_dir() == folder.string());
    return folder;
}

} // namespace

// The first test to ask for the settings folder: the answer is kept for the
// whole run.
TEST_CASE("the settings are in one folder, the devices in device/ under it") {
    const std::filesystem::path folder = test_settings_folder();
    std::filesystem::remove_all(folder);
    CHECK(settings_dir() == folder.string());
    CHECK(config_file_path() == (folder / "config.ini").string());
    CHECK(state_file_path() == (folder / "state.ini").string());
    CHECK(devices_dir() == (folder / "device").string());
    CHECK(device_file_path("R5GL153Y5EX") == (folder / "device" / "R5GL153Y5EX.ini").string());

    // A first load writes config.ini and leaves device/ for the devices.
    Config config;
    CHECK(load_store(config).empty());
    CHECK(config.adb_kill_server);
    CHECK(std::filesystem::exists(folder / "config.ini"));
    DeviceConfig device;
    device.serial = "R5GL153Y5EX";
    device.width = 1600;
    device.height = 2560;
    config.devices.push_back(device);
    CHECK(save_store(config));
    CHECK(std::filesystem::exists(folder / "device" / "R5GL153Y5EX.ini"));
    Config again;
    CHECK(load_store(again).empty());
    REQUIRE(again.devices.size() == 1);
    CHECK(again.devices[0].width == 1600);
    // A device's file the config no longer holds is removed with it.
    config.devices.clear();
    CHECK(save_store(config));
    CHECK_FALSE(std::filesystem::exists(folder / "device" / "R5GL153Y5EX.ini"));
    std::filesystem::remove_all(folder);
}

TEST_CASE("only mCurrentOrientation is the rotation") {
    CHECK(parse_rotation("mDefaultDisplayOrientation=1\nmCurrentRotation=1\n") == -1);
    CHECK(parse_rotation("mDefaultDisplayOrientation=1\n    mCurrentOrientation=2\n") == 180);
}

TEST_CASE("wm size may have spaces around the x") {
    int width = 0;
    int height = 0;
    REQUIRE(parse_wm_size("Physical size: 1600 x 2560\n", width, height));
    CHECK(width == 1600);
    CHECK(height == 2560);
}

TEST_CASE("connect waits for a phone that is offline at first") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "aoahid_share_test_adb_offline";
    std::filesystem::remove_all(folder);
    const Adb adb(write_fake_adb(folder,
                                 "case \"$1\" in\n"
                                 "connect) echo \"connected to $2\" ;;\n"
                                 "-s) n=$(cat \"$0.n\" 2>/dev/null || echo 0); n=$((n+1));\n"
                                 "  echo $n > \"$0.n\";\n"
                                 "  if [ $n -lt 3 ]; then echo offline; else echo device; fi ;;\n"
                                 "esac\n"));
    std::string note;
    CHECK(adb.connect(6555, note));
    std::filesystem::remove_all(folder);
}

TEST_CASE("connect tells whether it made the connection, and ends one that never worked") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "aoahid_share_test_adb_created";
    std::filesystem::remove_all(folder);
    // The fake records every command it is run with.
    const Adb fresh(write_fake_adb(folder, "echo \"$*\" >> \"$0.log\"\n"
                                           "case \"$1\" in\n"
                                           "connect) echo \"connected to $2\" ;;\n"
                                           "-s) echo device ;;\n"
                                           "esac\n"));
    std::string note;
    bool created = false;
    CHECK(fresh.connect(6555, note, nullptr, &created));
    CHECK(created);

    const Adb theirs(write_fake_adb(folder, "case \"$1\" in\n"
                                            "connect) echo \"already connected to $2\" ;;\n"
                                            "-s) echo device ;;\n"
                                            "esac\n"));
    CHECK(theirs.connect(6555, note, nullptr, &created));
    CHECK_FALSE(created);

    // A connection that never became usable is disconnected, not left half open.
    const Adb stuck(write_fake_adb(folder, "echo \"$*\" >> \"$0.log\"\n"
                                           "case \"$1\" in\n"
                                           "connect) echo \"connected to $2\" ;;\n"
                                           "-s) echo offline ;;\n"
                                           "esac\n"));
    const std::atomic<bool> cancel{true};
    CHECK_FALSE(stuck.connect(6555, note, &cancel, &created));
    std::ifstream log((folder / "adb.log").string());
    std::string text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
    CHECK(text.find("disconnect 127.0.0.1:6555") != std::string::npos);
    std::filesystem::remove_all(folder);
}

TEST_CASE("connect says why it failed") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "aoahid_share_test_adb_refused";
    std::filesystem::remove_all(folder);
    const Adb refused(write_fake_adb(
        folder, "echo \"failed to connect to '127.0.0.1:6555': Connection refused\"\n"));
    std::string note;
    CHECK_FALSE(refused.connect(6555, note));
    CHECK(note.find("Connection refused") != std::string::npos);

    // An unanswered permission dialog: a cancelled wait ends at once.
    const Adb unauthorized(write_fake_adb(folder, "case \"$1\" in\n"
                                                  "connect) echo \"connected to $2\" ;;\n"
                                                  "-s) echo \"error: device unauthorized.\" ;;\n"
                                                  "esac\n"));
    const std::atomic<bool> cancel{true};
    CHECK_FALSE(unauthorized.connect(6555, note, &cancel));
    CHECK(note.find("allow USB debugging") != std::string::npos);

    const Adb missing((folder / "no_such_adb").string());
    CHECK_FALSE(missing.connect(6555, note));
    CHECK(note == "adb could not be run");
    std::filesystem::remove_all(folder);
}

TEST_CASE("inspect reads what the phone says and invents nothing") {
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "aoahid_share_test_adb_inspect";
    std::filesystem::remove_all(folder);
    // No mCurrentOrientation in this phone's dumpsys: the rotation stays
    // unknown (-1), and the logical density is not taken as the physical one.
    const Adb adb(write_fake_adb(folder,
                                 "case \"$*\" in\n"
                                 "*\"wm size\") echo \"Physical size: 1600x2560\" ;;\n"
                                 "*\"wm density\") echo \"Physical density: 340\" ;;\n"
                                 "*ro.product.model) echo SM-X730 ;;\n"
                                 "*ro.build.version.sdk) echo 36 ;;\n"
                                 "*\"dumpsys display\") echo \"mDefaultDisplayOrientation=1\" ;;\n"
                                 "*pointer_speed) echo 0 ;;\n"
                                 "*mouse_pointer_acceleration_enabled) echo 1 ;;\n"
                                 "*enhance_pointer_precision) echo null ;;\n"
                                 "esac\n"));
    const PhoneFacts facts = adb.inspect(6555);
    CHECK(facts.reachable);
    CHECK(facts.model == "SM-X730");
    CHECK(facts.width == 1600);
    CHECK(facts.height == 2560);
    CHECK(facts.physical_width == 1600);
    CHECK(facts.physical_height == 2560);
    CHECK(facts.density == 340);
    CHECK(facts.rotation == -1);
    CHECK(facts.has_pointer_speed);
    CHECK(facts.accel_key == "mouse_pointer_acceleration_enabled");
    CHECK(facts.accel_before == "1");
    CHECK_FALSE(facts.accel_off);
    std::filesystem::remove_all(folder);
}

// The sample lines are what a Galaxy Tab S11 (Android 16) printed.
TEST_CASE("wm size prefers the override") {
    int width = 0;
    int height = 0;
    REQUIRE(parse_wm_size("Physical size: 1600x2560\n", width, height));
    CHECK(width == 1600);
    CHECK(height == 2560);
    REQUIRE(parse_wm_size("Physical size: 1600x2560\nOverride size: 1200x1920\n", width, height));
    CHECK(width == 1200);
    CHECK(height == 1920);
    // The panel's own pixels, which the physical dpi belongs to.
    REQUIRE(parse_wm_physical_size("Physical size: 1600x2560\nOverride size: 1200x1920\n", width,
                                   height));
    CHECK(width == 1600);
    CHECK(height == 2560);
    CHECK_FALSE(parse_wm_physical_size("Override size: 1200x1920\n", width, height));
    CHECK_FALSE(parse_wm_size("error: no display\n", width, height));
    CHECK_FALSE(parse_wm_size("Physical size: 1600\n", width, height));
}

TEST_CASE("wm density prefers the override") {
    int density = 0;
    REQUIRE(parse_wm_density("Physical density: 340\n", density));
    CHECK(density == 340);
    REQUIRE(parse_wm_density("Physical density: 340\nOverride density: 300\n", density));
    CHECK(density == 300);
    CHECK_FALSE(parse_wm_density("nothing here\n", density));
}

TEST_CASE("the physical dpi is read out of dumpsys display") {
    double x = 0.0;
    double y = 0.0;
    REQUIRE(parse_display_dpi("supportedColorModes [0, 7, 9], density 340, 274.5946 x 275.52545 "
                              "dpi, appVsyncOff 3099999",
                              x, y));
    CHECK(x == doctest::Approx(274.5946));
    CHECK(y == doctest::Approx(275.52545));
    // The parenthesised form alone has no pair in front of " dpi".
    CHECK_FALSE(parse_display_dpi("density 340 (274.5946 x 275.52545) dpi", x, y));
    CHECK_FALSE(parse_display_dpi("no such thing", x, y));
}

TEST_CASE("the rotation comes from mCurrentOrientation") {
    CHECK(parse_rotation("    mCurrentOrientation=1\n") == 90);
    CHECK(parse_rotation("mCurrentOrientation=0") == 0);
    CHECK(parse_rotation("mCurrentOrientation=3") == 270);
    CHECK(parse_rotation("mCurrentOrientation=7") == -1);
    CHECK(parse_rotation("mCurrentOrientation=") == -1);
    CHECK(parse_rotation("rotation 1") == -1);
}

TEST_CASE("the state text keeps one value per key") {
    std::string text;
    text = state_set(text, "restore_token", "abc");
    text = state_set(text, "accel.R5", "enhance_pointer_precision:1");
    CHECK(state_get(text, "restore_token") == "abc");
    CHECK(state_get(text, "accel.R5") == "enhance_pointer_precision:1");
    text = state_set(text, "restore_token", "def");
    CHECK(state_get(text, "restore_token") == "def");
    text = state_set(text, "accel.R5", "");
    CHECK(state_get(text, "accel.R5").empty());
    CHECK(state_get(text, "missing").empty());
}

TEST_CASE("fill writes what the phone said and names what it did not") {
    // The Galaxy Tab S11 of docs/MATH.md: density 340, pointer speed 0,
    // Samsung's switch off, which gives a gain of 2.169.
    PhoneFacts facts;
    facts.reachable = true;
    facts.width = 1600;
    facts.height = 2560;
    facts.physical_width = 1600;
    facts.physical_height = 2560;
    facts.dpi_x = 274.0;
    facts.dpi_y = 276.0;
    facts.density = 340;
    facts.rotation = 90;
    facts.has_pointer_speed = true;
    facts.pointer_speed = 0;
    facts.accel_key = "enhance_pointer_precision";
    facts.accel_before = "0";
    facts.accel_off = true;
    DeviceConfig device;
    CHECK(fill_device(facts, device).empty());
    CHECK(device.width == 1600);
    CHECK(device.height == 2560);
    CHECK(device.rotation == 90);
    CHECK(device.accel == AccelSetting::off);
    // The gain follows from the pointer speed and the density: none is typed.
    CHECK(device.gain == 0.0);
    CHECK(effective_gain(device) == doctest::Approx(2.169).epsilon(0.001));
    CHECK(device.diagonal_inch ==
          doctest::Approx(std::hypot(1600.0 / 274.0, 2560.0 / 276.0)).epsilon(0.0001));

    // A size the user overrode does not change the glass: the diagonal comes
    // from the panel's own pixels.
    PhoneFacts scaled = facts;
    scaled.width = 1200;
    scaled.height = 1920;
    DeviceConfig lowered;
    CHECK(fill_device(scaled, lowered).empty());
    CHECK(lowered.width == 1200);
    CHECK(lowered.diagonal_inch == device.diagonal_inch);

    // Nothing the phone left out is made up, and what was there is kept.
    PhoneFacts little;
    little.reachable = true;
    little.width = 1080;
    little.height = 2400;
    DeviceConfig other;
    other.rotation = 270;
    other.gain = 1.5;
    const std::vector<std::string> missing = fill_device(little, other);
    CHECK(other.width == 1080);
    CHECK(other.rotation == 270);
    CHECK(other.gain == 1.5);
    CHECK(other.diagonal_inch == 0.0);
    CHECK(other.accel == AccelSetting::automatic);
    CHECK(missing.size() == 4);
}
