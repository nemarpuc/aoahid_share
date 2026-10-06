// SPDX-License-Identifier: MIT
#include "paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace aoas {
namespace {

namespace fs = std::filesystem;

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "";
}

fs::path executable_directory() {
    std::error_code ignored;
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0)
        return {};
    return fs::canonical(buffer, ignored).parent_path();
#else
    return fs::read_symlink("/proc/self/exe", ignored).parent_path();
#endif
}

// A user's own folder for the settings, used where the program's folder
// cannot be written to (an installed copy).
fs::path user_directory() {
#if defined(_WIN32)
    return fs::path(env("APPDATA")) / "aoahid_share";
#elif defined(__APPLE__)
    return fs::path(env("HOME")) / "Library" / "Application Support" / "aoahid_share";
#else
    const std::string base = env("XDG_CONFIG_HOME");
    return (!base.empty() ? fs::path(base) : fs::path(env("HOME")) / ".config") / "aoahid_share";
#endif
}

// Where the older layout kept the state file (the config was in user_directory()).
fs::path legacy_state_directory() {
#if defined(_WIN32)
    return fs::path(env("LOCALAPPDATA")) / "aoahid_share";
#elif defined(__APPLE__)
    return user_directory();
#else
    const std::string base = env("XDG_STATE_HOME");
    return (!base.empty() ? fs::path(base) : fs::path(env("HOME")) / ".local" / "state") /
           "aoahid_share";
#endif
}

#if defined(_WIN32)
bool writable_directory(const fs::path& directory) {
    std::error_code error;
    fs::create_directories(directory, error);
    const fs::path probe = directory / ".write_test";
    {
        std::ofstream file(probe, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
    }
    fs::remove(probe, error);
    return true;
}
#else
// The settings name a program to run (`[adb] path`), so the folder is used
// only when it is this user's own and nobody else can write to it: a copy
// unpacked into a shared place falls back to the user's folder instead of
// reading a config another user could have changed. Nothing is written to
// find that out, so no link left there is followed.
bool writable_directory(const fs::path& directory) {
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
        return false;
    struct stat info{};
    if (lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return false;
    return access(directory.c_str(), W_OK | X_OK) == 0;
}
#endif

void copy_if_missing(const fs::path& from, const fs::path& to) {
    std::error_code error;
    // A link is not followed: only a file that is itself there is taken.
    if (fs::symlink_status(from, error).type() == fs::file_type::regular && !fs::exists(to, error))
        fs::copy_file(from, to, error);
}

// Settings an older layout kept elsewhere are copied, never moved, the
// first time the program's own folder is used.
void bring_old_settings(const fs::path& root) {
    std::error_code error;
    const fs::path old_config = user_directory();
    const fs::path old_state = legacy_state_directory();
    // The files the older layout kept under "devices": copied, so a
    // program of that version still finds them.
    if (fs::is_directory(root / "devices", error) && !fs::exists(root / "device", error)) {
        fs::create_directories(root / "device", error);
        for (const fs::directory_entry& entry : fs::directory_iterator(root / "devices", error)) {
            if (entry.path().extension() == ".ini")
                copy_if_missing(entry.path(), root / "device" / entry.path().filename());
        }
    }
    // The state holds what is needed to put a phone's settings back.
    copy_if_missing(old_state / "state.ini", root / "state.ini");
    if (fs::exists(root / "config.ini", error))
        return;
    fs::create_directories(root / "device", error);
    copy_if_missing(old_config / "config.ini", root / "config.ini");
    for (const fs::directory_entry& entry : fs::directory_iterator(old_config / "devices", error)) {
        if (entry.path().extension() == ".ini")
            copy_if_missing(entry.path(), root / "device" / entry.path().filename());
    }
}

fs::path find_settings_directory() {
    const std::string custom = env("AOAHID_SHARE_SETTINGS_DIR");
    if (!custom.empty()) {
        std::error_code error;
        fs::create_directories(custom, error);
        return custom;
    }
    const fs::path exe_dir = executable_directory();
    // Inside an application bundle the folder is not the user's to fill.
    const bool bundled = exe_dir.generic_string().find(".app/Contents/") != std::string::npos;
    if (!exe_dir.empty() && !bundled && writable_directory(exe_dir / "settings")) {
        bring_old_settings(exe_dir / "settings");
        return exe_dir / "settings";
    }
    std::error_code error;
    fs::create_directories(user_directory(), error);
    bring_old_settings(user_directory());
    return user_directory();
}

} // namespace

std::string settings_dir() {
    // Worked out once: the daemon asks for the log's place on every line.
    static const std::string directory = find_settings_directory().string();
    return directory;
}

std::string config_file_path() { return (fs::path(settings_dir()) / "config.ini").string(); }

std::string state_file_path() { return (fs::path(settings_dir()) / "state.ini").string(); }

std::string control_endpoint() {
#if defined(_WIN32)
    return R"(\\.\pipe\aoahid_share)";
#else
    // XDG_RUNTIME_DIR is already private to the user.
    const std::string runtime = env("XDG_RUNTIME_DIR");
    if (!runtime.empty())
        return (std::filesystem::path(runtime) / "aoahid_share.sock").string();

    // Otherwise the socket goes into a directory of its own under the shared
    // temporary directory. Another user could have made that name first, so
    // it is used only when it is a real directory, owned by this user and
    // closed to everyone else.
    std::string base = env("TMPDIR");
    if (base.empty())
        base = "/tmp";
    const std::string directory =
        (std::filesystem::path(base) / ("aoahid_share-" + std::to_string(getuid()))).string();
    static_cast<void>(mkdir(directory.c_str(), 0700));
    struct stat info{};
    if (lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 077) != 0)
        return {};
    return directory + "/control.sock";
#endif
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    std::ostringstream text;
    text << file.rdbuf();
    out = text.str();
    return true;
}

bool write_file(const std::string& path, const std::string& text) {
    const std::string temporary = path + ".tmp";
    std::error_code error;
#ifdef _WIN32
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << text;
        if (!file.flush())
            return false;
    }
#else
    // The folder may be one others can write to (an export): a link left
    // under the temporary name must not be followed. Removing it and then
    // creating the file exclusively never opens what a link points at.
    std::filesystem::remove(temporary, error);
    const int fd =
        open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    size_t done = 0;
    while (done < text.size()) {
        const ssize_t wrote = write(fd, text.data() + done, text.size() - done);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            close(fd);
            std::filesystem::remove(temporary, error);
            return false;
        }
        done += static_cast<size_t>(wrote);
    }
    close(fd);
#endif
    std::filesystem::rename(temporary, path, error);
    return !error;
}

std::string state_get(const std::string& text, const std::string& key) {
    std::istringstream lines(text);
    std::string line;
    const std::string prefix = key + " = ";
    while (std::getline(lines, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0)
            return line.substr(prefix.size());
    }
    return {};
}

std::string state_set(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream lines(text);
    std::string line;
    std::string result;
    const std::string prefix = key + " = ";
    while (std::getline(lines, line)) {
        if (line.empty() || line.compare(0, prefix.size(), prefix) == 0)
            continue;
        result += line + "\n";
    }
    if (!value.empty())
        result += prefix + value + "\n";
    return result;
}

} // namespace aoas
