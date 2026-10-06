// SPDX-License-Identifier: MIT
//
// The layer-shell backend, for compositors without the InputCapture portal
// (Sway, Hyprland, Wayfire and other wlroots-based ones). A one-pixel
// transparent surface sits on each crossing span; when the pointer enters it
// the pointer is locked there, relative motion is read, and the surface takes
// the keyboard. See docs/PLATFORMS.md.
#include "capture.hpp"
#include "task_queue.hpp"

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
// The generated C header names a parameter "namespace".
#define namespace namespace_
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-output-unstable-v1-client-protocol.h"

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

// The listener tables below stop at the protocol version that is bound; the
// members a newer libwayland adds after that are meant to stay null.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

namespace aoas {
namespace {

// Scroll distance a wheel notch is reported as.
constexpr double axis_units_per_notch = 15.0;
// Stands in for the push the compositor does not report.
constexpr double pushed_through = 1e9;

class LayerShellCapture;

struct Output {
    LayerShellCapture* owner{};
    wl_output* output{};
    zxdg_output_v1* xdg{};
    Monitor monitor;
    int transform{};
};

struct Edge {
    LayerShellCapture* owner{};
    Barrier barrier;
    wl_surface* surface{};
    zwlr_layer_surface_v1* layer{};
    wl_buffer* buffer{};
    int width{1};
    int height{1};
};

class LayerShellCapture final : public Capture {
  public:
    ~LayerShellCapture() override {
        if (display_ == nullptr)
            return;
        end_grab();
        clear_edges();
        wl_display_flush(display_);
        wl_display_disconnect(display_);
    }

    const char* name() const noexcept override { return "layer_shell"; }
    bool sees_local_keys() const noexcept override { return false; }

    std::string start(CaptureHandler& handler, const CaptureEnv&) override {
        handler_ = &handler;
        display_ = wl_display_connect(nullptr);
        if (display_ == nullptr)
            return "no Wayland display";
        registry_ = wl_display_get_registry(display_);
        static const wl_registry_listener listener = {on_global, on_global_remove};
        wl_registry_add_listener(registry_, &listener, this);
        // Globals first, then the events of what was bound to them.
        if (wl_display_roundtrip(display_) < 0 || wl_display_roundtrip(display_) < 0)
            return "the Wayland connection failed";
        if (compositor_ == nullptr || shm_ == nullptr || seat_ == nullptr)
            return "the compositor lacks a core Wayland global";
        if (layer_shell_ == nullptr || constraints_ == nullptr || relative_manager_ == nullptr ||
            xdg_manager_ == nullptr)
            return "the compositor lacks wlr-layer-shell, pointer-constraints, relative-pointer "
                   "or xdg-output";
        for (const std::unique_ptr<Output>& output : outputs_)
            add_xdg_output(*output);
        if (wl_display_roundtrip(display_) < 0)
            return "the Wayland connection failed";
        if (pointer_ == nullptr || keyboard_ == nullptr)
            return "the seat has no pointer or keyboard";
        return {};
    }

    std::vector<Monitor> monitors() override {
        std::vector<Monitor> result;
        for (const std::unique_ptr<Output>& output : outputs_) {
            Monitor monitor = output->monitor;
            const bool turned = output->transform == WL_OUTPUT_TRANSFORM_90 ||
                                output->transform == WL_OUTPUT_TRANSFORM_270 ||
                                output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
                                output->transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
            if (turned)
                std::swap(monitor.width_mm, monitor.height_mm);
            result.push_back(std::move(monitor));
        }
        return result;
    }

