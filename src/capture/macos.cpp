// SPDX-License-Identifier: MIT
//
// The macOS backend: one event tap watches the screen edge and, while the
// input is on the phone, drops every event and reads the movement from the
// events' own deltas, with the cursor detached and hidden. The tap needs the
// Accessibility permission. See docs/PLATFORMS.md.
#include "capture.hpp"

#include <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <bitset>
#include <cmath>
#include <mutex>

namespace aoas {
namespace {

// A macOS virtual key code to its Keyboard-page usage; 0 where there is none.
// The codes name key positions, as HID usages do.
constexpr uint8_t usages[128] = {
    0x04, 0x16, 0x07, 0x09, 0x0B, 0x0A, 0x1D, 0x1B, // A S D F H G Z X
    0x06, 0x19, 0x64, 0x05, 0x14, 0x1A, 0x08, 0x15, // C V ISO B Q W E R
    0x1C, 0x17, 0x1E, 0x1F, 0x20, 0x21, 0x23, 0x22, // Y T 1 2 3 4 6 5
    0x2E, 0x26, 0x24, 0x2D, 0x25, 0x27, 0x30, 0x12, // = 9 7 - 8 0 ] O
    0x18, 0x2F, 0x0C, 0x13, 0x28, 0x0F, 0x0D, 0x34, // U [ I P Return L J '
    0x0E, 0x33, 0x31, 0x36, 0x38, 0x11, 0x10, 0x37, // K ; Backslash , / N M .
    0x2B, 0x2C, 0x35, 0x2A, 0x00, 0x29, 0xE7, 0xE3, // Tab Space ` Backspace - Esc RCmd Cmd
    0xE1, 0x39, 0xE2, 0xE0, 0xE5, 0xE6, 0xE4, 0x00, // Shift Caps Opt Ctrl RShift ROpt RCtrl -
    0x6C, 0x63, 0x00, 0x55, 0x00, 0x57, 0x00, 0x53, // F17 KP. - KP* - KP+ - Clear
    0x00, 0x00, 0x00, 0x54, 0x58, 0x00, 0x56, 0x6D, // - - - KP/ KPEnter - KP- F18
    0x6E, 0x67, 0x62, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, // F19 KP= KP0 KP1 KP2 KP3 KP4 KP5
    0x5E, 0x5F, 0x6F, 0x60, 0x61, 0x89, 0x87, 0x85, // KP6 KP7 F20 KP8 KP9 Yen Ro KP,
    0x3E, 0x3F, 0x40, 0x3C, 0x41, 0x42, 0x91, 0x44, // F5 F6 F7 F3 F8 F9 Eisu F11
    0x90, 0x68, 0x6B, 0x69, 0x00, 0x43, 0x00, 0x45, // Kana F13 F16 F14 - F10 - F12
    0x00, 0x6A, 0x49, 0x4A, 0x4B, 0x4C, 0x3D, 0x4D, // - F15 Help Home PgUp Del F4 End
    0x3B, 0x4E, 0x3A, 0x50, 0x4F, 0x51, 0x52, 0x00, // F2 PgDn F1 Left Right Down Up -
};

class MacCapture final : public Capture {
  public:
    ~MacCapture() override {
        if (grabbed_)
            end_grab();
        if (tap_ != nullptr) {
            CGEventTapEnable(tap_, false);
            CFRelease(tap_);
        }
        if (tap_source_ != nullptr)
            CFRelease(tap_source_);
        if (task_source_ != nullptr)
            CFRelease(task_source_);
    }

    const char* name() const noexcept override { return "macos"; }
    bool sees_local_keys() const noexcept override { return true; }

