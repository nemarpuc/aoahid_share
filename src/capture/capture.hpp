// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/geometry.hpp"
#include "aoahid_share/keymap.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aoas {

// A span of one monitor edge the cursor can cross at.
struct Barrier {
    // Nonzero, chosen by the caller.
    uint32_t id{};
    size_t monitor{};
    Side side{Side::right};
    Span span;
};

// Called by a Capture on its own thread.
class CaptureHandler {
  public:
    virtual ~CaptureHandler() = default;
    // The monitor layout changed; barriers have to be set again.
    virtual void monitors_changed() = 0;
    // The cursor pushed `push` pixels outward at edge coordinate t of a
    // barrier. Returning true takes the input: the calls below follow until
    // Capture::release() or lost().
    virtual bool edge_hit(uint32_t barrier, int t, double push, bool button_down) = 0;
    virtual void motion(double dx, double dy) = 0;
    // In wheel notches; positive wheel scrolls up, positive pan right.
    virtual void scroll(double wheel, double pan) = 0;
    virtual void button(unsigned button, bool down) = 0;
    // grabbed is false for a key seen while the PC has the input, which
    // only some backends can report.
    virtual void key(HidKey key, bool down, bool grabbed) = 0;
    // The system ended the grab on its own.
    virtual void lost() = 0;
};

// What a backend needs from the daemon besides the handler.
struct CaptureEnv {
    // The portal's permission token from the last run, and where to keep the
    // next one.
    std::string restore_token;
    std::function<void(const std::string&)> save_restore_token;
};

// One way of watching the screen edge and taking the keyboard and mouse.
// Everything except stop() and post() is called on the thread that calls run().
class Capture {
  public:
    virtual ~Capture() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Whether a key pressed while the PC has the input reaches the handler.
    [[nodiscard]] virtual bool sees_local_keys() const noexcept = 0;
    // Empty on success.
    [[nodiscard]] virtual std::string start(CaptureHandler& handler, const CaptureEnv& env) = 0;
    [[nodiscard]] virtual std::vector<Monitor> monitors() = 0;
    // Replaces every barrier. Empty on success.
    [[nodiscard]] virtual std::string set_barriers(const std::vector<Barrier>& barriers) = 0;
    // Takes the input without an edge hit. False when the backend cannot.
    [[nodiscard]] virtual bool grab() = 0;
    // Gives the input back with the cursor at that position.
    virtual void release(int x, int y) = 0;

    // Runs the event loop until stop().
    virtual void run() = 0;
    virtual void stop() = 0;
    // Runs a function on the event loop's thread.
    virtual void post(std::function<void()> task) = 0;
};

// The backends to try, in order, for a `backend` setting; "auto" lists every
// one that suits this session. Empty with the reason in error when none does.
[[nodiscard]] std::vector<std::unique_ptr<Capture>> capture_candidates(const std::string& backend,
                                                                       std::string& error);

} // namespace aoas
