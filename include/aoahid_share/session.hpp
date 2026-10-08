// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/accel_model.hpp"
#include "aoahid_share/geometry.hpp"
#include "aoahid_share/position_cell.hpp"
#include "aoahid_share/tracker.hpp"

#include <bitset>
#include <cstdint>
#include <vector>

namespace aoas {

// One wheel notch as a swipe of the second finger, in the touchscreen's raw
// coordinates.
struct SwipePlan {
    // Where the finger is put down, and how far it goes from there.
    int x{};
    int y{};
    int dx{};
    int dy{};
    // The touchscreen's raw size: the finger stays inside it.
    Size area{};
    // The moves the distance is divided into, and the times (seconds) from
    // the finger being put down to its first move, from the first move to the
    // last, and from the last move to the lift.
    int steps{1};
    double start_s{};
    double total_s{};
    double release_s{};
    // When the swipe before it is still going: false adds the new distance to
    // what is left and goes on with the same finger; true lifts that finger
    // and starts again where the cursor is.
    bool restart{};
};

// Receives what has to reach Android. Deltas are already rotated for the
// mount, so an implementation only forwards them.
class Sink {
  public:
    virtual ~Sink() = default;
    // May be added to other pending moves and sent as their sum.
    virtual void move(Delta counts) = 0;
    // Sent as a movement of its own, after what was queued before it and
    // before anything queued after it. The corner reference depends on
    // Android clamping this one before the next movement is applied, so it
    // must never be summed with another.
    virtual void jump(Delta counts) = 0;
    virtual void scroll(int wheel, int pan) = 0;
    // One-based button number.
    virtual void button(unsigned button, bool down) = 0;
    // Keyboard page usage, modifiers included (0xE0-0xE7).
    virtual void key(uint16_t usage, bool down) = 0;
    // Consumer page usage.
    virtual void media(uint16_t usage, bool down) = 0;
    // A touchscreen contact in the device's raw coordinates (0 and 1 are the
    // two contacts); down == false lifts it, down == true places or moves it.
    virtual void touch(unsigned contact, int x, int y, bool down) = 0;
    // The second finger's swipe for one wheel notch.
    virtual void swipe(const SwipePlan& plan) = 0;
};

struct MotionConfig {
    // PC delta to counts: the sensitivity.
    double scale_x{1.0};
    double scale_y{1.0};
    double scroll_scale{1.0};
    // Take the corner reference on every entry instead of only when needed.
    bool always_corner{};
    // Leave the cursor in Android's bottom right corner while the input is
    // on the PC, and enter from there.
    bool park{};
    // Estimated-position width above which the next entry takes the corner.
    double resync_width{48.0};
    // Outward PC pixels the cursor has to push before it crosses.
    double enter_push{};
    // Pixels Android's cursor has to be pushed past its edge to come back;
    // negative selects 1 with acceleration off and 16 otherwise.
    double return_push{-1.0};
    bool no_cross_while_button{true};
    unsigned buttons{5};
};

// Touchscreen mode: the geometry the conversion needs, and the settings.
struct TouchSetup {
    bool enabled{};
    // The device's natural size and Android's display rotation (the mount is
    // the session's own).
    Size natural{};
    int rotation{};
    // The mouse button that becomes a tap.
    unsigned button{1};
    // Pixels a notch moves the finger on each axis; 0 leaves that wheel alone.
    int scroll{};
    int scroll_pan{};
    double release_s{0.2};
    // From a swipe's finger being put down to its first move.
    double start_s{0.001};
    // The moves a notch is divided into, and the time from the first to the
    // last of them.
    int steps{4};
    double total_s{0.008};
    // A notch that comes while a swipe goes on lifts it and starts again.
    bool restart{};
};

// Another device beside this one: where on this display's edge the cursor
// leaves for it, and the caller's number for that device.
struct Neighbour {
    // The edge of this display it sits at.
    Side edge{Side::right};
    // The part of that edge it touches, in this display's pixels.
    Span span{};
    int target{};
};

// The PC-side/Android-side state machine and the position arithmetic of
// docs/MATH.md. It owns no thread and calls no OS or USB function.
class Session {
  public:
    explicit Session(Sink& sink) noexcept : sink_(sink) {}

