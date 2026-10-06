// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace aoas {

struct RunResult {
    // False when the program could not be started at all.
    bool started{};
    bool timed_out{};
    int exit_code{-1};
    // Standard output and standard error, interleaved.
    std::string output;
};

// Runs a program without a shell and waits for it, at most timeout_ms.
// A program that runs past the deadline is killed.
[[nodiscard]] RunResult run_process(const std::vector<std::string>& argv, int timeout_ms);

// Starts a program that keeps running after this one exits, in a session of
// its own and with no input or output. False when it could not be started.
[[nodiscard]] bool spawn_detached(const std::vector<std::string>& argv);

// Collects the programs spawn_detached() started that have ended since, so
// none lingers as a defunct process. Only those: no other child is touched.
void reap_detached() noexcept;

} // namespace aoas
