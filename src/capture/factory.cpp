// SPDX-License-Identifier: MIT
#include "capture.hpp"

#include <cstdlib>

namespace aoas {

#ifdef AOAHID_SHARE_HAVE_PORTAL
std::unique_ptr<Capture> make_portal_capture();
#endif
#ifdef AOAHID_SHARE_HAVE_LAYER_SHELL
std::unique_ptr<Capture> make_layer_shell_capture();
#endif
#ifdef AOAHID_SHARE_HAVE_X11
std::unique_ptr<Capture> make_x11_capture();
#endif
#ifdef AOAHID_SHARE_HAVE_EVDEV
std::unique_ptr<Capture> make_evdev_capture();
#endif
#ifdef _WIN32
std::unique_ptr<Capture> make_windows_capture();
#endif
#ifdef __APPLE__
std::unique_ptr<Capture> make_macos_capture();
#endif

namespace {

bool has_env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

void add(std::vector<std::unique_ptr<Capture>>& list, const std::string& wanted, const char* name,
         std::unique_ptr<Capture> (*make)()) {
    if (wanted == "auto" || wanted == name)
        list.push_back(make());
}

} // namespace

std::vector<std::unique_ptr<Capture>> capture_candidates(const std::string& backend,
                                                         std::string& error) {
    std::vector<std::unique_ptr<Capture>> list;
    const bool automatic = backend == "auto";
#if defined(_WIN32)
    add(list, backend, "windows", &make_windows_capture);
#elif defined(__APPLE__)
    add(list, backend, "macos", &make_macos_capture);
#else
    // On Wayland an X11 connection only sees XWayland clients, so the X11
    // backend is offered automatically only without a Wayland display.
    const bool wayland = has_env("WAYLAND_DISPLAY");
    const bool x11 = has_env("DISPLAY");
#ifdef AOAHID_SHARE_HAVE_PORTAL
    if (!automatic || wayland)
        add(list, backend, "portal", &make_portal_capture);
#endif
#ifdef AOAHID_SHARE_HAVE_LAYER_SHELL
    if (!automatic || wayland)
        add(list, backend, "layer_shell", &make_layer_shell_capture);
#endif
#ifdef AOAHID_SHARE_HAVE_X11
    if (!automatic || (x11 && !wayland))
        add(list, backend, "x11", &make_x11_capture);
#endif
#ifdef AOAHID_SHARE_HAVE_EVDEV
    // Reading every key from the kernel is a wider permission than a
    // desktop's own capture, so it is never a silent fallback for one.
    if (!automatic || (!wayland && !x11))
        add(list, backend, "evdev", &make_evdev_capture);
#endif
    static_cast<void>(wayland);
    static_cast<void>(x11);
#endif
    static_cast<void>(automatic);
    if (list.empty())
        error = "the backend \"" + backend + "\" is not available in this build or session";
    return list;
}

} // namespace aoas
