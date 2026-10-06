// SPDX-License-Identifier: MIT
//
// The X11 backend: XFixes pointer barriers tell when the cursor pushes
// against the edge, core grabs take the keyboard and mouse, and XInput 2 raw
// events deliver the unaccelerated movement. See docs/PLATFORMS.md.
#include "capture.hpp"
#include "task_queue.hpp"

#include <poll.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

namespace aoas {
namespace {

double fp3232(const xcb_input_fp3232_t value) noexcept {
    return value.integral + value.frac / 4294967296.0;
}

double fp1616(const xcb_input_fp1616_t value) noexcept { return value / 65536.0; }

// X button numbers to HID ones. 4-7 are the wheel and are handled apart.
unsigned hid_button(const uint32_t detail) noexcept {
    switch (detail) {
    case 1:
        return 1;
    case 2:
        return 3;
    case 3:
        return 2;
    default:
        return detail >= 8 && detail <= 11 ? detail - 4 : 0;
    }
}

class X11Capture final : public Capture {
  public:
    ~X11Capture() override {
        if (connection_ == nullptr)
            return;
        for (const xcb_xfixes_barrier_t id : barrier_ids_)
            xcb_xfixes_delete_pointer_barrier(connection_, id);
        if (grabbed_) {
            xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
            xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME);
        }
        xcb_flush(connection_);
        xcb_disconnect(connection_);
    }

    const char* name() const noexcept override { return "x11"; }
    // Raw key events arrive whether or not this client holds a grab.
    bool sees_local_keys() const noexcept override { return true; }

    std::string start(CaptureHandler& handler, const CaptureEnv&) override {
        handler_ = &handler;
        int screen_number = 0;
        connection_ = xcb_connect(nullptr, &screen_number);
        if (xcb_connection_has_error(connection_) != 0)
            return "no X display";
        xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection_));
        for (int index = 0; index < screen_number; ++index)
            xcb_screen_next(&screens);
        root_ = screens.data->root;

        const xcb_query_extension_reply_t* xi = xcb_get_extension_data(connection_, &xcb_input_id);
        const xcb_query_extension_reply_t* randr =
            xcb_get_extension_data(connection_, &xcb_randr_id);
        const xcb_query_extension_reply_t* fixes =
            xcb_get_extension_data(connection_, &xcb_xfixes_id);
        if (xi == nullptr || xi->present == 0 || randr == nullptr || randr->present == 0 ||
            fixes == nullptr || fixes->present == 0)
            return "the X server lacks XInput, RandR or XFixes";
        xi_opcode_ = xi->major_opcode;
        randr_event_ = randr->first_event;

        // Barrier events need XInput 2.3 and XFixes 5, and both have to be
        // announced before use.
        xcb_input_xi_query_version_reply_t* xi_version = xcb_input_xi_query_version_reply(
            connection_, xcb_input_xi_query_version(connection_, 2, 3), nullptr);
        const bool xi_ok = xi_version != nullptr &&
                           (xi_version->major_version > 2 ||
                            (xi_version->major_version == 2 && xi_version->minor_version >= 3));
        std::free(xi_version);
        xcb_xfixes_query_version_reply_t* fixes_version = xcb_xfixes_query_version_reply(
            connection_, xcb_xfixes_query_version(connection_, 5, 0), nullptr);
        const bool fixes_ok = fixes_version != nullptr && fixes_version->major_version >= 5;
        std::free(fixes_version);
        if (!xi_ok || !fixes_ok)
            return "the X server is too old (needs XInput 2.3 and XFixes 5)";

        struct {
            xcb_input_event_mask_t head;
            uint32_t mask;
        } selection{};
        selection.head.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
        selection.head.mask_len = 1;
        selection.mask =
            XCB_INPUT_XI_EVENT_MASK_RAW_MOTION | XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS |
            XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS |
            XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE | XCB_INPUT_XI_EVENT_MASK_BARRIER_HIT;
        xcb_input_xi_select_events(connection_, root_, 1, &selection.head);
        xcb_randr_select_input(connection_, root_, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);

        // A fully transparent cursor for while the input is on the phone.
        const xcb_pixmap_t pixmap = xcb_generate_id(connection_);
        xcb_create_pixmap(connection_, 1, pixmap, root_, 1, 1);
        blank_cursor_ = xcb_generate_id(connection_);
        xcb_create_cursor(connection_, blank_cursor_, pixmap, pixmap, 0, 0, 0, 0, 0, 0, 0, 0);
        xcb_free_pixmap(connection_, pixmap);

