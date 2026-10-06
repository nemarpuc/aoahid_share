// SPDX-License-Identifier: MIT
#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace aoas {

// Functions other threads hand to a poll() loop, and the pipe that wakes it.
class TaskQueue {
  public:
    TaskQueue() {
        if (pipe(ends_) == 0) {
            for (const int end : ends_) {
                static_cast<void>(fcntl(end, F_SETFL, O_NONBLOCK));
                static_cast<void>(fcntl(end, F_SETFD, FD_CLOEXEC));
            }
        } else {
            ends_[0] = ends_[1] = -1;
        }
    }
    ~TaskQueue() {
        for (const int end : ends_) {
            if (end >= 0)
                close(end);
        }
    }
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // The descriptor to poll for readability.
    [[nodiscard]] int fd() const noexcept { return ends_[0]; }

    void post(std::function<void()> task) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(task));
        }
        const char byte = 0;
        static_cast<void>(!write(ends_[1], &byte, 1));
    }

    // Runs everything posted so far, on the caller's thread.
    void drain() {
        char bytes[64];
        while (read(ends_[0], bytes, sizeof bytes) > 0) {
        }
        std::vector<std::function<void()>> tasks;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            tasks.swap(tasks_);
        }
        for (const std::function<void()>& task : tasks)
            task();
    }

  private:
    int ends_[2]{-1, -1};
    std::mutex mutex_;
    std::vector<std::function<void()>> tasks_;
};

} // namespace aoas
