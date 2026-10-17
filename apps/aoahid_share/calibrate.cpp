// SPDX-License-Identifier: MIT
#include "calibrate.hpp"

#include "aoahid_share/config.hpp"
#include "control.hpp"
#include "paths.hpp"
#include "store.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <conio.h>
#include <windows.h>
#else
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace aoas {
namespace {

#if !defined(_WIN32)
class RawTerminal {
public:
    RawTerminal() {
        if (tcgetattr(STDIN_FILENO, &orig_) == 0) {
            termios raw = orig_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
                active_ = true;
        }
    }

    ~RawTerminal() { restore(); }

    void restore() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
            active_ = false;
        }
    }

    int read_key(int timeout_ms) {
        pollfd fds[1];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        int ret = poll(fds, 1, timeout_ms);
        if (ret > 0 && (fds[0].revents & POLLIN)) {
            unsigned char c = 0;
            if (read(STDIN_FILENO, &c, 1) == 1)
                return static_cast<int>(c);
        }
        return -1;
    }

private:
    termios orig_{};
    bool active_{false};
};
#else
class RawTerminal {
public:
    void restore() {}
    int read_key(int timeout_ms) {
        if (timeout_ms > 0)
            Sleep(timeout_ms);
        if (_kbhit())
            return _getch();
        return -1;
    }
};
#endif

int measure_axis(const std::string& target, const char* axis_name, RawTerminal& term) {
    std::string answer;
    // Reset cursor to top-left corner
    if (!control_request("probe " + target + " corner", answer) ||
        answer.rfind("error=", 0) == 0) {
        std::fprintf(stderr, "\nFailed to reset cursor: %s\n", answer.c_str());
        return -1;
    }

    int count = 0;
    auto last_press = std::chrono::steady_clock::now();
    int consecutive_count = 0;

    std::printf("\n  Measuring %s:\n", axis_name);
    std::printf("  - Tap SPACE for single count (1 count)\n");
    std::printf("  - Hold SPACE to move smoothly\n");
    std::printf("  - BACKSPACE to step back\n");
    std::printf("  - ENTER when cursor hits the edge\n");
    std::printf("  - Q to abort\n");
    std::printf("  * Tip: Pushing slightly past the edge (+5 to +10 extra counts) is recommended\n");
    std::printf("         so the cursor reliably reaches the far edge to tap buttons.\n\n");

    while (true) {
        int key = term.read_key(20);
        if (key < 0) {
            consecutive_count = 0;
            continue;
        }

        if (key == 'q' || key == 'Q' || key == 27) { // 27 = ESC
            std::puts("\n  Calibration aborted by user.");
            return -1;
        }

        if (key == '\n' || key == '\r') {
            std::puts("\n");
            break;
        }

        if (key == ' ') {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_press).count();
            last_press = now;

            int step = 1;
            if (elapsed < 120) {
                consecutive_count++;
                if (consecutive_count > 15)
                    step = 10;
                else if (consecutive_count > 5)
                    step = 5;
                else
                    step = 2;
            } else {
                consecutive_count = 0;
                step = 1;
            }

            count += step;

            std::string cmd;
            if (std::strcmp(axis_name, "Horizontal (Width)") == 0)
                cmd = "probe " + target + " move " + std::to_string(step) + " 0";
            else
                cmd = "probe " + target + " move 0 " + std::to_string(step);

            if (!control_request(cmd, answer) || answer.rfind("error=", 0) == 0) {
                std::fprintf(stderr, "\nFailed to move cursor: %s\n", answer.c_str());
                return -1;
            }
        } else if (key == 127 || key == 8) { // Backspace
            if (count > 0) {
                int step = (count >= 5) ? 5 : 1;
                count -= step;
                std::string cmd;
                if (std::strcmp(axis_name, "Horizontal (Width)") == 0)
                    cmd = "probe " + target + " move -" + std::to_string(step) + " 0";
                else
                    cmd = "probe " + target + " move 0 -" + std::to_string(step);

                static_cast<void>(control_request(cmd, answer));
            }
        }

        std::printf("\r  -> Accumulated: %5d counts  ", count);
        std::fflush(stdout);
    }

    return count;
}

} // namespace

