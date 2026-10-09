// SPDX-License-Identifier: MIT
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "third_party/doctest.h"

#include "aoahid_share/accel_model.hpp"
#include "aoahid_share/config.hpp"
#include "aoahid_share/geometry.hpp"
#include "aoahid_share/keymap.hpp"
#include "aoahid_share/position_cell.hpp"
#include "aoahid_share/session.hpp"
#include "aoahid_share/tracker.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

using namespace aoas;

namespace {

// Two monitors side by side, the second lower and shorter than the first.
std::vector<Monitor> two_monitors() {
    return {{"A", {0, 0, 1920, 1080}, 527.0, 296.0}, {"B", {1920, 142, 1423, 800}, 600.0, 340.0}};
}

// What Android does with a relative mouse: scale by a gain, clamp to the
// display. It works in the display's own axes, so a mounted (rotated) phone
// is modelled by converting to view space only for the checks.
struct FakeAndroid final : Sink {
    Size logical{};
    Size view{};
    int mount{};
    double x{};
    double y{};
    double gain_lo{1.0};
    double gain_hi{1.0};
    std::mt19937 random{7};
    std::vector<uint16_t> keys_down;
    unsigned buttons_down{};
    int jumps{};

    void move(const Delta counts) override {
        const double gain = std::uniform_real_distribution<double>(gain_lo, gain_hi)(random);
        x = std::clamp(x + gain * counts.x, 0.0, logical.w - 1.0);
        y = std::clamp(y + gain * counts.y, 0.0, logical.h - 1.0);
    }
    void jump(const Delta counts) override {
        ++jumps;
        move(counts);
    }
    void scroll(const int wheel, const int pan) override { wheels.emplace_back(wheel, pan); }
    void button(const unsigned button, const bool down) override {
        if (down)
            buttons_down |= 1U << button;
        else
            buttons_down &= ~(1U << button);
    }
    void key(const uint16_t usage, const bool down) override {
        if (down)
            keys_down.push_back(usage);
        else
            std::erase(keys_down, usage);
    }
    void media(uint16_t, bool) override {}
    struct TouchCall {
        unsigned contact;
        int x;
        int y;
        bool down;
    };
    std::vector<TouchCall> touches;
    std::vector<std::pair<int, int>> wheels;
    void touch(const unsigned contact, const int x, const int y, const bool down) override {
        touches.push_back({contact, x, y, down});
    }
    std::vector<SwipePlan> swipes;
    void swipe(const SwipePlan& plan) override { swipes.push_back(plan); }

    // The cursor in view space, derived independently of to_device_delta().
    [[nodiscard]] double view_x() const {
        switch (mount) {
        case 90:
            return view.w - 1.0 - y;
        case 180:
            return view.w - 1.0 - x;
        case 270:
            return y;
        default:
            return x;
        }
    }
    [[nodiscard]] double view_y() const {
        switch (mount) {
        case 90:
            return x;
        case 180:
            return view.h - 1.0 - y;
        case 270:
            return view.h - 1.0 - x;
        default:
            return y;
        }
    }
};

bool inside(const Interval& range, const double value) {
    return value >= range.lo - 1e-6 && value <= range.hi + 1e-6;
}

// Distance of the real cursor from the edge it shares with the PC.
double real_edge_distance(const FakeAndroid& phone, const Side side) {
    switch (side) {
    case Side::right:
        return phone.view_x();
    case Side::left:
        return phone.view.w - 1.0 - phone.view_x();
    case Side::bottom:
        return phone.view_y();
    case Side::top:
        return phone.view.h - 1.0 - phone.view_y();
    }
    return 0.0;
}

} // namespace

TEST_CASE("outer spans leave out what the neighbour covers") {
    const auto monitors = two_monitors();
    const auto right = outer_spans(monitors, 0, Side::right);
    REQUIRE(right.size() == 2);
    CHECK(right[0].lo == 0);
    CHECK(right[0].hi == 141);
    CHECK(right[1].lo == 942);
    CHECK(right[1].hi == 1079);

    CHECK(outer_spans(monitors, 1, Side::left).empty());
    const auto far_right = outer_spans(monitors, 1, Side::right);
    REQUIRE(far_right.size() == 1);
    CHECK(far_right[0].lo == 142);
    CHECK(far_right[0].hi == 941);
    CHECK(outer_spans(monitors, 0, Side::top).size() == 1);
    CHECK(outer_spans(monitors, 1, Side::bottom).size() == 1);
}

TEST_CASE("a portal keeps to the outer edge and maps both ways") {
    const auto monitors = two_monitors();
    Portal portal;
    PortalRequest request;
    request.monitor = 1;
    request.side = Side::right;
    request.anchor_length = 500;
    request.android_length = 2560;
    REQUIRE(resolve_portal(monitors, request, portal).empty());
    CHECK(portal.segment.lo >= 142 + 40);
    CHECK(portal.segment.hi <= 941 - 40);
    CHECK(portal.to_android(portal.anchor.lo) == doctest::Approx(0.0));
    CHECK(portal.to_android(portal.anchor.hi) == doctest::Approx(2559.0));
    for (int t = portal.segment.lo; t <= portal.segment.hi; t += 37)
        CHECK(portal.to_pc(portal.to_android(t)) == t);

    // Entirely on the part the second monitor covers: nothing to cross at.
    request.monitor = 0;
    request.centered = false;
    request.anchor_start = 300;
    request.anchor_length = 400;
    CHECK_FALSE(resolve_portal(monitors, request, portal).empty());

    // Reaching past the covered part keeps the longer outer piece.
    request.anchor_start = 800;
    request.anchor_length = 250;
    request.corner_margin = 0.0;
    REQUIRE(resolve_portal(monitors, request, portal).empty());
    CHECK(portal.segment.lo == 942);
    CHECK(portal.segment.hi == 1049);
}

TEST_CASE("view size and mount rotation") {
    CHECK(view_size({1600, 2560}, 0, 0).w == 1600);
    CHECK(view_size({1600, 2560}, 90, 0).w == 2560);
    CHECK(view_size({1600, 2560}, 90, 90).w == 1600);
    const Delta right{5, 0};
    CHECK(to_device_delta(0, right).x == 5);
    CHECK(to_device_delta(90, right).y == -5);
    CHECK(to_device_delta(180, right).x == -5);
    CHECK(to_device_delta(270, right).y == 5);
}

TEST_CASE("the acceleration curve joins up and never falls below its base") {
    // AOSP's constants leave a step of about 0.05% where two segments meet.
    const double edges[] = {32.002, 52.83, 119.124};
    for (const double edge : edges) {
        CHECK(curve_gain(0, edge - 1e-6) ==
              doctest::Approx(curve_gain(0, edge + 1e-6)).epsilon(1e-3));
    }
    CHECK(curve_gain(0, 0.0) == doctest::Approx(0.64 * 3.19));
    double last = 0.0;
    for (double speed = 1.0; speed < 2000.0; speed *= 1.3) {
        const double gain = curve_gain(0, speed);
        CHECK(gain >= last);
        CHECK(gain >= curve_gain(0, 0.0) - 0.002);
        last = gain;
    }
    CHECK(curve_gain(7, 10.0) == doctest::Approx(2.0 * curve_gain(0, 10.0)));

    const AccelModel exact(AccelMode::exact, 0, 0.2, 10.0);
    CHECK(exact.range(100.0, 0.01).lo == 1.0);
    CHECK(exact.floor() == 1.0);
    const AccelModel curve(AccelMode::curve, 0, 0.2, 10.0);
    const GainRange range = curve.range(40.0, 0.008);
    CHECK(range.lo <= range.hi);
    CHECK(range.lo >= curve.floor());
}