        // The grabs go on a window of their own. The server leaves raw
        // events out for a client whose grab is on the root window, and the
        // raw events are how the movement is read while grabbed.
        grab_window_ = xcb_generate_id(connection_);
        const uint32_t override_redirect = 1;
        xcb_create_window(connection_, 0, grab_window_, root_, -10, -10, 1, 1, 0,
                          XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                          XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
        xcb_map_window(connection_, grab_window_);
        xcb_flush(connection_);
        return xcb_connection_has_error(connection_) != 0 ? "the X connection failed"
                                                          : std::string();
    }

    std::vector<Monitor> monitors() override {
        std::vector<Monitor> result;
        xcb_randr_get_monitors_reply_t* reply = xcb_randr_get_monitors_reply(
            connection_, xcb_randr_get_monitors(connection_, root_, 1), nullptr);
        if (reply == nullptr)
            return result;
        for (xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(reply);
             it.rem != 0; xcb_randr_monitor_info_next(&it)) {
            Monitor monitor;
            monitor.rect = {it.data->x, it.data->y, it.data->width, it.data->height};
            monitor.width_mm = it.data->width_in_millimeters;
            monitor.height_mm = it.data->height_in_millimeters;
            xcb_get_atom_name_reply_t* name = xcb_get_atom_name_reply(
                connection_, xcb_get_atom_name(connection_, it.data->name), nullptr);
            if (name != nullptr) {
                monitor.name.assign(xcb_get_atom_name_name(name),
                                    static_cast<size_t>(xcb_get_atom_name_name_length(name)));
                std::free(name);
            }
            result.push_back(std::move(monitor));
        }
        std::free(reply);
        zones_.clear();
        for (const Monitor& monitor : result)
            zones_.push_back(monitor.rect);
        return result;
    }

    std::string set_barriers(const std::vector<Barrier>& barriers) override {
        for (const xcb_xfixes_barrier_t id : barrier_ids_)
            xcb_xfixes_delete_pointer_barrier(connection_, id);
        barrier_ids_.clear();
        barriers_ = barriers;
        for (const Barrier& barrier : barriers) {
            if (barrier.monitor >= zones_.size()) {
                // Keeps barrier_ids_ in step with barriers_.
                barrier_ids_.push_back(XCB_NONE);
                continue;
            }
            const Rect& zone = zones_[barrier.monitor];
            // One pixel in from the edge: the cursor then never reaches the
            // last row or column, where the desktop's own edge features
            // listen. Movement back into the screen stays free.
            int x1 = barrier.span.lo;
            int y1 = barrier.span.lo;
            int x2 = barrier.span.hi + 1;
            int y2 = barrier.span.hi + 1;
            uint32_t directions = 0;
            switch (barrier.side) {
            case Side::left:
                x1 = x2 = zone.x + 1;
                directions = XCB_XFIXES_BARRIER_DIRECTIONS_POSITIVE_X;
                break;
            case Side::right:
                x1 = x2 = zone.x + zone.w - 1;
                directions = XCB_XFIXES_BARRIER_DIRECTIONS_NEGATIVE_X;
                break;
            case Side::top:
                y1 = y2 = zone.y + 1;
                directions = XCB_XFIXES_BARRIER_DIRECTIONS_POSITIVE_Y;
                break;
            case Side::bottom:
                y1 = y2 = zone.y + zone.h - 1;
                directions = XCB_XFIXES_BARRIER_DIRECTIONS_NEGATIVE_Y;
                break;
            }
            const xcb_xfixes_barrier_t id = xcb_generate_id(connection_);
            xcb_xfixes_create_pointer_barrier(connection_, id, root_, static_cast<uint16_t>(x1),
                                              static_cast<uint16_t>(y1), static_cast<uint16_t>(x2),
                                              static_cast<uint16_t>(y2), directions, 0, nullptr);
            barrier_ids_.push_back(id);
        }
        xcb_flush(connection_);
        return {};
    }

