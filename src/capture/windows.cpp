// SPDX-License-Identifier: MIT
//
// The Windows backend: low-level mouse and keyboard hooks watch the screen
// edge and, while the input is on the phone, swallow every event. With the
// cursor parked, the position a swallowed move would have reached gives the
// movement. See docs/PLATFORMS.md.
#include "capture.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace aoas {
namespace {

constexpr UINT task_message = WM_APP + 1;

// A PS/2 set-1 scan code to the Linux key code the key map takes. The plain
// codes are the same numbers; the E0-prefixed ones are listed.
unsigned linux_code(const DWORD scan, const bool extended, const DWORD vk) noexcept {
    // Pause shares scan code 0x45 with Num Lock.
    if (vk == VK_PAUSE)
        return 119;
    if (!extended) {
        switch (scan) {
        case 0x70:
            return 93; // Katakana/Hiragana
        case 0x73:
            return 89; // Ro
        case 0x79:
            return 92; // Henkan
        case 0x7B:
            return 94; // Muhenkan
        case 0x7D:
            return 124; // Yen
        default:
            return scan <= 0x58 ? scan : 0;
        }
    }
    switch (scan) {
    case 0x1C:
        return 96; // keypad Enter
    case 0x1D:
        return 97; // right Ctrl
    case 0x35:
        return 98; // keypad /
    case 0x37:
        return 99; // Print Screen
    case 0x38:
        return 100; // right Alt
    case 0x45:
        return 69; // Num Lock
    case 0x47:
        return 102; // Home
    case 0x48:
        return 103; // Up
    case 0x49:
        return 104; // Page Up
    case 0x4B:
        return 105; // Left
    case 0x4D:
        return 106; // Right
    case 0x4F:
        return 107; // End
    case 0x50:
        return 108; // Down
    case 0x51:
        return 109; // Page Down
    case 0x52:
        return 110; // Insert
    case 0x53:
        return 111; // Delete
    case 0x5B:
        return 125; // left Windows
    case 0x5C:
        return 126; // right Windows
    case 0x5D:
        return 127; // Menu
    case 0x20:
        return 113; // Mute
    case 0x2E:
        return 114; // Volume Down
    case 0x30:
        return 115; // Volume Up
    case 0x19:
        return 163; // Next Track
    case 0x22:
        return 164; // Play/Pause
    case 0x10:
        return 165; // Previous Track
    case 0x24:
        return 166; // Stop
    default:
        return 0;
    }
}

class WindowsCapture final : public Capture {
  public:
    ~WindowsCapture() override {
        if (mouse_hook_ != nullptr)
            UnhookWindowsHookEx(mouse_hook_);
        if (keyboard_hook_ != nullptr)
            UnhookWindowsHookEx(keyboard_hook_);
        if (window_ != nullptr)
            DestroyWindow(window_);
        instance_ = nullptr;
    }

    const char* name() const noexcept override { return "windows"; }
    bool sees_local_keys() const noexcept override { return true; }

