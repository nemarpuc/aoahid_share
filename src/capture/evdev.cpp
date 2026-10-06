// SPDX-License-Identifier: MIT
//
// The evdev backend: reads the kernel's input devices directly and takes
// them with EVIOCGRAB. It works under any compositor and on the console, but
// it cannot see the cursor, so the input only moves by hotkey or command.
// It needs read access to /dev/input/event*. See docs/PLATFORMS.md.
#include "capture.hpp"
#include "task_queue.hpp"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cerrno>
#include <filesystem>

namespace aoas {
namespace {

// How often new devices (a mouse plugged in later) are looked for.
constexpr int rescan_ms = 3000;

bool has_bit(const unsigned long* bits, const unsigned index) noexcept {
    constexpr unsigned word = 8 * sizeof(unsigned long);
    return ((bits[index / word] >> (index % word)) & 1UL) != 0;
}

class EvdevCapture final : public Capture {
  public:
    ~EvdevCapture() override {
        for (const Device& device : devices_) {
            if (physically_grabbed_)
                ioctl(device.fd, EVIOCGRAB, 0);
            close(device.fd);
        }
    }

    const char* name() const noexcept override { return "evdev"; }
    bool sees_local_keys() const noexcept override { return true; }

    std::string start(CaptureHandler& handler, const CaptureEnv&) override {
        handler_ = &handler;
        scan();
        if (devices_.empty())
            return "no keyboard or mouse under /dev/input could be opened; the user needs "
                   "read access to /dev/input/event* (docs/PLATFORMS.md)";
        return {};
    }

    // There is no display to ask. One nominal monitor lets a layout resolve;
    // its edge is never watched.
    std::vector<Monitor> monitors() override {
        return {{"evdev", {0, 0, 1920, 1080}, 508.0, 286.0}};
    }

    std::string set_barriers(const std::vector<Barrier>&) override { return {}; }

    bool grab() override {
        grabbed_ = true;
        grab_when_idle();
        return true;
    }

    void release(int, int) override {
        grabbed_ = false;
        if (physically_grabbed_) {
            for (const Device& device : devices_)
                ioctl(device.fd, EVIOCGRAB, 0);
            physically_grabbed_ = false;
        }
    }

    void run() override {
        std::vector<pollfd> fds;
        while (!stopping_.load(std::memory_order_acquire)) {
            fds.clear();
            fds.push_back({tasks_.fd(), POLLIN, 0});
            for (const Device& device : devices_)
                fds.push_back({device.fd, POLLIN, 0});
            const int ready = poll(fds.data(), fds.size(), rescan_ms);
            if (ready < 0)
                continue;
            if (ready == 0) {
                scan();
                continue;
            }
            if ((fds[0].revents & POLLIN) != 0)
                tasks_.drain();
            bool lost = false;
            for (size_t index = 1; index < fds.size(); ++index) {
                if ((fds[index].revents & (POLLERR | POLLHUP)) != 0)
                    lost = true;
                else if ((fds[index].revents & POLLIN) != 0)
                    read_device(devices_[index - 1]);
            }
            if (lost)
                drop_dead();
        }
    }

    void stop() override {
        stopping_.store(true, std::memory_order_release);
        tasks_.post([] {});
    }

    void post(std::function<void()> task) override { tasks_.post(std::move(task)); }

  private:
    struct Device {
        int fd{-1};
        std::string path;
        double dx{};
        double dy{};
        double wheel{};
        double pan{};
        // The keys this device holds down: they go away with it, so an
        // unplugged keyboard cannot leave one held for good.
        std::bitset<KEY_CNT> keys{};
    };

    [[nodiscard]] bool any_key_held() const {
        return std::any_of(devices_.begin(), devices_.end(),
                           [](const Device& device) { return device.keys.any(); });
    }