    std::string set_barriers(const std::vector<Barrier>& barriers) override {
        if (grabbed_)
            return "the input is captured";
        clear_edges();
        for (const Barrier& barrier : barriers) {
            if (barrier.monitor >= outputs_.size())
                continue;
            const Output& output = *outputs_[barrier.monitor];
            const Rect& zone = output.monitor.rect;
            auto edge = std::make_unique<Edge>();
            edge->owner = this;
            edge->barrier = barrier;
            edge->surface = wl_compositor_create_surface(compositor_);
            edge->layer = zwlr_layer_shell_v1_get_layer_surface(
                layer_shell_, edge->surface, output.output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                "aoahid_share");
            uint32_t anchor = 0;
            const int length = barrier.span.length();
            if (vertical(barrier.side)) {
                edge->width = 1;
                edge->height = length;
                anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                         (barrier.side == Side::left ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
                                                     : ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
                zwlr_layer_surface_v1_set_margin(edge->layer, barrier.span.lo - zone.y, 0, 0, 0);
            } else {
                edge->width = length;
                edge->height = 1;
                anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                         (barrier.side == Side::top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                                                    : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
                zwlr_layer_surface_v1_set_margin(edge->layer, 0, 0, 0, barrier.span.lo - zone.x);
            }
            zwlr_layer_surface_v1_set_anchor(edge->layer, anchor);
            zwlr_layer_surface_v1_set_size(edge->layer, static_cast<uint32_t>(edge->width),
                                           static_cast<uint32_t>(edge->height));
            // -1: neither moved by a panel's reserved area nor reserving one.
            zwlr_layer_surface_v1_set_exclusive_zone(edge->layer, -1);
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                edge->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
            static const zwlr_layer_surface_v1_listener listener = {on_layer_configure,
                                                                    on_layer_closed};
            zwlr_layer_surface_v1_add_listener(edge->layer, &listener, edge.get());
            wl_surface_commit(edge->surface);
            edges_.push_back(std::move(edge));
        }
        wl_display_flush(display_);
        return {};
    }

    // The pointer can only be locked while it is on one of the surfaces.
    bool grab() override { return false; }

    void release(const int x, const int y) override {
        if (!grabbed_)
            return;
        Edge* edge = active_;
        const bool is_vertical = vertical(edge->barrier.side);
        const int along =
            std::clamp(is_vertical ? y : x, edge->barrier.span.lo, edge->barrier.span.hi) -
            edge->barrier.span.lo;
        if (locked_ != nullptr) {
            // Where the cursor reappears, in the surface's own coordinates.
            // The lock is on the edge surface, so it comes back on the edge.
            zwp_locked_pointer_v1_set_cursor_position_hint(
                locked_, wl_fixed_from_int(is_vertical ? 0 : along),
                wl_fixed_from_int(is_vertical ? along : 0));
            wl_surface_commit(edge->surface);
        }
        end_grab();
        wl_display_flush(display_);
    }

    void run() override {
        const int fd = wl_display_get_fd(display_);
        while (!stopping_.load(std::memory_order_acquire)) {
            while (wl_display_prepare_read(display_) != 0)
                wl_display_dispatch_pending(display_);
            wl_display_flush(display_);
            pollfd fds[2] = {{fd, POLLIN, 0}, {tasks_.fd(), POLLIN, 0}};
            const int ready = poll(fds, 2, -1);
            if (ready > 0 && (fds[0].revents & POLLIN) != 0)
                wl_display_read_events(display_);
            else
                wl_display_cancel_read(display_);
            if (wl_display_dispatch_pending(display_) < 0) {
                if (grabbed_) {
                    grabbed_ = false;
                    handler_->lost();
                }
                return;
            }
            if (ready > 0 && (fds[1].revents & POLLIN) != 0)
                tasks_.drain();
            if (layout_changed_) {
                layout_changed_ = false;
                handler_->monitors_changed();
            }
        }
    }

    void stop() override {
        stopping_.store(true, std::memory_order_release);
        tasks_.post([] {});
    }

    void post(std::function<void()> task) override { tasks_.post(std::move(task)); }

  private:
    static void on_global(void* data, wl_registry* registry, const uint32_t name,
                          const char* interface, const uint32_t version) {
        auto* self = static_cast<LayerShellCapture*>(data);
        const auto is = [interface](const wl_interface& wanted) {
            return std::strcmp(interface, wanted.name) == 0;
        };
        const auto bind = [&](const wl_interface& wanted, const uint32_t limit) {
            return wl_registry_bind(registry, name, &wanted, std::min(version, limit));
        };
        if (is(wl_compositor_interface)) {
            self->compositor_ = static_cast<wl_compositor*>(bind(wl_compositor_interface, 4));
        } else if (is(wl_shm_interface)) {
            self->shm_ = static_cast<wl_shm*>(bind(wl_shm_interface, 1));
        } else if (is(wl_seat_interface)) {
            if (self->seat_ == nullptr) {
                self->seat_ = static_cast<wl_seat*>(bind(wl_seat_interface, 5));
                static const wl_seat_listener listener = {on_seat_capabilities, on_seat_name};
                wl_seat_add_listener(self->seat_, &listener, self);
            }
        } else if (is(wl_output_interface)) {
            auto output = std::make_unique<Output>();
            output->owner = self;
            output->output = static_cast<wl_output*>(bind(wl_output_interface, 4));
            static const wl_output_listener listener = {on_output_geometry, on_output_mode,
                                                        on_output_done,     on_output_scale,
                                                        on_output_name,     on_output_description};
            wl_output_add_listener(output->output, &listener, output.get());
            if (self->xdg_manager_ != nullptr && self->started_)
                self->add_xdg_output(*output);
            self->outputs_.push_back(std::move(output));
            if (self->started_)
                self->layout_changed_ = true;
        } else if (is(zxdg_output_manager_v1_interface)) {
            self->xdg_manager_ =
                static_cast<zxdg_output_manager_v1*>(bind(zxdg_output_manager_v1_interface, 3));
        } else if (is(zwlr_layer_shell_v1_interface)) {
            self->layer_shell_ =
                static_cast<zwlr_layer_shell_v1*>(bind(zwlr_layer_shell_v1_interface, 4));
        } else if (is(zwp_pointer_constraints_v1_interface)) {
            self->constraints_ = static_cast<zwp_pointer_constraints_v1*>(
                bind(zwp_pointer_constraints_v1_interface, 1));
        } else if (is(zwp_relative_pointer_manager_v1_interface)) {
            self->relative_manager_ = static_cast<zwp_relative_pointer_manager_v1*>(
                bind(zwp_relative_pointer_manager_v1_interface, 1));
        } else if (is(zwp_keyboard_shortcuts_inhibit_manager_v1_interface)) {
            self->inhibit_manager_ = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
                bind(zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
        }
    }
    static void on_global_remove(void*, wl_registry*, uint32_t) {}

    static void on_seat_capabilities(void* data, wl_seat* seat, const uint32_t capabilities) {
        auto* self = static_cast<LayerShellCapture*>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && self->pointer_ == nullptr) {
            self->pointer_ = wl_seat_get_pointer(seat);
            static const wl_pointer_listener listener = {
                on_pointer_enter,       on_pointer_leave,     on_pointer_motion,
                on_pointer_button,      on_pointer_axis,      on_pointer_frame,
                on_pointer_axis_source, on_pointer_axis_stop, on_pointer_axis_discrete};
            wl_pointer_add_listener(self->pointer_, &listener, self);
        }
        if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && self->keyboard_ == nullptr) {
            self->keyboard_ = wl_seat_get_keyboard(seat);
            static const wl_keyboard_listener listener = {on_keymap,         on_keyboard_enter,
                                                          on_keyboard_leave, on_key,
                                                          on_modifiers,      on_repeat_info};
            wl_keyboard_add_listener(self->keyboard_, &listener, self);
        }
    }
    static void on_seat_name(void*, wl_seat*, const char*) {}

    static void on_output_geometry(void* data, wl_output*, int32_t, int32_t, const int32_t width_mm,
                                   const int32_t height_mm, int32_t, const char*, const char*,
                                   const int32_t transform) {
        auto* output = static_cast<Output*>(data);
        output->monitor.width_mm = width_mm;
        output->monitor.height_mm = height_mm;
        output->transform = transform;
    }
    static void on_output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
    static void on_output_done(void*, wl_output*) {}
    static void on_output_scale(void*, wl_output*, int32_t) {}
    static void on_output_name(void* data, wl_output*, const char* name) {
        static_cast<Output*>(data)->monitor.name = name;
    }
    static void on_output_description(void*, wl_output*, const char*) {}

    void add_xdg_output(Output& output) {
        if (output.xdg != nullptr)
            return;
        output.xdg = zxdg_output_manager_v1_get_xdg_output(xdg_manager_, output.output);
        static const zxdg_output_v1_listener listener = {on_xdg_position, on_xdg_size, on_xdg_done,
                                                         on_xdg_name, on_xdg_description};
        zxdg_output_v1_add_listener(output.xdg, &listener, &output);
        started_ = true;
    }
    static void on_xdg_position(void* data, zxdg_output_v1*, const int32_t x, const int32_t y) {
        auto* output = static_cast<Output*>(data);
        if (output->monitor.rect.x != x || output->monitor.rect.y != y)
            output->owner->layout_changed_ = true;
        output->monitor.rect.x = x;
        output->monitor.rect.y = y;
    }
    static void on_xdg_size(void* data, zxdg_output_v1*, const int32_t width,
                            const int32_t height) {
        auto* output = static_cast<Output*>(data);
        if (output->monitor.rect.w != width || output->monitor.rect.h != height)
            output->owner->layout_changed_ = true;
        output->monitor.rect.w = width;
        output->monitor.rect.h = height;
    }
    static void on_xdg_done(void*, zxdg_output_v1*) {}
    static void on_xdg_name(void* data, zxdg_output_v1*, const char* name) {
        auto* output = static_cast<Output*>(data);
        if (output->monitor.name.empty())
            output->monitor.name = name;
    }
    static void on_xdg_description(void*, zxdg_output_v1*, const char*) {}

    static void on_layer_configure(void* data, zwlr_layer_surface_v1* layer, const uint32_t serial,
                                   uint32_t, uint32_t) {
        auto* edge = static_cast<Edge*>(data);
        zwlr_layer_surface_v1_ack_configure(layer, serial);
        if (edge->buffer == nullptr)
            edge->buffer = edge->owner->transparent_buffer(edge->width, edge->height);
        if (edge->buffer != nullptr)
            wl_surface_attach(edge->surface, edge->buffer, 0, 0);
        wl_surface_commit(edge->surface);
    }
    static void on_layer_closed(void*, zwlr_layer_surface_v1*) {}

    // All-zero ARGB pixels: invisible, but still a target for the pointer.
    wl_buffer* transparent_buffer(const int width, const int height) {
        const int stride = width * 4;
        const int size = stride * height;
        const int fd = memfd_create("aoahid_share", MFD_CLOEXEC);
        if (fd < 0)
            return nullptr;
        if (ftruncate(fd, size) != 0) {
            close(fd);
            return nullptr;
        }
        wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, size);
        wl_buffer* buffer =
            wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        return buffer;
    }

