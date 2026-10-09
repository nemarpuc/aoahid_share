// SPDX-License-Identifier: MIT
#include "aoahid_share/session.hpp"

#include <algorithm>
#include <cmath>

namespace aoas {
namespace {

// Entry reports leave back to back; this stands in for the time between them
// when estimating the gain of the second one.
constexpr double report_gap_seconds = 0.002;

// Separate pushes against the PC edge further apart than this do not add up.
constexpr double push_window_seconds = 0.3;

// Extra pixels on the corner move, so rounding never leaves it short.
constexpr int corner_slack = 64;

// Unit vector pointing out of the Android display through one of its edges.
Delta toward(const Side edge) noexcept {
    switch (edge) {
    case Side::left:
        return {-1, 0};
    case Side::right:
        return {1, 0};
    case Side::top:
        return {0, -1};
    case Side::bottom:
        return {0, 1};
    }
    return {};
}

double take_whole(double value, double& fraction) noexcept {
    value += fraction;
    const double whole = std::trunc(value);
    fraction = value - whole;
    return whole;
}

} // namespace

void Session::configure(const Portal& portal, const Size view, const int mount,
                        const MotionConfig& motion, const AccelModel& accel,
                        std::vector<Neighbour> neighbours, const bool has_pc) {
    // A contact that is down keeps the raw position it was placed at; the new
    // layout may turn the display, so it is lifted before anything changes.
    lift_touch();
    portal_ = portal;
    view_ = view;
    mount_ = mount;
    motion_ = motion;
    accel_ = accel;
    neighbours_ = std::move(neighbours);
    has_pc_ = has_pc;
    pushes_.assign(neighbours_.size() + 1, 0.0);
    tracker_.reset(view);
    publish();
    known_ = false;
    entry_push_ = 0.0;
}

// The cursor's range along an edge: y for the left and right edges.
Interval Session::along_range(const Side edge) const noexcept {
    return vertical(edge) ? tracker_.y() : tracker_.x();
}

double Session::along(const Side edge) const noexcept { return along_range(edge).mid(); }

double Session::distance(const Side edge) const noexcept {
    switch (edge) {
    case Side::left:
        return tracker_.x().hi;
    case Side::right:
        return view_.w - 1.0 - tracker_.x().lo;
    case Side::top:
        return tracker_.y().hi;
    case Side::bottom:
        return view_.h - 1.0 - tracker_.y().lo;
    }
    return 0.0;
}

double Session::return_threshold() const noexcept {
    if (motion_.return_push >= 0.0)
        return motion_.return_push;
    return accel_.mode() == AccelMode::exact ? 1.0 : 16.0;
}

GainRange Session::send(const Delta counts, const double now, const bool alone) {
    const double seconds = last_send_ > 0.0 ? now - last_send_ : 0.0;
    const GainRange gain = accel_.range(std::hypot(counts.x, counts.y), seconds);
    if (counts.x == 0 && counts.y == 0)
        return gain;
    tracker_.sent(counts, gain);
    publish();
    if (alone)
        sink_.jump(to_device_delta(mount_, counts));
    else
        sink_.move(to_device_delta(mount_, counts));
    last_send_ = now;
    return gain;
}

bool Session::edge_hit(const int t, const double push, const bool button_down, const double now) {
    if (remote_ || !has_pc_)
        return false;
    if ((motion_.no_cross_while_button && button_down) || t < portal_.segment.lo ||
        t > portal_.segment.hi) {
        entry_push_ = 0.0;
        return false;
    }
    if (now - last_hit_ > push_window_seconds)
        entry_push_ = 0.0;
    last_hit_ = now;
    entry_push_ += push;
    if (entry_push_ < motion_.enter_push)
        return false;
    enter(t, now);
    return true;
}

void Session::enter(const int t, const double now) {
    enter_at(pc_edge(), portal_.to_android(t), now);
}

void Session::enter_at(const Side edge, const double along, const double now) {
    if (remote_)
        return;
    remote_ = true;
    entry_push_ = 0.0;
    std::fill(pushes_.begin(), pushes_.end(), 0.0);
    fraction_x_ = fraction_y_ = fraction_wheel_ = fraction_pan_ = 0.0;
    last_send_ = 0.0;

    const bool is_vertical = vertical(edge);
    const Delta out = toward(edge);
    const double last = (is_vertical ? view_.h : view_.w) - 1.0;
    const double target = std::clamp(along, 0.0, last);

    double start = along_range(edge).mid();
    double when = now;
    const int far = far_counts();
    // From the parked corner the entry is one report, which needs a gain
    // that does not depend on the report's length. Otherwise the cursor has
    // to be on the entry edge already.
    const bool placed = motion_.park ? accel_.mode() == AccelMode::exact : distance(edge) == 0.0;
    const bool usable = known_ && !motion_.always_corner && placed &&
                        along_range(edge).width() <= motion_.resync_width;
    if (!usable) {
        // Corner reference: push far past the nearer corner of the entry edge
        // so Android's clamp leaves the cursor exactly there.
        tracker_.reset(view_);
        publish();
        const int sign = target < last / 2.0 ? -1 : 1;
        send(is_vertical ? Delta{out.x * far, sign * far} : Delta{sign * far, out.y * far}, when,
             true);
        when += report_gap_seconds;
        start = sign < 0 ? 0.0 : last;
        known_ = true;
    }

    const double travel = target - start;
    const GainRange gain = accel_.range(std::fabs(travel), report_gap_seconds);
    const int counts = static_cast<int>(std::lround(travel / ((gain.lo + gain.hi) / 2.0)));
    // Away from the entry edge (parked), the same report also pushes far
    // past it, and Android's clamp puts the cursor exactly on it.
    const bool across = distance(edge) != 0.0;
    const Delta push = across ? Delta{out.x * far, out.y * far} : Delta{};
    send(is_vertical ? Delta{push.x, counts} : Delta{counts, push.y}, when, across);
}

int Session::far_counts() const noexcept {
    return static_cast<int>(
        std::ceil((std::max(view_.w, view_.h) + corner_slack) / accel_.floor()));
}

void Session::park() {
    // The bottom right corner in Android's own axes, however the device is
    // mounted: the arrow's tip is its top left, so nearly all of it is off
    // the display there.
    const int far = far_counts();
    Delta view{far, far};
    for (const Delta candidate :
         {Delta{far, far}, Delta{far, -far}, Delta{-far, far}, Delta{-far, -far}}) {
        const Delta device = to_device_delta(mount_, candidate);
        if (device.x > 0 && device.y > 0)
            view = candidate;
    }
    tracker_.reset(view_);
    publish();
    send(view, last_send_, true);
    known_ = true;
}

bool Session::motion(const double dx, const double dy, const double now) {
    if (!remote_)
        return false;
    const Delta counts{static_cast<int>(take_whole(dx * motion_.scale_x, fraction_x_)),
                       static_cast<int>(take_whole(dy * motion_.scale_y, fraction_y_))};
    if (counts.x == 0 && counts.y == 0)
        return false;

    // Each way out is an edge: the one facing the PC first, then one per
    // neighbour. Distances are taken before the report moves the cursor.
    const auto edge_of = [&](const size_t way) {
        return way == 0 ? pc_edge() : neighbours_[way - 1].edge;
    };
    const size_t ways = pushes_.size();
    before_.resize(ways);
    for (size_t way = 0; way < ways; ++way)
        before_[way] = distance(edge_of(way));
    GainRange gain;
    if (tap_down_) {
        // Only the cursor's estimated place moves; the report waits for the lift.
        const double seconds = last_send_ > 0.0 ? now - last_send_ : 0.0;
        gain = accel_.range(std::hypot(counts.x, counts.y), seconds);
        tracker_.sent(counts, gain);
        publish();
        held_ = {held_.x + counts.x, held_.y + counts.y};
        held_at_ = now;
        last_send_ = now;
    } else {
        gain = send(counts, now);
    }
    follow_tap();

    if (locked_) {
        // Nothing pushed while locked counts once it is unlocked.
        std::fill(pushes_.begin(), pushes_.end(), 0.0);
        return false;
    }
    const double threshold = return_threshold();
    for (size_t way = 0; way < ways; ++way) {
        if (way == 0 && !has_pc_)
            continue;
        const Side edge = edge_of(way);
        const Delta out = toward(edge);
        const int outward = out.x * counts.x + out.y * counts.y;
        if (outward > 0) {
            // The part of this move Android certainly clamped at that edge.
            const double past = gain.lo * outward - before_[way];
            if (past > 0.0)
                pushes_[way] += past;
        } else if (outward < 0) {
            pushes_[way] = 0.0;
        }
        if (pushes_[way] < threshold)
            continue;
        if (way == 0) {
            exit_ = to_pc;
            return true;
        }
        // A neighbour only touches part of the edge.
        const Neighbour& neighbour = neighbours_[way - 1];
        const double at = along(edge);
        if (at >= neighbour.span.lo && at <= neighbour.span.hi) {
            exit_ = neighbour.target;
            return true;
        }
    }
    return false;
}

TouchPoint Session::raw_at(const double view_x, const double view_y) const noexcept {
    return to_touch_point(touch_.natural, touch_.rotation, mount_, view_x, view_y);
}

void Session::set_touch(const TouchSetup& setup) {
    lift_touch();
    touch_ = setup;
}

void Session::tap(const bool down) {
    if (down == tap_down_)
        return;
    tap_down_ = down;
    tap_at_ = raw_at(tracker_.x().mid(), tracker_.y().mid());
    if (down) {
        held_ = {};
        held_from_ = tracker_;
    }
    sink_.touch(0, tap_at_.x, tap_at_.y, down);
    if (!down)
        flush_held();
}

void Session::flush_held() {
    if (held_.x == 0 && held_.y == 0)
        return;
    // The steps were estimated one by one; Android scales the report as one
    // movement, so the tracker is set back and told about that report.
    tracker_ = held_from_;
    const Delta counts = held_;
    held_ = {};
    send(counts, held_at_);
}

void Session::follow_tap() {
    if (!tap_down_)
        return;
    tap_at_ = raw_at(tracker_.x().mid(), tracker_.y().mid());
    sink_.touch(0, tap_at_.x, tap_at_.y, true);
}

void Session::swipe(const double dx, const double dy) {
    // Every notch is a swipe of its own: put down where the cursor is, moved
    // by the notch and lifted. The finger stays on the display: a notch that
    // would go past the edge is cut short, and one that cannot move at all is
    // not started (a finger put down and lifted in place is a tap).
    const double origin_x = tracker_.x().mid();
    const double origin_y = tracker_.y().mid();
    const double off_x = std::clamp(dx, -origin_x, view_.w - 1.0 - origin_x);
    const double off_y = std::clamp(dy, -origin_y, view_.h - 1.0 - origin_y);
    if (off_x == 0.0 && off_y == 0.0)
        return;
    const TouchPoint from = raw_at(origin_x, origin_y);
    const TouchPoint to = raw_at(origin_x + off_x, origin_y + off_y);
    sink_.swipe({from.x, from.y, to.x - from.x, to.y - from.y, touch_.natural, touch_.steps,
                 touch_.start_s, touch_.total_s, touch_.release_s, touch_.restart});
    scroll_active_ = true;
    scroll_at_ = to;
}

void Session::lift_touch() {
    if (tap_down_) {
        sink_.touch(0, tap_at_.x, tap_at_.y, false);
        tap_down_ = false;
        flush_held();
    }
    if (scroll_active_) {
        sink_.touch(1, scroll_at_.x, scroll_at_.y, false);
        scroll_active_ = false;
    }
}

void Session::scroll(const double wheel, const double pan) {
    if (!remote_)
        return;
    const int w = static_cast<int>(take_whole(wheel * motion_.scroll_scale, fraction_wheel_));
    const int p = static_cast<int>(take_whole(pan * motion_.scroll_scale, fraction_pan_));
    if (!touch_.enabled) {
        if (w != 0 || p != 0)
            sink_.scroll(w, p);
        return;
    }
    // The notches touch scroll takes; the rest stay a mouse wheel. A positive
    // wheel scrolls the view up, so the content and the finger go down; a
    // positive pan scrolls right, so the finger goes left. Both signs are
    // confirmed on a device in Task 7.
    int wheel_out = w;
    int pan_out = p;
    double dy = 0.0;
    double dx = 0.0;
    if (w != 0 && touch_.swipe && touch_.scroll != 0) {
        dy = static_cast<double>(w) * touch_.scroll;
        wheel_out = 0;
    }
    if (p != 0 && touch_.swipe && touch_.scroll_pan != 0) {
        dx = -static_cast<double>(p) * touch_.scroll_pan;
        pan_out = 0;
    }
    if (wheel_out != 0 || pan_out != 0)
        sink_.scroll(wheel_out, pan_out);
    if (wheel_out != w || pan_out != p)
        swipe(dx, dy);
}

void Session::button(const unsigned button, const bool down) {
    if (!remote_ || button < 1 || button > 32)
        return;
    // The tap button is the host's choice and need not be one the mouse Node
    // declares.
    if (touch_.enabled && button == touch_.button) {
        tap(down);
        return;
    }
    if (button > motion_.buttons)
        return;
    const uint32_t bit = uint32_t{1} << (button - 1);
    if (((buttons_ & bit) != 0) == down)
        return;
    buttons_ ^= bit;
    sink_.button(button, down);
}

void Session::key(const uint16_t usage, const bool down) {
    if (!remote_ || usage >= keys_.size() || keys_[usage] == down)
        return;
    keys_[usage] = down;
    sink_.key(usage, down);
}

void Session::media(const uint16_t usage, const bool down) {
    if (!remote_ || usage == 0)
        return;
    if (down) {
        if (media_ == usage)
            return;
        // The toggle profile holds one usage at a time.
        if (media_ != 0)
            sink_.media(media_, false);
        media_ = usage;
        sink_.media(usage, true);
    } else if (media_ == usage) {
        media_ = 0;
        sink_.media(usage, false);
    }
}

int Session::leave() {
    lift_touch();
    for (size_t usage = 0; usage < keys_.size(); ++usage) {
        if (keys_[usage])
            sink_.key(static_cast<uint16_t>(usage), false);
    }
    keys_.reset();
    for (unsigned button = 1; button <= 32; ++button) {
        if ((buttons_ & (uint32_t{1} << (button - 1))) != 0)
            sink_.button(button, false);
    }
    buttons_ = 0;
    if (media_ != 0)
        sink_.media(media_, false);
    media_ = 0;
    const int back = has_pc_ ? portal_.to_pc(along(pc_edge())) : 0;
    if (remote_ && motion_.park)
        park();
    locked_ = false;
    remote_ = false;
    return back;
}

} // namespace aoas