    // Sets the layout. Forgets the cursor position, so the next entry takes
    // the corner reference. Not valid while remote().
    // Without `has_pc` the display touches no monitor (it sits beside
    // another device) and the portal is not used.
    void configure(const Portal& portal, Size view, int mount, const MotionConfig& motion,
                   const AccelModel& accel, std::vector<Neighbour> neighbours = {},
                   bool has_pc = true);

    // Sets touchscreen mode. Called after configure(). A contact that is down
    // is lifted first: the old setup's positions mean nothing in the new one.
    void set_touch(const TouchSetup& setup);

    // Where the tracker's range is published after every change, for a thread
    // that may not touch the session. Null: nowhere.
    void set_position_cell(PositionCell* cell) noexcept {
        cell_ = cell;
        publish();
    }

    [[nodiscard]] bool remote() const noexcept { return remote_; }
    [[nodiscard]] const Tracker& tracker() const noexcept { return tracker_; }
    [[nodiscard]] const Portal& portal() const noexcept { return portal_; }

    // PC side: the cursor pushed `push` pixels outward at edge coordinate t.
    // True when the caller has to grab input; the entry reports are sent.
    bool edge_hit(int t, double push, bool button_down, double now);
    // PC side: cross now, as edge_hit() would at t (hotkey or command).
    void enter(int t, double now);
    // From a neighbour: the cursor comes in through `edge` of this display,
    // at `along` pixels from that edge's start.
    void enter_at(Side edge, double along, double now);

    // Android side. motion() returns true when the cursor leaves this
    // display. exit() then says where to: to_pc, or a neighbour's target, in
    // which case along() gives where on that neighbour's edge of this
    // display. leave() gives the PC edge coordinate to put the cursor at.
    static constexpr int to_pc = -1;
    bool motion(double dx, double dy, double now);
    [[nodiscard]] int exit() const noexcept { return exit_; }
    [[nodiscard]] double along(Side edge) const noexcept;
    // The edge of this display that faces the PC.
    [[nodiscard]] Side pc_edge() const noexcept { return opposite(portal_.side); }
    void scroll(double wheel, double pan);
    void button(unsigned button, bool down);
    void key(uint16_t usage, bool down);
    void media(uint16_t usage, bool down);
    // Releases everything still pressed on Android and returns to the PC side.
    int leave();

    // The next entry takes the corner reference.
    void resync() noexcept { known_ = false; }

  private:
    GainRange send(Delta view_counts, double now, bool alone = false);
    void publish() const noexcept {
        if (cell_ != nullptr)
            cell_->store(tracker_);
    }
    // Counts that reach any edge from anywhere, whatever the gain.
    [[nodiscard]] int far_counts() const noexcept;
    void park();
    [[nodiscard]] Interval along_range(Side edge) const noexcept;
    // Distance of the cursor's far bound from that edge.
    [[nodiscard]] double distance(Side edge) const noexcept;
    [[nodiscard]] double return_threshold() const noexcept;
    [[nodiscard]] TouchPoint raw_at(double view_x, double view_y) const noexcept;
    void tap(bool down);
    void follow_tap();
    void swipe(double dx, double dy);
    void lift_touch();
    // Sends the movement kept back while the finger was down, as one report.
    void flush_held();

    Sink& sink_;
    Portal portal_{};
    Size view_{};
    int mount_{};
    MotionConfig motion_{};
    AccelModel accel_{};
    Tracker tracker_{};
    PositionCell* cell_{};

    bool remote_{};
    bool known_{};
    double entry_push_{};
    double last_hit_{};
    // Certainly-clamped push toward the PC (first) and toward each
    // neighbour, in their order.
    std::vector<double> pushes_{};
    // Scratch for motion(): each way out's distance before the report.
    std::vector<double> before_{};
    std::vector<Neighbour> neighbours_{};
    bool has_pc_{true};
    int exit_{to_pc};
    double last_send_{};
    double fraction_x_{};
    double fraction_y_{};
    double fraction_wheel_{};
    double fraction_pan_{};

    TouchSetup touch_{};
    bool tap_down_{};
    TouchPoint tap_at_{};
    // While the finger is down no mouse report goes out (Android drops the
    // touch when the mouse reports): the movement is summed here, with where
    // the tracker stood when the finger went down and the time of the last
    // step, and sent when the finger is lifted.
    Delta held_{};
    Tracker held_from_{};
    double held_at_{};
    bool scroll_active_{};
    TouchPoint scroll_at_{};

    std::bitset<256> keys_{};
    uint32_t buttons_{};
    uint16_t media_{};
};

} // namespace aoas