    void clear_edges() {
        for (const std::unique_ptr<Edge>& edge : edges_) {
            if (edge->layer != nullptr)
                zwlr_layer_surface_v1_destroy(edge->layer);
            if (edge->surface != nullptr)
                wl_surface_destroy(edge->surface);
            if (edge->buffer != nullptr)
                wl_buffer_destroy(edge->buffer);
        }
        edges_.clear();
    }

    void end_grab() {
        if (locked_ != nullptr)
            zwp_locked_pointer_v1_destroy(locked_);
        if (relative_ != nullptr)
            zwp_relative_pointer_v1_destroy(relative_);
        if (inhibitor_ != nullptr)
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor_);
        locked_ = nullptr;
        relative_ = nullptr;
        inhibitor_ = nullptr;
        if (active_ != nullptr && active_->layer != nullptr) {
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                active_->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
            wl_surface_commit(active_->surface);
        }
        active_ = nullptr;
        grabbed_ = false;
        pointer_locked_ = false;
        keyboard_ours_ = false;
    }

    static void on_pointer_enter(void* data, wl_pointer* pointer, const uint32_t serial,
                                 wl_surface* surface, const wl_fixed_t x, const wl_fixed_t y) {
        auto* self = static_cast<LayerShellCapture*>(data);
        if (self->grabbed_)
            return;
        for (const std::unique_ptr<Edge>& edge : self->edges_) {
            if (edge->surface != surface)
                continue;
            const Barrier& barrier = edge->barrier;
            const int along = barrier.span.lo + wl_fixed_to_int(vertical(barrier.side) ? y : x);
            if (!self->handler_->edge_hit(barrier.id,
                                          std::clamp(along, barrier.span.lo, barrier.span.hi),
                                          pushed_through, self->buttons_down_ != 0))
                return;
            self->grabbed_ = true;
            self->active_ = edge.get();
            wl_pointer_set_cursor(pointer, serial, nullptr, 0, 0);
            self->locked_ = zwp_pointer_constraints_v1_lock_pointer(
                self->constraints_, surface, pointer, nullptr,
                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
            static const zwp_locked_pointer_v1_listener lock_listener = {on_locked, on_unlocked};
            zwp_locked_pointer_v1_add_listener(self->locked_, &lock_listener, self);
            self->relative_ = zwp_relative_pointer_manager_v1_get_relative_pointer(
                self->relative_manager_, pointer);
            static const zwp_relative_pointer_v1_listener listener = {on_relative_motion};
            zwp_relative_pointer_v1_add_listener(self->relative_, &listener, self);
            zwlr_layer_surface_v1_set_keyboard_interactivity(
                edge->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
            if (self->inhibit_manager_ != nullptr)
                self->inhibitor_ = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
                    self->inhibit_manager_, surface, self->seat_);
            wl_surface_commit(surface);
            return;
        }
    }
    // Nothing is sent to the phone until the compositor confirms that the
    // pointer is locked and the keyboard is on the surface; otherwise the
    // same input would also be acting on the desktop.
    static void on_locked(void* data, zwp_locked_pointer_v1*) {
        static_cast<LayerShellCapture*>(data)->pointer_locked_ = true;
    }
    static void on_unlocked(void* data, zwp_locked_pointer_v1*) {
        auto* self = static_cast<LayerShellCapture*>(data);
        if (!self->grabbed_)
            return;
        self->end_grab();
        self->handler_->lost();
    }
    static void on_pointer_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
        // The pointer left before the lock took hold: the compositor did not
        // grant it.
        auto* self = static_cast<LayerShellCapture*>(data);
        if (!self->grabbed_ || self->pointer_locked_)
            return;
        self->end_grab();
        self->handler_->lost();
    }
    static void on_pointer_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void on_pointer_button(void* data, wl_pointer*, uint32_t, uint32_t, const uint32_t code,
                                  const uint32_t state) {
        auto* self = static_cast<LayerShellCapture*>(data);
        const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
        self->buttons_down_ += down ? 1 : (self->buttons_down_ > 0 ? -1 : 0);
        unsigned button = 0;
        if (self->grabbed_ && self->pointer_locked_ && evdev_to_button(code, button))
            self->handler_->button(button, down);
    }
    static void on_pointer_axis(void* data, wl_pointer*, uint32_t, const uint32_t axis,
                                const wl_fixed_t value) {
        auto* self = static_cast<LayerShellCapture*>(data);
        // Down and right are positive here; a HID wheel counts up as positive.
        const double notches = wl_fixed_to_double(value) / axis_units_per_notch;
        if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
            self->wheel_ -= notches;
        else
            self->pan_ += notches;
    }
    static void on_pointer_frame(void* data, wl_pointer*) {
        auto* self = static_cast<LayerShellCapture*>(data);
        if (self->grabbed_ && self->pointer_locked_ && (self->wheel_ != 0.0 || self->pan_ != 0.0))
            self->handler_->scroll(self->wheel_, self->pan_);
        self->wheel_ = self->pan_ = 0.0;
    }
    static void on_pointer_axis_source(void*, wl_pointer*, uint32_t) {}
    static void on_pointer_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void on_pointer_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