    bool grab() override {
        if (grabbed_)
            return true;
        // The cursor is held where it is: the grab confines it to a window of
        // one pixel there, so that while the input is on the phone it does not
        // wander over the screen's edges and hot corners. The raw events still
        // report every movement of the mouse.
        xcb_query_pointer_reply_t* where =
            xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr);
        if (where == nullptr)
            return false;
        const uint32_t position[2] = {static_cast<uint32_t>(where->root_x),
                                      static_cast<uint32_t>(where->root_y)};
        xcb_configure_window(connection_, grab_window_, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             position);
        std::free(where);
        xcb_grab_pointer_reply_t* pointer = xcb_grab_pointer_reply(
            connection_,
            xcb_grab_pointer(connection_, 0, grab_window_, 0, XCB_GRAB_MODE_ASYNC,
                             XCB_GRAB_MODE_ASYNC, grab_window_, blank_cursor_, XCB_CURRENT_TIME),
            nullptr);
        const bool pointer_ok = pointer != nullptr && pointer->status == XCB_GRAB_STATUS_SUCCESS;
        std::free(pointer);
        if (!pointer_ok)
            return false;
        xcb_grab_keyboard_reply_t* keyboard = xcb_grab_keyboard_reply(
            connection_,
            xcb_grab_keyboard(connection_, 0, grab_window_, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
                              XCB_GRAB_MODE_ASYNC),
            nullptr);
        const bool keyboard_ok = keyboard != nullptr && keyboard->status == XCB_GRAB_STATUS_SUCCESS;
        std::free(keyboard);
        if (!keyboard_ok) {
            // Never leave the mouse taken while the keyboard is not.
            xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
            xcb_flush(connection_);
            return false;
        }
        grabbed_ = true;
        return true;
    }

    void release(const int x, const int y) override {
        if (!grabbed_)
            return;
        grabbed_ = false;
        xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME);
        // Out of the screen again, so no pixel of it keeps taking clicks.
        const uint32_t away[2] = {static_cast<uint32_t>(-10), static_cast<uint32_t>(-10)};
        xcb_configure_window(connection_, grab_window_, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
                             away);
        xcb_warp_pointer(connection_, XCB_NONE, root_, 0, 0, 0, 0, static_cast<int16_t>(x),
                         static_cast<int16_t>(y));
        xcb_flush(connection_);
    }

    void run() override {
        const int fd = xcb_get_file_descriptor(connection_);
        while (!stopping_.load(std::memory_order_acquire)) {
            while (xcb_generic_event_t* event = xcb_poll_for_event(connection_)) {
                dispatch(event);
                std::free(event);
            }
            if (xcb_connection_has_error(connection_) != 0) {
                if (grabbed_) {
                    grabbed_ = false;
                    handler_->lost();
                }
                return;
            }
            xcb_flush(connection_);
            pollfd fds[2] = {{fd, POLLIN, 0}, {tasks_.fd(), POLLIN, 0}};
            if (poll(fds, 2, -1) > 0 && (fds[1].revents & POLLIN) != 0)
                tasks_.drain();
        }
    }

    void stop() override {
        stopping_.store(true, std::memory_order_release);
        tasks_.post([] {});
    }

    void post(std::function<void()> task) override { tasks_.post(std::move(task)); }

  private:
    bool any_button_down() {
        xcb_query_pointer_reply_t* reply =
            xcb_query_pointer_reply(connection_, xcb_query_pointer(connection_, root_), nullptr);
        if (reply == nullptr)
            return false;
        const bool down =
            (reply->mask & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3)) != 0;
        std::free(reply);
        return down;
    }

    void dispatch(xcb_generic_event_t* event) {
        const uint8_t type = event->response_type & 0x7FU;
        if (type == randr_event_ + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
            handler_->monitors_changed();
            return;
        }
        if (type != XCB_GE_GENERIC)
            return;
        const auto* generic = reinterpret_cast<xcb_ge_generic_event_t*>(event);
        if (generic->extension != xi_opcode_)
            return;

        switch (generic->event_type) {
        case XCB_INPUT_BARRIER_HIT: {
            const auto* hit = reinterpret_cast<xcb_input_barrier_hit_event_t*>(event);
            if (grabbed_)
                break;
            for (size_t index = 0; index < barrier_ids_.size(); ++index) {
                if (barrier_ids_[index] != hit->barrier)
                    continue;
                const Barrier& barrier = barriers_[index];
                const double dx = fp3232(hit->dx);
                const double dy = fp3232(hit->dy);
                double push = 0.0;
                switch (barrier.side) {
                case Side::left:
                    push = -dx;
                    break;
                case Side::right:
                    push = dx;
                    break;
                case Side::top:
                    push = -dy;
                    break;
                case Side::bottom:
                    push = dy;
                    break;
                }
                if (push <= 0.0)
                    break;
                const int along = static_cast<int>(std::lround(
                    vertical(barrier.side) ? fp1616(hit->root_y) : fp1616(hit->root_x)));
                // Take the input first: if a menu or another client holds a
                // grab, the cursor has to stay on the PC.
                if (!grab())
                    break;
                if (!handler_->edge_hit(barrier.id, along, push, any_button_down())) {
                    grabbed_ = false;
                    xcb_ungrab_pointer(connection_, XCB_CURRENT_TIME);
                    xcb_ungrab_keyboard(connection_, XCB_CURRENT_TIME);
                    xcb_flush(connection_);
                }
                break;
            }
            break;
        }
        case XCB_INPUT_RAW_MOTION: {
            if (!grabbed_)
                break;
            auto* motion = reinterpret_cast<xcb_input_raw_motion_event_t*>(event);
            const uint32_t* mask = xcb_input_raw_button_press_valuator_mask(motion);
            const xcb_input_fp3232_t* values = xcb_input_raw_button_press_axisvalues_raw(motion);
            const int count = xcb_input_raw_button_press_axisvalues_raw_length(motion);
            double dx = 0.0;
            double dy = 0.0;
            int next = 0;
            // The values follow the set bits of the mask; axes 0 and 1 are
            // the unaccelerated X and Y.
            for (int axis = 0; axis < motion->valuators_len * 32 && next < count; ++axis) {
                if ((mask[axis / 32] & (1U << (axis % 32))) == 0)
                    continue;
                if (axis == 0)
                    dx = fp3232(values[next]);
                else if (axis == 1)
                    dy = fp3232(values[next]);
                ++next;
            }
            if (dx != 0.0 || dy != 0.0)
                handler_->motion(dx, dy);
            break;
        }
        case XCB_INPUT_RAW_BUTTON_PRESS:
        case XCB_INPUT_RAW_BUTTON_RELEASE: {
            if (!grabbed_)
                break;
            const auto* button = reinterpret_cast<xcb_input_raw_button_press_event_t*>(event);
            const bool down = generic->event_type == XCB_INPUT_RAW_BUTTON_PRESS;
            // Buttons 4 to 7 are the wheel, one press per notch.
            if (button->detail >= 4 && button->detail <= 7) {
                if (down) {
                    const double wheel = button->detail == 4 ? 1.0 : button->detail == 5 ? -1.0 : 0;
                    const double pan = button->detail == 7 ? 1.0 : button->detail == 6 ? -1.0 : 0;
                    handler_->scroll(wheel, pan);
                }
            } else if (const unsigned number = hid_button(button->detail)) {
                handler_->button(number, down);
            }
            break;
        }
        case XCB_INPUT_RAW_KEY_PRESS:
        case XCB_INPUT_RAW_KEY_RELEASE: {
            const auto* key = reinterpret_cast<xcb_input_raw_key_press_event_t*>(event);
            HidKey hid;
            // X key codes are the kernel's plus 8.
            if (key->detail >= 8 && evdev_to_hid(key->detail - 8, hid))
                handler_->key(hid, generic->event_type == XCB_INPUT_RAW_KEY_PRESS, grabbed_);
            break;
        }
        default:
            break;
        }
    }

    CaptureHandler* handler_{};
    xcb_connection_t* connection_{};
    xcb_window_t root_{};
    xcb_window_t grab_window_{};
    xcb_cursor_t blank_cursor_{};
    uint8_t xi_opcode_{};
    uint8_t randr_event_{};
    TaskQueue tasks_;
    std::atomic<bool> stopping_{};

    std::vector<Rect> zones_;
    std::vector<Barrier> barriers_;
    std::vector<xcb_xfixes_barrier_t> barrier_ids_;
    bool grabbed_{};
};

} // namespace

std::unique_ptr<Capture> make_x11_capture() { return std::make_unique<X11Capture>(); }

} // namespace aoas