    std::string start(CaptureHandler& handler, const CaptureEnv&) override {
        handler_ = &handler;
        const CGEventMask mask =
            CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged) |
            CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged) |
            CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
            CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kCGEventKeyDown) |
            CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
        tap_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                                mask, on_event, this);
        if (tap_ == nullptr)
            return "the event tap could not be created; allow this program under System "
                   "Settings > Privacy & Security > Accessibility";
        loop_ = CFRunLoopGetCurrent();
        tap_source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap_, 0);
        CFRunLoopAddSource(loop_, tap_source_, kCFRunLoopCommonModes);

        CFRunLoopSourceContext context{};
        context.info = this;
        context.perform = on_tasks;
        task_source_ = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
        CFRunLoopAddSource(loop_, task_source_, kCFRunLoopCommonModes);
        CGDisplayRegisterReconfigurationCallback(on_display_change, this);
        return {};
    }

    std::vector<Monitor> monitors() override {
        std::vector<Monitor> result;
        CGDirectDisplayID ids[16];
        uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess)
            return result;
        zones_.clear();
        for (uint32_t index = 0; index < count; ++index) {
            const CGRect bounds = CGDisplayBounds(ids[index]);
            const CGSize size = CGDisplayScreenSize(ids[index]);
            Monitor monitor;
            monitor.name = "display-" + std::to_string(ids[index]);
            monitor.rect = {static_cast<int>(std::lround(bounds.origin.x)),
                            static_cast<int>(std::lround(bounds.origin.y)),
                            static_cast<int>(std::lround(bounds.size.width)),
                            static_cast<int>(std::lround(bounds.size.height))};
            monitor.width_mm = size.width;
            monitor.height_mm = size.height;
            zones_.push_back(monitor.rect);
            result.push_back(std::move(monitor));
        }
        return result;
    }

    std::string set_barriers(const std::vector<Barrier>& barriers) override {
        barriers_ = barriers;
        return {};
    }

    bool grab() override {
        if (!grabbed_) {
            grabbed_ = true;
            // Movement no longer moves the cursor; the events still carry it.
            CGAssociateMouseAndMouseCursorPosition(false);
            CGDisplayHideCursor(kCGDirectMainDisplay);
        }
        return true;
    }

    void release(const int x, const int y) override {
        if (!grabbed_)
            return;
        CGWarpMouseCursorPosition(CGPointMake(x, y));
        end_grab();
    }

    void run() override { CFRunLoopRun(); }

    void stop() override {
        post([this] { CFRunLoopStop(loop_); });
    }

    void post(std::function<void()> task) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        CFRunLoopSourceSignal(task_source_);
        CFRunLoopWakeUp(loop_);
    }

  private:
    void end_grab() {
        grabbed_ = false;
        CGAssociateMouseAndMouseCursorPosition(true);
        CGDisplayShowCursor(kCGDirectMainDisplay);
    }

    static void on_tasks(void* info) {
        auto* self = static_cast<MacCapture*>(info);
        std::vector<std::function<void()>> tasks;
        {
            const std::lock_guard<std::mutex> lock(self->mutex_);
            tasks.swap(self->tasks_);
        }
        for (const std::function<void()>& task : tasks)
            task();
    }

    static void on_display_change(CGDirectDisplayID, const CGDisplayChangeSummaryFlags flags,
                                  void* info) {
        // Called before and after a change; only the second one has the
        // new layout.
        if ((flags & kCGDisplayBeginConfigurationFlag) == 0)
            static_cast<MacCapture*>(info)->handler_->monitors_changed();
    }

    static CGEventRef on_event(CGEventTapProxy, const CGEventType type, const CGEventRef event,
                               void* info) {
        return static_cast<MacCapture*>(info)->handle(type, event);
    }

    // Returning null drops the event.
    CGEventRef handle(const CGEventType type, const CGEventRef event) {
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            // The system switches a slow tap off; it has to be switched back.
            CGEventTapEnable(tap_, true);
            return event;
        }
        // Only what the hardware produced is sent to the phone. An event
        // another program posted goes on to the system untouched.
        // This is a filter against accidents and ordinary automation, not a
        // security boundary: a program allowed to post input to the session
        // can dress its events up as hardware ones (docs/PLATFORMS.md).
        if (CGEventGetIntegerValueField(event, kCGEventSourceStateID) !=
                kCGEventSourceStateHIDSystemState ||
            CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) != 0)
            return event;
        const bool was_grabbed = grabbed_;
        switch (type) {
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged: {
            const double dx = CGEventGetDoubleValueField(event, kCGMouseEventDeltaX);
            const double dy = CGEventGetDoubleValueField(event, kCGMouseEventDeltaY);
            if (grabbed_) {
                if (dx != 0.0 || dy != 0.0)
                    handler_->motion(dx, dy);
            } else if (check_edge(CGEventGetLocation(event), dx, dy, type != kCGEventMouseMoved)) {
                return nullptr;
            }
            break;
        }
        case kCGEventLeftMouseDown:
        case kCGEventLeftMouseUp:
            if (grabbed_)
                handler_->button(1, type == kCGEventLeftMouseDown);
            break;
        case kCGEventRightMouseDown:
        case kCGEventRightMouseUp:
            if (grabbed_)
                handler_->button(2, type == kCGEventRightMouseDown);
            break;
        case kCGEventOtherMouseDown:
        case kCGEventOtherMouseUp:
            if (grabbed_) {
                // Button 2 is the middle one; the rest follow in order.
                const int64_t number =
                    CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
                handler_->button(static_cast<unsigned>(number + 1), type == kCGEventOtherMouseDown);
            }
            break;
        case kCGEventScrollWheel:
            if (grabbed_) {
                // In lines; up and left are positive.
                handler_->scroll(CGEventGetDoubleValueField(event, kCGScrollWheelEventDeltaAxis1),
                                 -CGEventGetDoubleValueField(event, kCGScrollWheelEventDeltaAxis2));
            }
            break;
        case kCGEventKeyDown:
        case kCGEventKeyUp:
        case kCGEventFlagsChanged: {
            const int64_t code = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
            if (code < 0 || code >= 128 || usages[code] == 0)
                break;
            bool down = type == kCGEventKeyDown;
            if (type == kCGEventFlagsChanged) {
                // A modifier reports only that it changed; each change of a
                // key is a press, then a release.
                down = !modifiers_[static_cast<size_t>(code)];
                modifiers_[static_cast<size_t>(code)] = down;
            }
            handler_->key({usages[code], false}, down, was_grabbed);
            break;
        }
        default:
            break;
        }
        return was_grabbed ? nullptr : event;
    }

    // True when the cursor pushed against a barrier and the input was taken.
    bool check_edge(const CGPoint at, const double dx, const double dy, const bool dragging) {
        for (const Barrier& barrier : barriers_) {
            if (barrier.monitor >= zones_.size())
                continue;
            const Rect& zone = zones_[barrier.monitor];
            double push = 0.0;
            double along = 0.0;
            bool at_edge = false;
            // The cursor stops on the last pixel; movement that still points
            // outward there is the push.
            switch (barrier.side) {
            case Side::left:
                at_edge = at.x <= zone.x;
                push = -dx;
                along = at.y;
                break;
            case Side::right:
                at_edge = at.x >= zone.x + zone.w - 1;
                push = dx;
                along = at.y;
                break;
            case Side::top:
                at_edge = at.y <= zone.y;
                push = -dy;
                along = at.x;
                break;
            case Side::bottom:
                at_edge = at.y >= zone.y + zone.h - 1;
                push = dy;
                along = at.x;
                break;
            }
            const int t = static_cast<int>(std::lround(along));
            if (!at_edge || push <= 0.0 || t < barrier.span.lo || t > barrier.span.hi)
                continue;
            if (!handler_->edge_hit(barrier.id, t, push, dragging))
                continue;
            // Two pixels in, clear of the Dock and hot corners, then detach.
            CGPoint park = at;
            park.x = std::clamp(park.x, zone.x + 2.0, zone.x + zone.w - 3.0);
            park.y = std::clamp(park.y, zone.y + 2.0, zone.y + zone.h - 3.0);
            CGWarpMouseCursorPosition(park);
            static_cast<void>(grab());
            return true;
        }
        return false;
    }

    CaptureHandler* handler_{};
    CFMachPortRef tap_{};
    CFRunLoopSourceRef tap_source_{};
    CFRunLoopSourceRef task_source_{};
    CFRunLoopRef loop_{};
    std::vector<Rect> zones_;
    std::vector<Barrier> barriers_;
    bool grabbed_{};
    std::bitset<128> modifiers_{};
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

} // namespace

std::unique_ptr<Capture> make_macos_capture() { return std::make_unique<MacCapture>(); }

} // namespace aoas
