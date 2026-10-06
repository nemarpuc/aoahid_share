// SPDX-License-Identifier: MIT
#include "process.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace aoas {

#ifdef _WIN32

namespace {

// CommandLineToArgvW's rules: quote, and double the backslashes before a quote.
std::string command_line(const std::vector<std::string>& argv) {
    std::string line;
    for (const std::string& arg : argv) {
        if (!line.empty())
            line += ' ';
        line += '"';
        size_t slashes = 0;
        for (const char c : arg) {
            if (c == '\\') {
                ++slashes;
            } else if (c == '"') {
                line.append(slashes + 1, '\\');
                slashes = 0;
            } else {
                slashes = 0;
            }
            line += c;
        }
        line.append(slashes, '\\');
        line += '"';
    }
    return line;
}

bool spawn(const std::vector<std::string>& argv, HANDLE& process, HANDLE& read_end) {
    if (argv.empty())
        return false;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &attributes, 0))
        return false;
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // Only the pipe is handed down. Sockets are inheritable by default, and
    // adb leaves a server behind that would keep the ADB proxy's port bound.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> storage(size);
    const auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    HANDLE inherited[] = {write_end};
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof inherited, nullptr, nullptr)) {
        CloseHandle(write_end);
        CloseHandle(read_end);
        return false;
    }
    STARTUPINFOEXA startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = write_end;
    startup.StartupInfo.hStdError = write_end;
    startup.lpAttributeList = list;
    PROCESS_INFORMATION info{};
    std::string line = command_line(argv);
    const BOOL created = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                        nullptr, &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(list);
    CloseHandle(write_end);
    if (!created) {
        CloseHandle(read_end);
        return false;
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
    return true;
}

} // namespace

RunResult run_process(const std::vector<std::string>& argv, const int timeout_ms) {
    RunResult result;
    HANDLE process = nullptr;
    HANDLE pipe = nullptr;
    if (!spawn(argv, process, pipe))
        return result;
    result.started = true;

    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    char chunk[4096];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available != 0) {
            DWORD got = 0;
            if (!ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) || got == 0)
                break;
            result.output.append(chunk, got);
            continue;
        }
        if (WaitForSingleObject(process, 10) == WAIT_OBJECT_0) {
            // The program ended; whatever it wrote last is still in the pipe.
            if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available != 0)
                continue;
            break;
        }
        if (GetTickCount64() >= deadline) {
            result.timed_out = true;
            TerminateProcess(process, 1);
            break;
        }
    }
    WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(process, &code))
        result.exit_code = static_cast<int>(code);
    CloseHandle(pipe);
    CloseHandle(process);
    return result;
}

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty())
        return false;
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::string line = command_line(argv);
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup,
                        &info))
        return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}

void reap_detached() noexcept {}

#else

namespace {

// A program started from here gets its three standard streams and nothing
// else of ours. adb leaves a server behind that lives on; a listening socket
// it inherited (the ADB proxy's) would stay bound long after this process.
void keep_descriptors_home(posix_spawn_file_actions_t& actions, posix_spawnattr_t& attributes) {
#if defined(__APPLE__)
    short flags = 0;
    posix_spawnattr_getflags(&attributes, &flags);
    posix_spawnattr_setflags(&attributes, static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT));
    static_cast<void>(actions);
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
    static_cast<void>(attributes);
#else
    static_cast<void>(actions);
    static_cast<void>(attributes);
#endif
}

std::mutex detached_mutex;
std::vector<pid_t> detached;

bool spawn(const std::vector<std::string>& argv, pid_t& pid, int& read_end) {
    if (argv.empty())
        return false;
    int ends[2];
#if defined(__linux__)
    if (pipe2(ends, O_CLOEXEC) != 0)
        return false;
#else
    if (pipe(ends) != 0)
        return false;
    static_cast<void>(fcntl(ends[0], F_SETFD, FD_CLOEXEC));
#endif

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, ends[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, ends[1]);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    keep_descriptors_home(actions, attributes);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
        args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);

    const int error = posix_spawnp(&pid, args[0], &actions, &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(ends[1]);
    if (error != 0) {
        close(ends[0]);
        return false;
    }
    read_end = ends[0];
    return true;
}

} // namespace

RunResult run_process(const std::vector<std::string>& argv, const int timeout_ms) {
    RunResult result;
    pid_t pid = -1;
    int fd = -1;
    if (!spawn(argv, pid, fd))
        return result;
    result.started = true;

    using clock = std::chrono::steady_clock;
    const clock::time_point deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    char chunk[4096];
    for (;;) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            break;
        }
        pollfd waiting{fd, POLLIN, 0};
        const int ready = poll(&waiting, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0)
            continue;
        const ssize_t got = read(fd, chunk, sizeof chunk);
        if (got > 0) {
            result.output.append(chunk, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        break;
    }
    close(fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    return result;
}

bool spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty())
        return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    // A session of its own: closing the starter's terminal or window does
    // not take it along.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    keep_descriptors_home(actions, attributes);

    std::vector<char*> args;
    for (const std::string& arg : argv)
        args.push_back(const_cast<char*>(arg.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    const int error = posix_spawnp(&pid, args[0], &actions, &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (error != 0)
        return false;
    const std::lock_guard<std::mutex> lock(detached_mutex);
    detached.push_back(pid);
    return true;
}

void reap_detached() noexcept {
    const std::lock_guard<std::mutex> lock(detached_mutex);
    for (size_t index = 0; index < detached.size();) {
        // Each is still our child until it is waited for, so the number
        // cannot have passed to another process.
        int status = 0;
        const pid_t result = waitpid(detached[index], &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR))
            ++index;
        else
            detached.erase(detached.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

#endif

} // namespace aoas
