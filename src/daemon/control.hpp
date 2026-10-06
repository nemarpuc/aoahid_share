// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace aoas {

// Serves one-line commands on the control endpoint: a Unix socket only its
// owner can open, or a named pipe on Windows. One command per connection;
// the answer is everything written back before the connection closes.
class ControlServer {
  public:
    using Handler = std::function<std::string(const std::string&)>;

    ControlServer() = default;
    ~ControlServer() { stop(); }
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Empty on success. Fails when another daemon already serves the endpoint.
    [[nodiscard]] std::string start(Handler handler);
    void stop();

  private:
    void serve();

    Handler handler_;
    std::thread thread_;
    std::atomic<bool> stopping_{};
#ifdef _WIN32
    void* stop_event_{};
#else
    int listener_{-1};
    int wake_[2]{-1, -1};
#endif
};

// Sends one command to the running daemon. False when none is running.
[[nodiscard]] bool control_request(const std::string& line, std::string& answer);

} // namespace aoas
