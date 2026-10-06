// SPDX-License-Identifier: MIT
#include "usb_watch.hpp"

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <filesystem>
#endif

namespace aoas {

#ifdef __linux__

namespace {

constexpr const char* bus_root = "/dev/bus/usb";
// A phone re-enumerates in a burst of changes; the scan waits for the end.
constexpr int quiet_ms = 300;

void watch_bus(const int inotify, const char* path) {
    static_cast<void>(inotify_add_watch(inotify, path, IN_CREATE | IN_DELETE | IN_ATTRIB));
}

} // namespace

bool UsbWatch::start(std::function<void()> changed) {
    const int inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify < 0)
        return false;
    std::error_code ignored;
    if (!std::filesystem::is_directory(bus_root, ignored) ||
        pipe2(stop_pipe_, O_CLOEXEC | O_NONBLOCK) != 0) {
        close(inotify);
        return false;
    }
    watch_bus(inotify, bus_root);
    for (const auto& entry : std::filesystem::directory_iterator(bus_root, ignored)) {
        if (entry.is_directory(ignored))
            watch_bus(inotify, entry.path().c_str());
    }
    stopping_.store(false);
    thread_ = std::thread([this, inotify, changed = std::move(changed)] {
        char buffer[4096];
        bool pending = false;
        while (!stopping_.load()) {
            pollfd fds[2] = {{inotify, POLLIN, 0}, {stop_pipe_[0], POLLIN, 0}};
            const int ready = poll(fds, 2, pending ? quiet_ms : -1);
            if (ready < 0)
                continue;
            if ((fds[1].revents & POLLIN) != 0)
                break;
            if (ready == 0) {
                pending = false;
                changed();
                continue;
            }
            while (read(inotify, buffer, sizeof buffer) > 0)
                pending = true;
        }
        close(inotify);
    });
    return true;
}

void UsbWatch::stop() {
    if (!thread_.joinable())
        return;
    stopping_.store(true);
    // A full pipe means a stop is already waiting there.
    if (write(stop_pipe_[1], "x", 1) < 0)
        stopping_.store(true);
    thread_.join();
    close(stop_pipe_[0]);
    close(stop_pipe_[1]);
    stop_pipe_[0] = stop_pipe_[1] = -1;
}

#else

bool UsbWatch::start(std::function<void()>) { return false; }
void UsbWatch::stop() {}

#endif

} // namespace aoas
