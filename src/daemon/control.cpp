// SPDX-License-Identifier: MIT
#include "control.hpp"

#include "paths.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace aoas {
namespace {

constexpr size_t longest_command = 4096;

#ifndef _WIN32
// How long a connected client may take to send its line.
constexpr int client_timeout_ms = 2000;

bool socket_address(const std::string& path, sockaddr_un& address) {
    if (path.empty() || path.size() >= sizeof(address.sun_path))
        return false;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// macOS has neither SOCK_CLOEXEC nor MSG_NOSIGNAL; the same two things are
// set on the descriptor instead.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

int open_socket() {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return fd;
    static_cast<void>(fcntl(fd, F_SETFD, FD_CLOEXEC));
#ifdef SO_NOSIGPIPE
    const int on = 1;
    static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on));
#endif
    return fd;
}

void write_all(const int fd, const std::string& text) {
    size_t sent = 0;
    while (sent < text.size()) {
        const ssize_t wrote = send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (wrote <= 0) {
            if (wrote < 0 && errno == EINTR)
                continue;
            return;
        }
        sent += static_cast<size_t>(wrote);
    }
}
#endif

} // namespace

#ifdef _WIN32

std::string ControlServer::start(Handler handler) {
    handler_ = std::move(handler);
    std::string probe;
    if (control_request("status", probe))
        return "another aoahid_share_daemon is already running";
    stop_event_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    stopping_.store(false);
    thread_ = std::thread([this] { serve(); });
    return {};
}

void ControlServer::serve() {
    const std::string name = control_endpoint();
    while (!stopping_.load()) {
        // The default security descriptor lets only the creating user and
        // administrators open the pipe.
        const HANDLE pipe = CreateNamedPipeA(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            return;
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        bool connected =
            ConnectNamedPipe(pipe, &overlapped) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected && GetLastError() == ERROR_IO_PENDING) {
            const HANDLE waits[] = {overlapped.hEvent, static_cast<HANDLE>(stop_event_)};
            connected = WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0;
        }
        if (connected) {
            std::string line;
            char chunk[512];
            for (;;) {
                DWORD got = 0;
                OVERLAPPED reading{};
                reading.hEvent = overlapped.hEvent;
                ResetEvent(reading.hEvent);
                if (!ReadFile(pipe, chunk, sizeof chunk, &got, &reading) &&
                    (GetLastError() != ERROR_IO_PENDING ||
                     !GetOverlappedResult(pipe, &reading, &got, TRUE)))
                    break;
                line.append(chunk, got);
                if (got == 0 || line.find('\n') != std::string::npos ||
                    line.size() > longest_command)
                    break;
            }
            const size_t end = line.find('\n');
            if (end != std::string::npos) {
                line.resize(end);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const std::string answer = handler_(line);
                DWORD wrote = 0;
                OVERLAPPED writing{};
                writing.hEvent = overlapped.hEvent;
                ResetEvent(writing.hEvent);
                if (!WriteFile(pipe, answer.data(), static_cast<DWORD>(answer.size()), &wrote,
                               &writing) &&
                    GetLastError() == ERROR_IO_PENDING)
                    GetOverlappedResult(pipe, &writing, &wrote, TRUE);
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(overlapped.hEvent);
        CloseHandle(pipe);
    }
}

void ControlServer::stop() {
    if (!thread_.joinable())
        return;
    stopping_.store(true);
    SetEvent(static_cast<HANDLE>(stop_event_));
    thread_.join();
    CloseHandle(static_cast<HANDLE>(stop_event_));
    stop_event_ = nullptr;
}

bool control_request(const std::string& line, std::string& answer) {
    const std::string name = control_endpoint();
    if (!WaitNamedPipeA(name.c_str(), 2000))
        return false;
    const HANDLE pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
        return false;
    const std::string message = line + "\n";
    DWORD wrote = 0;
    WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &wrote, nullptr);
    answer.clear();
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) && got != 0)
        answer.append(chunk, got);
    CloseHandle(pipe);
    return true;
}

#else

std::string ControlServer::start(Handler handler) {
    handler_ = std::move(handler);
    const std::string path = control_endpoint();
    sockaddr_un address{};
    if (!socket_address(path, address))
        return "the control socket path is too long: " + path;

    std::string probe;
    if (control_request("status", probe))
        return "another aoahid_share_daemon is already running";
    // Nobody answered, so a file left there belongs to a daemon that died.
    unlink(path.c_str());

    listener_ = open_socket();
    if (listener_ < 0)
        return std::string("the control socket could not be created: ") + std::strerror(errno);
    // Created private from the start; bind() applies the mask.
    const mode_t previous = umask(0177);
    const int bound = bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    umask(previous);
    if (bound != 0 || listen(listener_, 8) != 0 || pipe(wake_) != 0) {
        const std::string reason = std::strerror(errno);
        close(listener_);
        listener_ = -1;
        return "the control socket could not be opened at " + path + ": " + reason;
    }
    stopping_.store(false);
    thread_ = std::thread([this] { serve(); });
    return {};
}

void ControlServer::serve() {
    while (!stopping_.load()) {
        pollfd waiting[2] = {{listener_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        if (poll(waiting, 2, -1) < 0 || (waiting[1].revents & POLLIN) != 0)
            continue;
        const int client = accept(listener_, nullptr, nullptr);
        if (client < 0)
            continue;
        std::string line;
        char chunk[512];
        while (line.find('\n') == std::string::npos && line.size() <= longest_command) {
            pollfd reading{client, POLLIN, 0};
            if (poll(&reading, 1, client_timeout_ms) <= 0)
                break;
            const ssize_t got = recv(client, chunk, sizeof chunk, 0);
            if (got <= 0)
                break;
            line.append(chunk, static_cast<size_t>(got));
        }
        const size_t end = line.find('\n');
        if (end != std::string::npos) {
            line.resize(end);
            write_all(client, handler_(line));
        }
        close(client);
    }
}

void ControlServer::stop() {
    if (!thread_.joinable())
        return;
    stopping_.store(true);
    const char byte = 0;
    static_cast<void>(!write(wake_[1], &byte, 1));
    thread_.join();
    close(listener_);
    close(wake_[0]);
    close(wake_[1]);
    listener_ = wake_[0] = wake_[1] = -1;
    unlink(control_endpoint().c_str());
}

bool control_request(const std::string& line, std::string& answer) {
    sockaddr_un address{};
    if (!socket_address(control_endpoint(), address))
        return false;
    const int fd = open_socket();
    if (fd < 0)
        return false;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }
    // A daemon that is stuck must not freeze a window that asked it. The
    // slowest command (a reload that closes every phone) takes seconds.
    timeval limit{};
    limit.tv_sec = 30;
    static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit));
    static_cast<void>(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof limit));
    write_all(fd, line + "\n");
    answer.clear();
    char chunk[4096];
    for (;;) {
        const ssize_t got = recv(fd, chunk, sizeof chunk, 0);
        if (got > 0) {
            answer.append(chunk, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);
    return true;
}

#endif

} // namespace aoas