    std::string start(CaptureHandler& handler, const CaptureEnv&) override {
        handler_ = &handler;
        instance_ = this;
        thread_ = GetCurrentThreadId();
        // Coordinates in real pixels on every monitor.
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

        // A hidden window, only to hear about display changes.
        WNDCLASSA window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandleA(nullptr);
        window_class.lpszClassName = "aoahid_share";
        RegisterClassA(&window_class);
        window_ = CreateWindowExA(0, "aoahid_share", "aoahid_share", WS_OVERLAPPED, 0, 0, 0, 0,
                                  nullptr, nullptr, window_class.hInstance, nullptr);

        // The hooks are called on this thread, from its message loop.
        mouse_hook_ = SetWindowsHookExA(WH_MOUSE_LL, mouse_proc, window_class.hInstance, 0);
        keyboard_hook_ =
            SetWindowsHookExA(WH_KEYBOARD_LL, keyboard_proc, window_class.hInstance, 0);
        if (mouse_hook_ == nullptr || keyboard_hook_ == nullptr)
            return "the input hooks could not be installed";
        // Make the thread's message queue exist before anyone posts to it.
        MSG message;
        PeekMessageA(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        return {};
    }

    std::vector<Monitor> monitors() override {
        std::vector<Monitor> result;
        EnumDisplayMonitors(nullptr, nullptr, monitor_proc, reinterpret_cast<LPARAM>(&result));
        zones_.clear();
        for (const Monitor& monitor : result)
            zones_.push_back(monitor.rect);
        return result;
    }

    std::string set_barriers(const std::vector<Barrier>& barriers) override {
        barriers_ = barriers;
        return {};
    }

    bool grab() override {
        if (!grabbed_) {
            GetCursorPos(&parked_);
            grabbed_ = true;
        }
        return true;
    }

    void release(const int x, const int y) override {
        if (!grabbed_)
            return;
        grabbed_ = false;
        SetCursorPos(x, y);
    }

    void run() override {
        MSG message;
        while (GetMessageA(&message, nullptr, 0, 0) > 0) {
            if (message.message == task_message && message.hwnd == nullptr) {
                std::vector<std::function<void()>> tasks;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    tasks.swap(tasks_);
                }
                for (const std::function<void()>& task : tasks)
                    task();
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }

    void stop() override { PostThreadMessageA(thread_, WM_QUIT, 0, 0); }

    void post(std::function<void()> task) override {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        PostThreadMessageA(thread_, task_message, 0, 0);
    }

  private:
    static BOOL CALLBACK monitor_proc(HMONITOR handle, HDC, LPRECT, const LPARAM data) {
        auto* list = reinterpret_cast<std::vector<Monitor>*>(data);
        MONITORINFOEXA info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoA(handle, &info))
            return TRUE;
        Monitor monitor;
        monitor.name = info.szDevice;
        monitor.rect = {info.rcMonitor.left, info.rcMonitor.top,
                        info.rcMonitor.right - info.rcMonitor.left,
                        info.rcMonitor.bottom - info.rcMonitor.top};
        // What the display driver reports; zero when it does not know.
        if (const HDC dc = CreateDCA(info.szDevice, nullptr, nullptr, nullptr)) {
            monitor.width_mm = GetDeviceCaps(dc, HORZSIZE);
            monitor.height_mm = GetDeviceCaps(dc, VERTSIZE);
            DeleteDC(dc);
        }
        list->push_back(std::move(monitor));
        return TRUE;
    }

    static LRESULT CALLBACK window_proc(const HWND window, const UINT message, const WPARAM wparam,
                                        const LPARAM lparam) {
        if (message == WM_DISPLAYCHANGE && instance_ != nullptr)
            instance_->handler_->monitors_changed();
        return DefWindowProcA(window, message, wparam, lparam);
    }

    static bool any_button_down() noexcept {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
               (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    }

    // True when the event was taken and must not reach Windows.
    bool on_mouse(const WPARAM message, const MSLLHOOKSTRUCT& info) {
        // What another program injected (this one's own SetCursorPos
        // included) is never sent to the phone. While the input is on the
        // phone it is swallowed too, so the parked cursor stays put.
        if ((info.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) != 0)
            return grabbed_;
        if (!grabbed_) {
            if (message != WM_MOUSEMOVE)
                return false;
            // info.pt is where the cursor would go, before Windows keeps it
            // on the desktop; past the edge means a push against it.
            for (const Barrier& barrier : barriers_) {
                if (barrier.monitor >= zones_.size())
                    continue;
                const Rect& zone = zones_[barrier.monitor];
                int push = 0;
                int along = 0;
                switch (barrier.side) {
                case Side::left:
                    push = zone.x - info.pt.x + 1;
                    along = info.pt.y;
                    break;
                case Side::right:
                    push = info.pt.x - (zone.x + zone.w - 1) + 1;
                    along = info.pt.y;
                    break;
                case Side::top:
                    push = zone.y - info.pt.y + 1;
                    along = info.pt.x;
                    break;
                case Side::bottom:
                    push = info.pt.y - (zone.y + zone.h - 1) + 1;
                    along = info.pt.x;
                    break;
                }
                if (push <= 0 || along < barrier.span.lo || along > barrier.span.hi)
                    continue;
                if (!handler_->edge_hit(barrier.id, along, push, any_button_down()))
                    continue;
                // Park the cursor two pixels in, clear of the taskbar and
                // other things that react to the very edge.
                POINT park{info.pt.x, info.pt.y};
                park.x = std::clamp<LONG>(park.x, zone.x + 2, zone.x + zone.w - 3);
                park.y = std::clamp<LONG>(park.y, zone.y + 2, zone.y + zone.h - 3);
                SetCursorPos(park.x, park.y);
                parked_ = park;
                grabbed_ = true;
                return true;
            }
            return false;
        }

        switch (message) {
        case WM_MOUSEMOVE: {
            // The move is swallowed, so the cursor stays parked and info.pt
            // is the parked position plus this movement.
            const int dx = info.pt.x - parked_.x;
            const int dy = info.pt.y - parked_.y;
            if (dx != 0 || dy != 0)
                handler_->motion(dx, dy);
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            handler_->button(1, message == WM_LBUTTONDOWN);
            break;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            handler_->button(2, message == WM_RBUTTONDOWN);
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            handler_->button(3, message == WM_MBUTTONDOWN);
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            handler_->button(HIWORD(info.mouseData) == XBUTTON1 ? 4U : 5U,
                             message == WM_XBUTTONDOWN);
            break;
        case WM_MOUSEWHEEL:
            handler_->scroll(static_cast<short>(HIWORD(info.mouseData)) / double{WHEEL_DELTA}, 0.0);
            break;
        case WM_MOUSEHWHEEL:
            handler_->scroll(0.0, static_cast<short>(HIWORD(info.mouseData)) / double{WHEEL_DELTA});
            break;
        default:
            break;
        }
        return true;
    }

    bool on_key(const WPARAM message, const KBDLLHOOKSTRUCT& info) {
        // Leave alone what another program injected.
        if ((info.flags & LLKHF_INJECTED) != 0)
            return false;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool was_grabbed = grabbed_;
        HidKey key;
        const unsigned code =
            linux_code(info.scanCode, (info.flags & LLKHF_EXTENDED) != 0, info.vkCode);
        if (code != 0 && evdev_to_hid(code, key))
            handler_->key(key, down, was_grabbed);
        return was_grabbed;
    }

    static LRESULT CALLBACK mouse_proc(const int code, const WPARAM wparam, const LPARAM lparam) {
        if (code == HC_ACTION && instance_ != nullptr &&
            instance_->on_mouse(wparam, *reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam)))
            return 1;
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    static LRESULT CALLBACK keyboard_proc(const int code, const WPARAM wparam,
                                          const LPARAM lparam) {
        if (code == HC_ACTION && instance_ != nullptr &&
            instance_->on_key(wparam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam)))
            return 1;
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    static inline WindowsCapture* instance_ = nullptr;

    CaptureHandler* handler_{};
    DWORD thread_{};
    HWND window_{};
    HHOOK mouse_hook_{};
    HHOOK keyboard_hook_{};
    std::vector<Rect> zones_;
    std::vector<Barrier> barriers_;
    bool grabbed_{};
    POINT parked_{};
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

} // namespace

std::unique_ptr<Capture> make_windows_capture() { return std::make_unique<WindowsCapture>(); }

} // namespace aoas