int run_calibrate(int argc, char** argv) {
    std::string target_name;
    if (argc >= 3)
        target_name = argv[2];

    Config current;
    std::string problem = load_store(current);
    if (!problem.empty()) {
        std::fprintf(stderr, "aoahid_share: %s\n", problem.c_str());
        return 1;
    }

    if (current.devices.empty()) {
        std::fputs("aoahid_share calibrate: no devices configured in settings.\n", stderr);
        return 1;
    }

    size_t dev_index = 0;
    if (!target_name.empty()) {
        dev_index = find_device(current, target_name);
        if (dev_index >= current.devices.size()) {
            std::fprintf(stderr, "aoahid_share calibrate: device '%s' not found.\n",
                         target_name.c_str());
            return 1;
        }
    }

    DeviceConfig& dev = current.devices[dev_index];
    const std::string dev_id = dev.serial.empty() ? dev.name : dev.serial;

    // Verify daemon is running
    std::string answer;
    if (!control_request("status", answer)) {
        std::fputs("aoahid_share calibrate: daemon is not running (start aoahid_share_daemon).\n",
                   stderr);
        return 1;
    }

    std::puts("======================================================================");
    std::puts(" aoahid_share Interactive Calibration Wizard");
    std::printf(" Target device: %s (serial: %s)\n", dev.name.c_str(), dev.serial.c_str());
    std::puts("======================================================================");
    std::puts("\n[NOTICE]");
    std::puts("  1. Before calibrating, please make sure 'Pointer Acceleration' /");
    std::puts("     'Enhance pointer precision' is turned OFF in your Android settings.");
    std::puts("     This ensures a constant linear gain across your display.");
    std::puts("  2. If you change pointer speed/settings on your device later,");
    std::puts("     re-calibration will be required.\n");

    std::printf("Press ENTER to begin calibration...");
    std::fflush(stdout);
    {
        int ch;
        while ((ch = std::getchar()) != '\n' && ch != EOF) {}
    }

    RawTerminal term;

    // Step 1: Measure horizontal
    std::puts("\n[Step 1/2] Width Measurement");
    int count_x = measure_axis(dev_id, "Horizontal (Width)", term);
    if (count_x <= 0) {
        term.restore();
        std::fputs("Calibration failed or aborted.\n", stderr);
        return 1;
    }

    // Step 2: Measure vertical
    std::puts("[Step 2/2] Height Measurement");
    int count_y = measure_axis(dev_id, "Vertical (Height)", term);
    if (count_y <= 0) {
        term.restore();
        std::fputs("Calibration failed or aborted.\n", stderr);
        return 1;
    }

    term.restore();

    if (dev.rotation < 0)
        dev.rotation = 0;

    const bool turned = dev.rotation == 90 || dev.rotation == 270;
    const int comp_w = (turned ? count_y : count_x) + 1;
    const int comp_h = (turned ? count_x : count_y) + 1;

    std::puts("======================================================================");
    std::puts(" Calibration Summary");
    std::printf("   Target: %s\n", dev.name.c_str());
    std::printf("   Measured Counts: X = %d,  Y = %d\n", count_x, count_y);
    std::printf("   Screen Rotation: %d°\n", dev.rotation);
    std::printf("   Calculated Size: Width = %d, Height = %d (Gain = 1.0, Accel = off)\n", comp_w,
                comp_h);
    std::puts("======================================================================");

    std::printf("Save these dimensions to device settings? [Y/n]: ");
    std::fflush(stdout);

    char choice[16] = {0};
    if (std::fgets(choice, sizeof(choice), stdin) == nullptr)
        return 0;

    if (choice[0] == '\n' || choice[0] == 'y' || choice[0] == 'Y') {
        dev.width = comp_w;
        dev.height = comp_h;
        dev.gain = 1.0;
        dev.accel = AccelSetting::off;
        if (dev.diagonal_inch <= 0.0)
            dev.diagonal_inch = 6.5;
        if (!save_store(current)) {
            std::fputs("Error: failed to save settings file.\n", stderr);
            return 1;
        }
        std::string reload_ans;
        static_cast<void>(control_request("reload", reload_ans));
        std::puts("Successfully saved dimensions and reloaded daemon!");
    } else {
        std::puts("Cancelled. Settings were not modified.");
    }

    return 0;
}

} // namespace aoas