TEST_CASE("round trips keep the estimate around the real cursor") {
    const Side sides[] = {Side::left, Side::right, Side::top, Side::bottom};
    const int angles[] = {0, 90, 180, 270};
    const double sensitivities[] = {0.3, 1.0, 2.7};
    std::mt19937 random(11);

    for (const Side side : sides) {
        for (const int mount : angles) {
            for (const int rotation : angles) {
                for (const int variant : {0, 1, 2, 3}) {
                    const bool exact = variant < 2;
                    const bool park = variant % 2 != 0;
                    for (const double sensitivity : sensitivities) {
                        CAPTURE(park);
                        CAPTURE(static_cast<int>(side));
                        CAPTURE(mount);
                        CAPTURE(rotation);
                        CAPTURE(exact);
                        CAPTURE(sensitivity);

                        FakeAndroid phone;
                        phone.mount = mount;
                        phone.view = view_size({1600, 2560}, rotation, mount);
                        phone.logical = view_size({1600, 2560}, rotation, 0);
                        phone.x = 123.0;
                        phone.y = 456.0;
                        if (!exact) {
                            phone.gain_lo = 2.04;
                            phone.gain_hi = 9.6;
                        }

                        Portal portal;
                        portal.side = side;
                        portal.anchor = {200, 900};
                        portal.segment = {200, 900};
                        portal.android_length = vertical(side) ? phone.view.h : phone.view.w;

                        MotionConfig motion;
                        motion.scale_x = motion.scale_y = sensitivity;
                        motion.park = park;
                        const AccelModel accel(exact ? AccelMode::exact : AccelMode::unknown, 0,
                                               2.04, 9.6);
                        Session session(phone);
                        session.configure(portal, phone.view, mount, motion, accel);

                        double now = 1.0;
                        for (int trip = 0; trip < 20; ++trip) {
                            const int t = std::uniform_int_distribution<int>(200, 900)(random);
                            REQUIRE(session.edge_hit(t, 1.0, false, now));
                            REQUIRE(session.remote());
                            REQUIRE(inside(session.tracker().x(), phone.view_x()));
                            REQUIRE(inside(session.tracker().y(), phone.view_y()));
                            if (exact) {
                                const double along =
                                    vertical(side) ? phone.view_y() : phone.view_x();
                                CHECK(std::fabs(along - portal.to_android(t)) <= 0.5 + 1e-9);
                                CHECK(real_edge_distance(phone, side) == 0.0);
                            }

                            // Wander without going back.
                            for (int step = 0; step < 40; ++step) {
                                now += 0.004;
                                const double dx =
                                    std::uniform_int_distribution<int>(-40, 40)(random);
                                const double dy =
                                    std::uniform_int_distribution<int>(-40, 40)(random);
                                if (session.motion(dx, dy, now)) {
                                    // Only a real arrival at the shared edge may end the visit.
                                    REQUIRE(real_edge_distance(phone, side) == 0.0);
                                    break;
                                }
                                REQUIRE(inside(session.tracker().x(), phone.view_x()));
                                REQUIRE(inside(session.tracker().y(), phone.view_y()));
                            }

                            // Push back toward the PC until it lets go.
                            const double push = 30.0;
                            double dx = 0.0;
                            double dy = 0.0;
                            switch (side) {
                            case Side::right:
                                dx = -push;
                                break;
                            case Side::left:
                                dx = push;
                                break;
                            case Side::bottom:
                                dy = -push;
                                break;
                            case Side::top:
                                dy = push;
                                break;
                            }
                            int pushes = 0;
                            while (session.remote()) {
                                now += 0.004;
                                if (session.motion(dx, dy, now)) {
                                    REQUIRE(real_edge_distance(phone, side) == 0.0);
                                    const int back = session.leave();
                                    CHECK(back >= portal.segment.lo);
                                    CHECK(back <= portal.segment.hi);
                                    if (park) {
                                        // Android's own bottom right, whatever the mount.
                                        CHECK(phone.x == phone.logical.w - 1.0);
                                        CHECK(phone.y == phone.logical.h - 1.0);
                                    }
                                }
                                REQUIRE(++pushes < 5000);
                            }
                            now += 1.0;
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("the second entry aligns from where the cursor was left") {
    FakeAndroid phone;
    phone.view = phone.logical = {1600, 2560};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {0, 1079};
    portal.android_length = 2560;
    Session session(phone);
    session.configure(portal, phone.view, 0, MotionConfig{}, AccelModel{});

    session.enter(100, 1.0);
    // The corner move is marked to go out on its own.
    CHECK(phone.jumps == 1);
    CHECK(phone.y == doctest::Approx(portal.to_android(100)).epsilon(0.001));
    session.motion(50.0, 300.0, 1.01);
    while (!session.motion(-40.0, 0.0, 1.02)) {
    }
    const double left_at = phone.y;
    session.leave();

    // No corner move this time: the cursor goes straight to the new height.
    phone.x = 0.0;
    session.enter(700, 2.0);
    CHECK(phone.jumps == 1);
    CHECK(phone.x == 0.0);
    CHECK(phone.y == doctest::Approx(portal.to_android(700)).epsilon(0.001));
    CHECK(phone.y != doctest::Approx(left_at));
}

TEST_CASE("a parked cursor enters with one report") {
    FakeAndroid phone;
    phone.view = phone.logical = {1600, 2560};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {0, 1079};
    portal.android_length = 2560;
    MotionConfig motion;
    motion.park = true;
    Session session(phone);
    session.configure(portal, phone.view, 0, motion, AccelModel{});

    // First entry: the corner reference, then the height.
    session.enter(100, 1.0);
    CHECK(phone.jumps == 1);
    session.motion(50.0, 300.0, 1.01);
    while (!session.motion(-40.0, 0.0, 1.02)) {
    }
    session.leave();
    CHECK(phone.jumps == 2);
    CHECK(phone.x == 1599.0);
    CHECK(phone.y == 2559.0);

    // From the corner, one report reaches the shared edge at the new height.
    session.enter(700, 2.0);
    CHECK(phone.jumps == 3);
    CHECK(phone.x == 0.0);
    CHECK(phone.y == doctest::Approx(portal.to_android(700)).epsilon(0.001));
    CHECK(session.tracker().x().width() == 0.0);
    CHECK(inside(session.tracker().y(), phone.y));
}

TEST_CASE("an imported config keeps this computer's adb section") {
    Config imported;
    REQUIRE(parse_config("[adb]\nkill_server = false\npath = /tmp/other\nfirst_port = 7000\n"
                         "--- device ---\n[device]\nserial = A1\n[motion]\nsensitivity = 0.5\n"
                         "[adb]\nproxy = true\nport = 7100\n",
                         imported)
                .empty());
    Config local;
    local.adb_path = "/usr/bin/adb";
    CHECK(keep_local_adb(imported, local));
    CHECK(imported.adb_kill_server);
    CHECK(imported.adb_path == "/usr/bin/adb");
    CHECK(imported.adb_first_port == local.adb_first_port);
    // A device's proxy switch and port are this computer's too; its motion is not.
    CHECK(!imported.devices[0].adb_proxy);
    CHECK(imported.devices[0].adb_port == 0);
    CHECK(imported.devices[0].motion.sensitivity == 0.5);
    CHECK(!keep_local_adb(imported, local));
    // A device this computer has already keeps its own proxy.
    local.devices.resize(1);
    local.devices[0].serial = "A1";
    local.devices[0].adb_proxy = true;
    CHECK(keep_local_adb(imported, local));
    CHECK(imported.devices[0].adb_proxy);
    // A local device's name is not a serial: it passes its proxy on to no one.
    local.devices[0].serial = "B2";
    local.devices[0].name = "A1";
    imported.devices[0].adb_proxy = true;
    CHECK(keep_local_adb(imported, local));
    CHECK(!imported.devices[0].adb_proxy);
}

TEST_CASE("a value that was never given is written as nothing, and reads back so") {
    DeviceConfig device;
    REQUIRE(parse_device("[device]\nserial = S1\n", device).empty());
    const std::string file = format_device(device);
    CHECK(file.find("auto") == std::string::npos);
    // Nothing is written for what was not given, not even an empty value.
    CHECK(file.find("width") == std::string::npos);
    CHECK(file.find("monitor_diagonal_inch") == std::string::npos);
    CHECK(file.find("[adb]") == std::string::npos);
    DeviceConfig back;
    REQUIRE(parse_device(file, back).empty());
    CHECK(back.width == 0);
    CHECK(back.rotation == -1);
    CHECK(back.accel == AccelSetting::automatic);
    CHECK(back.segment_start == centred_start);
}

TEST_CASE("the cursor crosses from one device to the one beside it") {
    // A stands upright; B sits at its right edge, touching rows 500 to 1500.
    FakeAndroid a;
    a.view = a.logical = {1600, 2560};
    FakeAndroid b;
    b.view = b.logical = {1080, 2400};
    Portal from_pc;
    from_pc.side = Side::right;
    from_pc.anchor = from_pc.segment = {0, 1079};
    from_pc.android_length = 2560;
    MotionConfig motion;
    motion.park = true;
    Session on_a(a);
    on_a.configure(from_pc, a.view, 0, motion, AccelModel{}, {{Side::right, {500, 1500}, 7}});
    // B touches no monitor: it is only reached through A.
    Session on_b(b);
    on_b.configure(Portal{}, b.view, 0, motion, AccelModel{}, {{Side::left, {0, 2399}, 3}}, false);
    CHECK(!on_b.edge_hit(100, 5.0, false, 1.0));

    on_a.enter(100, 1.0);
    // Above B's part of the edge the right edge is a wall.
    on_a.motion(0.0, -400.0, 1.01);
    for (int step = 0; step < 100; ++step)
        CHECK(!on_a.motion(40.0, 0.0, 1.02 + step * 0.004));
    CHECK(a.x == 1599.0);
    CHECK(a.y < 500.0);

    // Down into B's part, a push to the right leaves for B.
    on_a.motion(-200.0, 900.0, 2.0);
    bool left = false;
    for (int step = 0; step < 200 && !left; ++step)
        left = on_a.motion(40.0, 0.0, 2.1 + step * 0.004);
    REQUIRE(left);
    CHECK(on_a.exit() == 7);
    const double at = on_a.along(Side::right);
    CHECK(at == doctest::Approx(a.y));
    CHECK(at >= 500.0);
    CHECK(at <= 1500.0);
    on_a.leave();
    // A's cursor is parked again.
    CHECK(a.x == 1599.0);
    CHECK(a.y == 2559.0);

    // B takes it at its left edge, at the height the caller mapped to.
    on_b.enter_at(Side::left, 1200.0, 3.0);
    CHECK(on_b.remote());
    CHECK(b.x == 0.0);
    CHECK(b.y == doctest::Approx(1200.0).epsilon(0.001));
    // Pushing back left returns to A, not to the PC.
    bool back = false;
    for (int step = 0; step < 200 && !back; ++step)
        back = on_b.motion(-40.0, 0.0, 3.1 + step * 0.004);
    REQUIRE(back);
    CHECK(on_b.exit() == 3);
    CHECK(on_b.along(Side::left) == doctest::Approx(b.y));
}

TEST_CASE("crossing waits for the push, the segment and a free button") {
    FakeAndroid phone;
    phone.view = phone.logical = {1000, 1000};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {100, 500};
    portal.android_length = 1000;
    MotionConfig motion;
    motion.enter_push = 30.0;
    Session session(phone);
    session.configure(portal, phone.view, 0, motion, AccelModel{});

    CHECK_FALSE(session.edge_hit(50, 100.0, false, 1.0));
    CHECK_FALSE(session.edge_hit(300, 100.0, true, 1.0));
    CHECK_FALSE(session.edge_hit(300, 20.0, false, 1.0));
    CHECK_FALSE(session.edge_hit(300, 20.0, false, 2.0));
    CHECK(session.edge_hit(300, 20.0, false, 2.1));
}

TEST_CASE("leaving releases what was held") {
    FakeAndroid phone;
    phone.view = phone.logical = {1000, 1000};
    Portal portal;
    portal.anchor = portal.segment = {0, 999};
    portal.android_length = 1000;
    Session session(phone);
    session.configure(portal, phone.view, 0, MotionConfig{}, AccelModel{});

    session.key(0x04, true);
    CHECK(phone.keys_down.empty());

    session.enter(10, 1.0);
    session.key(0x04, true);
    session.key(0x04, true);
    session.key(0xE0, true);
    session.button(1, true);
    session.button(9, true);
    CHECK(phone.keys_down.size() == 2);
    CHECK(phone.buttons_down == 2U);
    session.leave();
    CHECK(phone.keys_down.empty());
    CHECK(phone.buttons_down == 0U);
    CHECK_FALSE(session.remote());
}

TEST_CASE("a locked session keeps the input until it is unlocked") {
    FakeAndroid phone;
    phone.view = phone.logical = {1600, 2560};
    Portal portal;
    portal.side = Side::right;
    portal.anchor = portal.segment = {0, 1079};
    portal.android_length = 2560;
    Session session(phone);
    session.configure(portal, phone.view, 0, MotionConfig{}, AccelModel{});

    session.enter(100, 1.0);
    session.motion(50.0, 300.0, 1.01);
    session.set_locked(true);
    CHECK(session.locked());
    // Pushed hard against the edge that faces the PC: nothing gives.
    for (int step = 0; step < 200; ++step)
        CHECK_FALSE(session.motion(-40.0, 0.0, 1.02 + step * 0.004));
    CHECK(session.remote());
    CHECK(phone.x == 0.0);
    CHECK(inside(session.tracker().x(), phone.view_x()));
    CHECK(inside(session.tracker().y(), phone.view_y()));

    // What was pushed while locked does not count: unlocking alone, and a
    // move away from the edge, leave the input where it is.
    session.set_locked(false);
    CHECK_FALSE(session.motion(40.0, 0.0, 2.0));
    CHECK(session.remote());
    // From here it returns as it always did.
    bool back = false;
    for (int step = 0; step < 200 && !back; ++step)
        back = session.motion(-40.0, 0.0, 2.01 + step * 0.004);
    CHECK(back);
    CHECK(session.exit() == Session::to_pc);

    // Leaving unlocks: the next visit is not held.
    session.set_locked(true);
    session.leave();
    CHECK_FALSE(session.locked());
}

TEST_CASE("a locked session does not hand the cursor to a neighbour") {
    FakeAndroid a;
    a.view = a.logical = {1600, 2560};
    Portal from_pc;
    from_pc.side = Side::right;
    from_pc.anchor = from_pc.segment = {0, 1079};
    from_pc.android_length = 2560;
    Session on_a(a);
    on_a.configure(from_pc, a.view, 0, MotionConfig{}, AccelModel{},
                   {{Side::right, {0, 2559}, 7}});
    on_a.enter(100, 1.0);
    on_a.set_locked(true);
    for (int step = 0; step < 200; ++step)
        CHECK_FALSE(on_a.motion(40.0, 0.0, 1.02 + step * 0.004));
    CHECK(a.x == 1599.0);
}

TEST_CASE("key codes and hotkeys") {
    HidKey key;
    REQUIRE(evdev_to_hid(30, key));
    CHECK(key.usage == 0x04);
    CHECK_FALSE(key.media);
    REQUIRE(evdev_to_hid(115, key));
    CHECK(key.usage == 0x00E9);
    CHECK(key.media);
    REQUIRE(evdev_to_hid(124, key));
    CHECK(key.usage == 0x89);
    CHECK_FALSE(evdev_to_hid(700, key));

    unsigned button = 0;
    REQUIRE(evdev_to_button(0x112, button));
    CHECK(button == 3);
    CHECK_FALSE(evdev_to_button(0x120, button));

    Hotkey hotkey;
    REQUIRE(parse_hotkey("Ctrl + Alt+S", hotkey));
    CHECK(hotkey.modifiers == (mod_ctrl | mod_alt));
    CHECK(hotkey.usage == 0x16);
    REQUIRE(parse_hotkey("ctrl+alt+shift+escape", hotkey));
    CHECK(hotkey.usage == 0x29);
    REQUIRE(parse_hotkey("f13", hotkey));
    CHECK(hotkey.usage == 0x68);
    REQUIRE(parse_hotkey("", hotkey));
    CHECK_FALSE(hotkey.set());
    CHECK_FALSE(parse_hotkey("ctrl+alt", hotkey));
    CHECK_FALSE(parse_hotkey("ctrl+nosuchkey", hotkey));
    CHECK_FALSE(parse_hotkey("a+b", hotkey));
}

TEST_CASE("config.ini round-trips and rejects what it does not know") {
    Config config;
    const char* text = "[daemon]\n"
                       "backend = portal\n"
                       "toggle_hotkey = ctrl+alt+k ; comment\n"
                       "[mouse]\n"
                       "buttons = 7\n"
                       "button_map = 1,3,2\n"
                       "[motion]\n"
                       "sensitivity = 2.5\n"
                       "report_rate_hz = 125\n"
                       "entry = corner\n"
                       "[media]\n"
                       "target = Tab\n"
                       "play_pause = ctrl+alt+p\n"
                       "[adb]\n"
                       "kill_server = false\n"
                       "first_port = 7100\n";
    REQUIRE(parse_globals(text, config).empty());
    CHECK(config.backend == "portal");
    // The 0.3.4 hotkeys are read and dropped: they live in each device's file now.
    CHECK(format_globals(config).find("hotkey") == std::string::npos);
    CHECK(format_globals(config).find("target") == std::string::npos);
    CHECK(format_globals(config).find("play_pause") == std::string::npos);
    CHECK(config.mouse_buttons == 7);
    CHECK(config.button_map.size() == 3);
    CHECK(config.motion.sensitivity == 2.5);
    CHECK(config.motion.entry == EntryMode::corner);
    CHECK(config.report_rate_hz == 125);
    CHECK(!config.report_every);
    CHECK(!config.adb_kill_server);
    CHECK(config.adb_first_port == 7100);
    CHECK(config.devices.empty());

    Config again;
    REQUIRE(parse_globals(format_globals(config), again).empty());
    CHECK(format_globals(again) == format_globals(config));

    Config every;
    REQUIRE(parse_globals("[motion]\nreport_rate_hz = every\n", every).empty());
    CHECK(every.report_every);
    REQUIRE(parse_globals(format_globals(every), again).empty());
    CHECK(again.report_every);

    Config untouched;
    CHECK(parse_globals("[motion]\nspeed = 2\n", untouched).substr(0, 7) == "line 2:");
    CHECK(parse_globals("[nope]\n", untouched).substr(0, 7) == "line 1:");
    CHECK_FALSE(parse_globals("[motion]\nsensitivity = 0\n", untouched).empty());
    CHECK_FALSE(parse_globals("[motion]\nreport_rate_hz = 9000\n", untouched).empty());
    CHECK(parse_globals("[media]\nplay_pause = ctrl+\n", untouched).empty());
    CHECK(parse_globals("[daemon]\npause_hotkey = ctrl+\n", untouched).empty());
    CHECK_FALSE(parse_globals("[media]\nwarp = ctrl+w\n", untouched).empty());
    CHECK_FALSE(parse_globals("[adb]\nfirst_port = 5555\n", untouched).empty());
    CHECK(untouched.backend == "auto");
}

TEST_CASE("a device's file round-trips and only replaces what it names") {
    DeviceConfig device;
    const char* text = "[device]\n"
                       "serial = R5GL153Y5EX\n"
                       "name = Galaxy Tab S11\n"
                       "hotkey = ctrl+alt+1\n"
                       "[placement]\n"
                       "monitor = HDMI-A-1\n"
                       "side = top\n"
                       "start = -120\n"
                       "corner_margin = 10%\n"
                       "rotation = 90\n"
                       "turn_input = 180\n"
                       "[detected]\n"
                       "width = 1600\n"
                       "gain = 2.1692\n"
                       "accel = off\n"
                       "read_at = 2026-10-05\n"
                       "[motion]\n"
                       "sensitivity = 1.4\n"
                       "park = false\n"
                       "[keyboard]\n"
                       "enabled = false\n"
                       "[mouse]\n"
                       "enabled = false\n"
                       "[keys]\n"
                       "switch = ctrl+alt+1\n"
                       "lock = ctrl+alt+l\n"
                       "play_pause = ctrl+alt+p\n"
                       "[adb]\n"
                       "port = 7000\n"
                       "proxy = true\n";
    REQUIRE(parse_device(text, device).empty());
    CHECK(device.keys.switch_key == "ctrl+alt+1");
    CHECK(device.keys.lock == "ctrl+alt+l");
    CHECK(device.keys.resync.empty());
    CHECK(device.keys.media[1] == "ctrl+alt+p");
    CHECK(device.keys.media[0].empty());
    // The 0.3.4 key is gone from what is written.
    CHECK(format_device(device).find("\nhotkey = ") == std::string::npos);
    DeviceConfig broken;
    CHECK_FALSE(parse_device("[keys]\nlock = ctrl+\n", broken).empty());
    CHECK_FALSE(parse_device("[keys]\nwarp = ctrl+w\n", broken).empty());
    CHECK(parse_device("[device]\nhotkey = ctrl+\n", broken).empty());
    CHECK(device.serial == "R5GL153Y5EX");
    CHECK(device.name == "Galaxy Tab S11");
    CHECK(label_of(device) == "Galaxy Tab S11");
    CHECK(device.enabled);
    CHECK(device.side == Side::top);
    // A start before the edge's own: the device reaches past the corner.
    CHECK(device.segment_start == -120);
    CHECK(device.segment_length == -1);
    CHECK(device.corner_margin == doctest::Approx(0.10));
    CHECK(device.rotation == 90);
    CHECK(device.mount_rotation == 180);
    CHECK(device.width == 1600);
    CHECK(device.height == 0);
    CHECK(device.accel == AccelSetting::off);
    CHECK(device.read_at == "2026-10-05");
    CHECK(device.adb_port == 7000);
    CHECK(device.adb_proxy);

    DeviceConfig again;
    REQUIRE(parse_device(format_device(device), again).empty());
    CHECK(format_device(again) == format_device(device));

    // What the file leaves out follows config.ini; what it names replaces it.
    Config config;
    config.motion.sensitivity = 2.0;
    config.motion.sensitivity_y = 3.0;
    config.motion.scroll_sensitivity = 0.7;
    const Motion motion = motion_of(config, device);
    CHECK(motion.sensitivity == 1.4);
    // Its own sensitivity is for both axes, not the global vertical one.
    CHECK(motion.sensitivity_y == 0.0);
    CHECK(motion.scroll_sensitivity == 0.7);
    CHECK(motion.entry == EntryMode::aligned);
    CHECK(motion.no_cross_while_button);
    CHECK(!keyboard_of(config, device));
    CHECK(keyboard_of(config, DeviceConfig{}));
    CHECK(!mouse_of(config, device));
    CHECK(mouse_of(config, DeviceConfig{}));
    // Media is its own switch: it does not follow the keyboard.
    CHECK(media_of(config, device));
    config.media = false;
    CHECK(!media_of(config, DeviceConfig{}));

    // A serial names a file; a name is shown and typed. Both come from
    // outside, so only plain ones are let in.
    CHECK(valid_serial("R5GL153Y5EX"));
    CHECK(!valid_serial(""));
    CHECK(!valid_serial("../x"));
    CHECK(!valid_serial(".hidden"));
    CHECK(!valid_serial("a b"));
    CHECK(!valid_serial("nul"));
    CHECK(!valid_serial("COM1.x"));
    CHECK(valid_name("Galaxy Tab S11"));
    CHECK(!valid_name("a=b"));
    CHECK(!valid_name("two\nlines"));
    CHECK(clean_name("  Pixel 8\n[adb]\ntune = true ") == "Pixel 8[adb]tune  true");
    // `;` and `#` would start a comment when the file is read back.
    CHECK(!valid_name("Tab #2"));
    CHECK(!valid_name("a ;b"));
    CHECK(clean_name("Tab #2 ; x") == "Tab 2  x");
    {
        // Whatever a device calls itself, its file reads back as written.
        DeviceConfig named;
        named.serial = "S1";
        named.name = clean_name("Galaxy #1 ; [adb]\rtune=true");
        DeviceConfig back;
        REQUIRE(parse_device(format_device(named), back).empty());
        CHECK(back.name == named.name);
        CHECK(format_device(back) == format_device(named));
    }
    CHECK(valid_name(clean_name(std::string(200, 'x'))));
    // The limit is in bytes; a character it would cut through is left out.
    const std::string cut = clean_name(std::string(47, 'x') + "\xC3\xA9");
    CHECK(cut == std::string(47, 'x'));
    CHECK(clean_name(std::string(46, 'x') + "\xC3\xA9") == std::string(46, 'x') + "\xC3\xA9");
    CHECK(valid_name(cut));
    {
        // A name that is another device's serial could never be reached.
        Config clash;
        clash.devices.resize(2);
        clash.devices[0].serial = "A1";
        clash.devices[0].name = "B2";
        clash.devices[1].serial = "B2";
        clash.devices[1].name = "Other";
        CHECK_FALSE(validate_config(clash).empty());
        clash.devices[0].name = "Tab";
        CHECK(validate_config(clash).empty());
    }
    {
        // An editor's byte order mark does not spoil the first line.
        Config marked;
        CHECK(parse_globals("\xEF\xBB\xBF[daemon]\nbackend = x11\n", marked).empty());
        CHECK(marked.backend == "x11");
    }
    {
        // The proxy is off until it is switched on, for a device, by hand.
        const DeviceConfig fresh;
        CHECK(!fresh.adb_proxy);
        CHECK(Config{}.adb_kill_server);
    }

    DeviceConfig untouched;
    CHECK_FALSE(parse_device("[device]\nserial = a/b\n", untouched).empty());
    CHECK_FALSE(parse_device("[placement]\nrotation = 45\n", untouched).empty());
    CHECK_FALSE(parse_device("[adb]\nport = 5555\n", untouched).empty());
    CHECK_FALSE(parse_device("[motion]\nreport_rate_hz = 9000\n", untouched).empty());
    CHECK_FALSE(parse_device("[daemon]\nbackend = x11\n", untouched).empty());
    CHECK(untouched.serial.empty());
}

TEST_CASE("devices get their ports, and what clashes between them is refused") {
    Config config;
    config.adb_first_port = 6555;
    config.devices.resize(4);
    config.devices[0].serial = "A";
    config.devices[1].serial = "B";
    config.devices[1].adb_port = 6556;
    config.devices[2].serial = "C";
    config.devices[3].serial = "D";
    config.devices[3].name = "Phone";
    // The free ports upward from the first, around the one B took.
    CHECK(adb_port_of(config, 0) == 6555);
    CHECK(adb_port_of(config, 1) == 6556);
    CHECK(adb_port_of(config, 2) == 6557);
    CHECK(adb_port_of(config, 3) == 6558);
    CHECK(validate_config(config).empty());
    CHECK(find_device(config, "C") == 2);
    CHECK(find_device(config, "Phone") == 3);
    CHECK(find_device(config, "nobody") == 4);

    Config clash = config;
    clash.devices[2].serial = "A";
    CHECK_FALSE(validate_config(clash).empty());
    // On a file system that ignores case these would be one file.
    clash.devices[2].serial = "a";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    clash.devices[0].name = "Phone";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    // A switch or media key works wherever the input is: nothing else,
    // on any device, may be the same combination.
    clash.devices[0].keys.switch_key = "ctrl+alt+1";
    clash.devices[1].keys.switch_key = "alt+ctrl+1";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    clash.devices[0].keys.media[1] = "ctrl+alt+p";
    clash.devices[1].keys.media[1] = "ctrl+alt+p";
    CHECK_FALSE(validate_config(clash).empty());
    clash = config;
    clash.devices[0].keys.switch_key = "ctrl+alt+1";
    clash.devices[1].keys.lock = "ctrl+alt+1";
    CHECK_FALSE(validate_config(clash).empty());
    // Within one device every key is its own.
    clash = config;
    clash.devices[0].keys.lock = "ctrl+alt+l";
    clash.devices[0].keys.resync = "ctrl+alt+l";
    CHECK_FALSE(validate_config(clash).empty());
    // Lock and resync act on the device that has the input, so two devices
    // may use the same keys for them.
    Config shared = config;
    shared.devices[0].keys.switch_key = "ctrl+alt+1";
    shared.devices[1].keys.switch_key = "ctrl+alt+2";
    shared.devices[0].keys.lock = shared.devices[1].keys.lock = "ctrl+alt+l";
    shared.devices[0].keys.resync = shared.devices[1].keys.resync = "ctrl+alt+r";
    CHECK(validate_config(shared).empty());

    // Beside another device is fine; beside nothing or in a ring is not.
    Config chain = config;
    chain.devices[1].beside = "A";
    chain.devices[2].beside = "B";
    CHECK(validate_config(chain).empty());
    chain.devices[0].beside = "C";
    CHECK_FALSE(validate_config(chain).empty());
    chain = config;
    chain.devices[1].beside = "Z";
    CHECK_FALSE(validate_config(chain).empty());
}

TEST_CASE("everything in one text reads back as devices") {
    Config config;
    config.motion.sensitivity = 1.5;
    config.devices.resize(2);
    config.devices[0].serial = "A";
    config.devices[0].name = "Tab";
    config.devices[0].side = Side::left;
    config.devices[1].serial = "B";
    config.devices[1].beside = "A";
    config.devices[1].motion.scroll_sensitivity = 0.5;
    Config back;
    REQUIRE(parse_config(format_config(config), back).empty());
    CHECK(format_config(back) == format_config(config));
    REQUIRE(back.devices.size() == 2);
    CHECK(back.devices[1].beside == "A");
    CHECK(back.devices[1].motion.scroll_sensitivity == 0.5);

    // No more devices than the limit, however the text names them.
    std::string many = "[motion]\n";
    for (size_t index = 0; index <= max_devices; ++index)
        many += "--- device ---\n[device]\nserial = S" + std::to_string(index) + "\n";
    Config flood;
    CHECK_FALSE(parse_config(many, flood).empty());
}

TEST_CASE("a view point maps to the touch device's raw coordinates") {
    const Size natural{1600, 2560};
    CHECK(to_touch_point(natural, 0, 0, 10.0, 20.0).x == 10);
    CHECK(to_touch_point(natural, 0, 0, 10.0, 20.0).y == 20);
    // Rotation 90: display (dx, dy) is raw (Wn-1-dy, dx).
    const TouchPoint r90 = to_touch_point(natural, 90, 0, 100.0, 30.0);
    CHECK(r90.x == 1600 - 1 - 30);
    CHECK(r90.y == 100);
    const TouchPoint r180 = to_touch_point(natural, 180, 0, 100.0, 30.0);
    CHECK(r180.x == 1600 - 1 - 100);
    CHECK(r180.y == 2560 - 1 - 30);
    // Rotation 270: display (dx, dy) is raw (dy, Hn-1-dx).
    const TouchPoint r270 = to_touch_point(natural, 270, 0, 100.0, 30.0);
    CHECK(r270.x == 30);
    CHECK(r270.y == 2560 - 1 - 100);
    // Mount 90: view (vx, vy) is display (vy, W-1-vx), W being the view width.
    const Size view90 = view_size(natural, 0, 90);
    const TouchPoint m90 = to_touch_point(natural, 0, 90, 100.0, 30.0);
    CHECK(m90.x == 30);
    CHECK(m90.y == view90.w - 1 - 100);
}

TEST_CASE("every rotation and mount keeps the touch point on the device") {
    const Size natural{1600, 2560};
    for (const int rotation : {0, 90, 180, 270}) {
        for (const int mount : {0, 90, 180, 270}) {
            const Size view = view_size(natural, rotation, mount);
            // The four corners of the view land on four different corners of
            // the raw area.
            std::vector<TouchPoint> corners;
            for (const double x : {0.0, view.w - 1.0}) {
                for (const double y : {0.0, view.h - 1.0}) {
                    const TouchPoint p = to_touch_point(natural, rotation, mount, x, y);
                    CHECK((p.x == 0 || p.x == natural.w - 1));
                    CHECK((p.y == 0 || p.y == natural.h - 1));
                    corners.push_back(p);
                }
            }
            for (size_t a = 0; a < corners.size(); ++a) {
                for (size_t b = a + 1; b < corners.size(); ++b)
                    CHECK((corners[a].x != corners[b].x || corners[a].y != corners[b].y));
            }
            // A point outside the view is clamped, never wrapped.
            const TouchPoint outside = to_touch_point(natural, rotation, mount, 1e6, -1e6);
            CHECK(outside.x >= 0);
            CHECK(outside.x <= natural.w - 1);
            CHECK(outside.y >= 0);
            CHECK(outside.y <= natural.h - 1);
        }
    }
}

TEST_CASE("the touch settings are read, written back and overridden per device") {
    Config config;
    REQUIRE(parse_globals("[touch]\nenabled = true\nscroll = 120\nscroll_pan = -80\n"
                          "scroll_release_ms = 300\ntap_button = 2\n",
                          config)
                .empty());
    CHECK(config.touch.enabled);
    CHECK(config.touch.scroll == 120);
    CHECK(config.touch.scroll_pan == -80);
    CHECK(config.touch.release_ms == 300);
    CHECK(config.touch.button == 2);
    Config back;
    REQUIRE(parse_globals(format_globals(config), back).empty());
    CHECK(format_globals(back) == format_globals(config));

    DeviceConfig device;
    REQUIRE(parse_device("[device]\nserial = S1\n[touch]\nenabled = false\nscroll = 0\n", device)
                .empty());
    const Touch seen = touch_of(config, device);
    CHECK_FALSE(seen.enabled);
    CHECK(seen.scroll == 0);
    // What the file leaves out follows config.ini.
    CHECK(seen.scroll_pan == -80);
    CHECK(seen.release_ms == 300);
    CHECK(seen.button == 2);
    DeviceConfig again;
    REQUIRE(parse_device(format_device(device), again).empty());
    CHECK(format_device(again) == format_device(device));

    // Values out of range, and unknown keys, are refused.
    Config bad;
    CHECK_FALSE(parse_globals("[touch]\nscroll = 5000\n", bad).empty());
    CHECK_FALSE(parse_globals("[touch]\nscroll_release_ms = -1\n", bad).empty());
    CHECK_FALSE(parse_globals("[touch]\ntap_button = 0\n", bad).empty());
    CHECK_FALSE(parse_globals("[touch]\nnonsense = 1\n", bad).empty());
}

namespace {

struct TouchRig {
    FakeAndroid phone;
    Session session{phone};
    Size natural{1600, 2560};
    int rotation;
    int mount;
    explicit TouchRig(const int rotation_ = 0, const int mount_ = 0)
        : rotation(rotation_), mount(mount_) {
        phone.view = phone.logical = view_size(natural, rotation, mount);
        Portal portal;
        portal.side = Side::right;
        portal.anchor = portal.segment = {0, 1079};
        portal.android_length = phone.view.h;
        session.configure(portal, phone.view, mount, MotionConfig{}, AccelModel{});
        TouchSetup setup;
        setup.enabled = true;
        setup.natural = natural;
        setup.rotation = rotation;
        setup.button = 1;
        setup.scroll = 100;
        setup.scroll_pan = 100;
        setup.release_s = 0.2;
        session.set_touch(setup);
        session.enter(100, 1.0);
    }
    // Where the session's tracked cursor is, in the device's raw coordinates.
    [[nodiscard]] TouchPoint here() const {
        return to_touch_point(natural, rotation, mount, session.tracker().x().mid(),
                              session.tracker().y().mid());
    }
};

} // namespace

TEST_CASE("the left button taps at the tracked position and the others stay mouse buttons") {
    TouchRig rig;
    rig.session.button(1, true);
    REQUIRE(rig.phone.touches.size() == 1);
    CHECK(rig.phone.touches[0].contact == 0);
    CHECK(rig.phone.touches[0].down);
    CHECK(rig.phone.touches[0].x == rig.here().x);
    CHECK(rig.phone.touches[0].y == rig.here().y);
    CHECK(rig.phone.buttons_down == 0);
    rig.session.button(1, false);
    REQUIRE(rig.phone.touches.size() == 2);
    CHECK_FALSE(rig.phone.touches[1].down);
    rig.session.button(2, true);
    CHECK(rig.phone.buttons_down == (1U << 2));
    CHECK(rig.phone.touches.size() == 2);
}

TEST_CASE("moving while the tap is held drags contact 0") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.session.motion(30.0, 10.0, 1.1);
    REQUIRE(rig.phone.touches.size() == 2);
    CHECK(rig.phone.touches[1].contact == 0);
    CHECK(rig.phone.touches[1].down);
    CHECK(rig.phone.touches[1].x == rig.here().x);
    CHECK(rig.phone.touches[1].y == rig.here().y);
}

TEST_CASE("a wheel notch is a swipe of the second finger, independent of contact 0") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.phone.touches.clear();
    const TouchPoint start = rig.here();
    rig.session.scroll(1.0, 0.0);
    // A positive wheel scrolls the view up, so the content and the finger go
    // down. The sink does the putting down, the moves and the lift.
    REQUIRE(rig.phone.swipes.size() == 1);
    const SwipePlan& plan = rig.phone.swipes[0];
    CHECK(plan.x == start.x);
    CHECK(plan.y == start.y);
    CHECK(plan.dx == 0);
    CHECK(plan.dy == 100);
    CHECK(plan.area.w == rig.natural.w);
    CHECK(plan.area.h == rig.natural.h);
    CHECK(plan.steps == 4);
    CHECK_FALSE(plan.restart);
    // The next notch is another swipe from the cursor, not one notch further:
    // adding to a swipe that goes on is the sink's.
    rig.session.scroll(1.0, 0.0);
    REQUIRE(rig.phone.swipes.size() == 2);
    CHECK(rig.phone.swipes[1].y == start.y);
    CHECK(rig.phone.swipes[1].dy == 100);
    // The finger of the click was not touched, and the wheel was taken.
    CHECK(rig.phone.touches.empty());
    CHECK(rig.phone.wheels.empty());
}

TEST_CASE("the swipe settings reach the sink") {
    TouchRig rig;
    TouchSetup setup;
    setup.enabled = true;
    setup.natural = rig.natural;
    setup.scroll = 100;
    setup.steps = 7;
    setup.start_s = 0.002;
    setup.total_s = 0.03;
    setup.release_s = 0.0;
    setup.restart = true;
    rig.session.set_touch(setup);
    rig.session.scroll(1.0, 0.0);
    REQUIRE(rig.phone.swipes.size() == 1);
    CHECK(rig.phone.swipes[0].steps == 7);
    CHECK(rig.phone.swipes[0].start_s == 0.002);
    CHECK(rig.phone.swipes[0].total_s == 0.03);
    CHECK(rig.phone.swipes[0].release_s == 0.0);
    CHECK(rig.phone.swipes[0].restart);
}

TEST_CASE("a notch that cannot move the finger does not tap") {
    TouchRig rig;
    // Push the cursor against the top edge, then scroll up: the finger would
    // be put down at the edge and lifted without moving, which is a tap.
    rig.session.motion(0.0, -5000.0, 1.5);
    rig.phone.touches.clear();
    rig.session.scroll(-1.0, 0.0);
    CHECK(rig.phone.swipes.empty());
    // The other way it moves.
    rig.session.scroll(1.0, 0.0);
    CHECK_FALSE(rig.phone.swipes.empty());
}

TEST_CASE("changing the touch setup lifts a contact that is down") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.session.scroll(1.0, 0.0);
    rig.phone.touches.clear();
    TouchSetup off;
    off.natural = rig.natural;
    rig.session.set_touch(off);
    int lifts0 = 0;
    int lifts1 = 0;
    for (const auto& call : rig.phone.touches) {
        if (!call.down && call.contact == 0)
            ++lifts0;
        if (!call.down && call.contact == 1)
            ++lifts1;
    }
    CHECK(lifts0 == 1);
    CHECK(lifts1 == 1);
    // Switched on again, a movement places nothing: no button is held.
    TouchSetup on;
    on.enabled = true;
    on.natural = rig.natural;
    rig.session.set_touch(on);
    rig.phone.touches.clear();
    rig.session.motion(10.0, 10.0, 3.0);
    CHECK(rig.phone.touches.empty());
}

TEST_CASE("a tap button above the mouse's button count still taps") {
    TouchRig rig;
    TouchSetup setup;
    setup.enabled = true;
    setup.natural = rig.natural;
    setup.button = 7;
    rig.session.set_touch(setup);
    rig.session.button(7, true);
    REQUIRE(rig.phone.touches.size() == 1);
    CHECK(rig.phone.touches[0].contact == 0);
    CHECK(rig.phone.touches[0].down);
}

TEST_CASE("a swipe stays on the display") {
    TouchRig rig;
    for (int notch = 0; notch < 100; ++notch)
        rig.session.scroll(-1.0, 1.0);
    REQUIRE_FALSE(rig.phone.swipes.empty());
    for (const auto& plan : rig.phone.swipes) {
        CHECK(plan.x + plan.dx >= 0);
        CHECK(plan.x + plan.dx < rig.natural.w);
        CHECK(plan.y + plan.dy >= 0);
        CHECK(plan.y + plan.dy < rig.natural.h);
    }
}

TEST_CASE("a wheel stays a mouse wheel while touch scroll is 0") {
    TouchRig rig;
    TouchSetup setup;
    setup.enabled = true;
    setup.natural = rig.natural;
    rig.session.set_touch(setup);
    rig.session.scroll(2.0, 0.0);
    CHECK(rig.phone.touches.empty());
    REQUIRE(rig.phone.wheels.size() == 1);
    CHECK(rig.phone.wheels[0].first == 2);
}

TEST_CASE("leaving lifts the touch contacts") {
    TouchRig rig;
    rig.session.button(1, true);
    rig.session.scroll(1.0, 0.0);
    rig.phone.touches.clear();
    rig.session.leave();
    int lifts0 = 0;
    int lifts1 = 0;
    for (const auto& call : rig.phone.touches) {
        if (!call.down && call.contact == 0)
            ++lifts0;
        if (!call.down && call.contact == 1)
            ++lifts1;
    }
    CHECK(lifts0 == 1);
    CHECK(lifts1 == 1);
}

TEST_CASE("the tap follows every rotation and mount") {
    for (const int rotation : {0, 90, 180, 270}) {
        for (const int mount : {0, 90, 180, 270}) {
            TouchRig rig(rotation, mount);
            rig.session.button(1, true);
            REQUIRE(rig.phone.touches.size() == 1);
            CHECK(rig.phone.touches[0].x == rig.here().x);
            CHECK(rig.phone.touches[0].y == rig.here().y);
        }
    }
}

TEST_CASE("the position cell hands over a whole range, never half of two") {
    PositionCell cell;
    Tracker tracker;
    tracker.reset({101, 51});
    cell.store(tracker);
    PositionCell::Value value = cell.load();
    CHECK(value.x_lo == 0.0);
    CHECK(value.x_hi == 100.0);
    CHECK(value.y_hi == 50.0);

    // The writer keeps all four numbers equal to one another plus a fixed
    // step; a reader that ever sees them out of step has seen a torn write.
    tracker.reset({1001, 1001});
    cell.store(tracker);
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int step = 1; !stop.load(); ++step) {
            tracker.reset({step + 1000, step + 1000});
            cell.store(tracker);
        }
    });
    bool whole = true;
    for (int read = 0; read < 200000; ++read) {
        const PositionCell::Value seen = cell.load();
        whole = whole && seen.x_hi == seen.y_hi && seen.x_lo == 0.0 && seen.y_lo == 0.0;
    }
    stop.store(true);
    writer.join();
    CHECK(whole);
}

TEST_CASE("the gain follows Android's pointer speed and density") {
    // The Galaxy Tab S11 of docs/MATH.md: density 340, pointer speed 0 gave 2.169.
    CHECK(low_speed_gain(0, 340) == doctest::Approx(2.169).epsilon(0.001));
    CHECK(low_speed_gain(3, 340) == doctest::Approx(2.820).epsilon(0.001));
    CHECK(low_speed_gain(-4, 340) == doctest::Approx(1.301).epsilon(0.001));
    CHECK(low_speed_gain(0, 0) == 0.0);
    // A faster setting moves the cursor further for each count.
    CHECK(low_speed_gain(5, 340) > low_speed_gain(0, 340));

    DeviceConfig device;
    REQUIRE(parse_device("[detected]\ndensity = 340\npointer_speed = 3\n", device).empty());
    CHECK(device.density_dpi == 340);
    DeviceConfig back;
    REQUIRE(parse_device(format_device(device), back).empty());
    CHECK(back.density_dpi == 340);
    CHECK(back.pointer_speed == 3);
    DeviceConfig none;
    CHECK(format_device(none).find("density") == std::string::npos);
}

TEST_CASE("the gain follows the pointer speed unless one is typed") {
    DeviceConfig device;
    device.density_dpi = 340;
    device.pointer_speed = 0;
    CHECK(effective_gain(device) == doctest::Approx(2.169).epsilon(0.001));
    // Changing the pointer speed changes the gain at once.
    device.pointer_speed = 3;
    CHECK(effective_gain(device) == doctest::Approx(2.820).epsilon(0.001));
    // A typed (measured) gain wins.
    device.gain = 1.5;
    CHECK(effective_gain(device) == 1.5);
    // Without a density there is nothing to calculate.
    DeviceConfig bare;
    CHECK(effective_gain(bare) == 0.0);

    // A gain in a file that is only what the speed and density give (an old
    // "fill" wrote it) is not kept as a value of its own.
    DeviceConfig stale;
    REQUIRE(parse_device("[detected]\ndensity = 340\npointer_speed = 0\ngain = 2.169\n", stale)
                .empty());
    CHECK(stale.gain == 0.0);
    // One that differs is the user's own.
    DeviceConfig own;
    REQUIRE(
        parse_device("[detected]\ndensity = 340\npointer_speed = 0\ngain = 1.5\n", own).empty());
    CHECK(own.gain == 1.5);
}

TEST_CASE("how the cursor enters is one setting, and the old two keys still read") {
    Config config;
    REQUIRE(parse_globals("[motion]\nentry = aligned\n", config).empty());
    CHECK(config.motion.entry == EntryMode::aligned);
    Config back;
    REQUIRE(parse_globals(format_globals(config), back).empty());
    CHECK(back.motion.entry == EntryMode::aligned);
    CHECK(format_globals(config).find("park") == std::string::npos);

    // What earlier files said: park = true|false and entry = align|corner.
    Config old_parked;
    REQUIRE(parse_globals("[motion]\npark = true\nentry = align\n", old_parked).empty());
    CHECK(old_parked.motion.entry == EntryMode::parked);
    Config old_aligned;
    REQUIRE(parse_globals("[motion]\npark = false\nentry = align\n", old_aligned).empty());
    CHECK(old_aligned.motion.entry == EntryMode::aligned);
    Config old_corner;
    REQUIRE(parse_globals("[motion]\npark = true\nentry = corner\n", old_corner).empty());
    CHECK(old_corner.motion.entry == EntryMode::corner);
    CHECK_FALSE(parse_globals("[motion]\nentry = sideways\n", old_corner).empty());
}

TEST_CASE("a device either follows Settings' movement or has all of its own") {
    Config config;
    config.motion.sensitivity = 2.0;
    config.motion.sensitivity_y = 3.0;
    config.motion.entry = EntryMode::corner;
    DeviceConfig device;
    CHECK_FALSE(has_own_motion(device.motion));

    device.motion = motion_patch_of(config.motion);
    CHECK(has_own_motion(device.motion));
    // The same values, now the device's: a change in Settings does not reach it.
    config.motion.sensitivity = 5.0;
    const Motion own = motion_of(config, device);
    CHECK(own.sensitivity == 2.0);
    CHECK(own.sensitivity_y == 3.0);
    CHECK(own.entry == EntryMode::corner);

    DeviceConfig back;
    REQUIRE(parse_device(format_device(device), back).empty());
    CHECK(has_own_motion(back.motion));
    CHECK(motion_of(config, back).sensitivity == 2.0);
}