    static void on_relative_motion(void* data, zwp_relative_pointer_v1*, uint32_t, uint32_t,
                                   wl_fixed_t, wl_fixed_t, const wl_fixed_t dx_unaccel,
                                   const wl_fixed_t dy_unaccel) {
        auto* self = static_cast<LayerShellCapture*>(data);
        if (self->grabbed_ && self->pointer_locked_)
            self->handler_->motion(wl_fixed_to_double(dx_unaccel), wl_fixed_to_double(dy_unaccel));
    }

    static void on_keymap(void*, wl_keyboard*, uint32_t, const int32_t fd, uint32_t) { close(fd); }
    static void on_keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface* surface,
                                  wl_array*) {
        auto* self = static_cast<LayerShellCapture*>(data);
        self->keyboard_ours_ = self->active_ != nullptr && self->active_->surface == surface;
    }
    static void on_keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
        // The compositor took the keyboard away while it was ours.
        auto* self = static_cast<LayerShellCapture*>(data);
        const bool was_ours = self->keyboard_ours_;
        self->keyboard_ours_ = false;
        if (!self->grabbed_ || self->leaving_ || !was_ours)
            return;
        self->end_grab();
        self->handler_->lost();
    }
    static void on_key(void* data, wl_keyboard*, uint32_t, uint32_t, const uint32_t code,
                       const uint32_t state) {
        auto* self = static_cast<LayerShellCapture*>(data);
        HidKey key;
        if (self->grabbed_ && self->keyboard_ours_ && evdev_to_hid(code, key)) {
            self->leaving_ = true;
            self->handler_->key(key, state == WL_KEYBOARD_KEY_STATE_PRESSED, true);
            self->leaving_ = false;
        }
    }
    static void on_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t) {}
    static void on_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}

    CaptureHandler* handler_{};
    wl_display* display_{};
    wl_registry* registry_{};
    wl_compositor* compositor_{};
    wl_shm* shm_{};
    wl_seat* seat_{};
    wl_pointer* pointer_{};
    wl_keyboard* keyboard_{};
    zxdg_output_manager_v1* xdg_manager_{};
    zwlr_layer_shell_v1* layer_shell_{};
    zwp_pointer_constraints_v1* constraints_{};
    zwp_relative_pointer_manager_v1* relative_manager_{};
    zwp_keyboard_shortcuts_inhibit_manager_v1* inhibit_manager_{};

    std::vector<std::unique_ptr<Output>> outputs_;
    std::vector<std::unique_ptr<Edge>> edges_;
    bool started_{};
    bool layout_changed_{};

    bool grabbed_{};
    bool leaving_{};
    bool pointer_locked_{};
    bool keyboard_ours_{};
    Edge* active_{};
    zwp_locked_pointer_v1* locked_{};
    zwp_relative_pointer_v1* relative_{};
    zwp_keyboard_shortcuts_inhibitor_v1* inhibitor_{};
    int buttons_down_{};
    double wheel_{};
    double pan_{};

    TaskQueue tasks_;
    std::atomic<bool> stopping_{};
};

} // namespace

std::unique_ptr<Capture> make_layer_shell_capture() {
    return std::make_unique<LayerShellCapture>();
}

} // namespace aoas
