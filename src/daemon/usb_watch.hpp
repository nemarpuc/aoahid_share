// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace aoas {

// Tells when a USB device came or went, so the daemon scans for phones then
// and not on a timer: a scan sends a request to every device on the bus.
// Linux only (inotify on /dev/bus/usb); elsewhere start() returns false and
// the caller keeps polling.
class UsbWatch {
  public:
    ~UsbWatch() { stop(); }

    // `changed` runs on the watch's own thread, once the bus has been quiet
    // for a moment after a burst of changes.
    [[nodiscard]] bool start(std::function<void()> changed);
    void stop();

  private:
    std::thread thread_;
    std::atomic<bool> stopping_{};
    int stop_pipe_[2]{-1, -1};
};

} // namespace aoas