    void scan() {
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator("/dev/input", ignored)) {
            const std::string path = entry.path().string();
            if (entry.path().filename().string().rfind("event", 0) != 0 ||
                std::any_of(devices_.begin(), devices_.end(),
                            [&](const Device& device) { return device.path == path; }))
                continue;
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            unsigned long keys[KEY_MAX / (8 * sizeof(unsigned long)) + 1]{};
            unsigned long rel[REL_MAX / (8 * sizeof(unsigned long)) + 1]{};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys);
            ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
            const bool mouse = has_bit(rel, REL_X) && has_bit(keys, BTN_LEFT);
            const bool keyboard = has_bit(keys, KEY_A) && has_bit(keys, KEY_ENTER);
            if (!mouse && !keyboard) {
                close(fd);
                continue;
            }
            if (physically_grabbed_)
                ioctl(fd, EVIOCGRAB, 1);
            Device device;
            device.fd = fd;
            device.path = path;
            devices_.push_back(std::move(device));
        }
    }

    void drop_dead() {
        for (auto it = devices_.begin(); it != devices_.end();) {
            input_event probe;
            if (read(it->fd, &probe, sizeof probe) < 0 && errno == ENODEV) {
                close(it->fd);
                it = devices_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Taking the devices while a key is down would leave the desktop
    // thinking it is still held, so the kernel grab waits for the moment no
    // key is pressed. Until it is in place nothing is sent to the phone:
    // input never goes to both sides at once.
    void grab_when_idle() {
        if (!grabbed_ || physically_grabbed_ || any_key_held())
            return;
        for (size_t index = 0; index < devices_.size(); ++index) {
            if (ioctl(devices_[index].fd, EVIOCGRAB, 1) == 0)
                continue;
            // One device could not be taken (another program holds it):
            // give the others back and stay on the PC.
            for (size_t taken = 0; taken < index; ++taken)
                ioctl(devices_[taken].fd, EVIOCGRAB, 0);
            grabbed_ = false;
            handler_->lost();
            return;
        }
        physically_grabbed_ = true;
    }

    void read_device(Device& device) {
        input_event events[64];
        for (;;) {
            const ssize_t got = read(device.fd, events, sizeof events);
            if (got < static_cast<ssize_t>(sizeof(input_event)))
                return;
            const size_t count = static_cast<size_t>(got) / sizeof(input_event);
            for (size_t index = 0; index < count; ++index)
                handle(device, events[index]);
        }
    }

    void handle(Device& device, const input_event& event) {
        if (event.type == EV_REL) {
            switch (event.code) {
            case REL_X:
                device.dx += event.value;
                break;
            case REL_Y:
                device.dy += event.value;
                break;
            case REL_WHEEL:
                device.wheel += event.value;
                break;
            case REL_HWHEEL:
                device.pan += event.value;
                break;
            default:
                break;
            }
        } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
            if (physically_grabbed_) {
                if (device.dx != 0.0 || device.dy != 0.0)
                    handler_->motion(device.dx, device.dy);
                if (device.wheel != 0.0 || device.pan != 0.0)
                    handler_->scroll(device.wheel, device.pan);
            }
            device.dx = device.dy = device.wheel = device.pan = 0.0;
        } else if (event.type == EV_KEY && event.value != 2) {
            // Value 2 is the kernel's key repeat.
            const bool down = event.value == 1;
            if (event.code < device.keys.size())
                device.keys[event.code] = down;
            unsigned button = 0;
            HidKey key;
            if (evdev_to_button(event.code, button)) {
                if (physically_grabbed_)
                    handler_->button(button, down);
            } else if (evdev_to_hid(event.code, key)) {
                // The handler may grab or release from inside this call.
                handler_->key(key, down, physically_grabbed_);
            }
            if (!down)
                grab_when_idle();
        }
    }

    CaptureHandler* handler_{};
    std::vector<Device> devices_;
    bool grabbed_{};
    bool physically_grabbed_{};
    TaskQueue tasks_;
    std::atomic<bool> stopping_{};
};

} // namespace

std::unique_ptr<Capture> make_evdev_capture() { return std::make_unique<EvdevCapture>(); }

} // namespace aoas
