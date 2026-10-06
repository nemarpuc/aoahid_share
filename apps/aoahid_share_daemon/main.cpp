// SPDX-License-Identifier: MIT
//
// aoahid_share_daemon: the background process. It has no window and prints
// only to standard error; everything else goes through the control endpoint.
#include "control.hpp"
#include "daemon.hpp"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <pthread.h>
#endif

namespace {

aoas::Daemon* running = nullptr;

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD) {
    if (running != nullptr)
        running->quit();
    return TRUE;
}
#endif

} // namespace

int main(const int argc, char** argv) {
    if (argc > 1 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0)) {
        std::puts("aoahid_share_daemon " AOAHID_SHARE_VERSION);
        return 0;
    }
    if (argc > 1) {
        std::puts("usage: aoahid_share_daemon [--version]\n"
                  "Runs in the background; control it with aoahid_share or aoahid_share_gui.");
        return std::strcmp(argv[1], "--help") == 0 ? 0 : 2;
    }

#ifndef _WIN32
    // Signals are taken by one thread below; every other thread, including
    // the ones libraries start, inherits the mask and never sees them.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    aoas::Daemon daemon;
    running = &daemon;
    aoas::ControlServer control;
    const std::string error =
        control.start([&daemon](const std::string& line) { return daemon.command(line); });
    if (!error.empty()) {
        std::fprintf(stderr, "aoahid_share: %s\n", error.c_str());
        return 1;
    }

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::thread([&daemon, signals] {
        int received = 0;
        sigwait(&signals, &received);
        daemon.quit();
    }).detach();
#endif

    const int code = daemon.run();
    control.stop();
    running = nullptr;
    return code;
}
