add_library(aoahid_share_core STATIC
  core/accel_model.cpp
  core/config.cpp
  core/geometry.cpp
  core/ini_edit.cpp
  core/keys.cpp
  core/keymap.cpp
  core/session.cpp
  core/tracker.cpp
)
target_include_directories(aoahid_share_core PUBLIC "${PROJECT_SOURCE_DIR}/include")
aoahid_share_warnings(aoahid_share_core)

add_library(aoahid_share_output STATIC
  output/adb.cpp
  output/process.cpp
  output/usb.cpp
)
target_include_directories(aoahid_share_output PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/output")
target_link_libraries(aoahid_share_output PUBLIC aoahid_share_core aoahid_share_proxy
                                                 aoahid::aoahid Threads::Threads)
aoahid_share_warnings(aoahid_share_output)

# --- Input capture: one backend per way of taking the keyboard and mouse ---

add_library(aoahid_share_capture STATIC capture/factory.cpp)
target_include_directories(aoahid_share_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/capture")
target_link_libraries(aoahid_share_capture PUBLIC aoahid_share_core Threads::Threads)
aoahid_share_warnings(aoahid_share_capture)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)

  # Wayland protocol glue, generated from the XML files in protocols/.
  pkg_check_modules(WAYLAND_CLIENT IMPORTED_TARGET wayland-client)
  find_program(WAYLAND_SCANNER wayland-scanner)
  if(WAYLAND_CLIENT_FOUND AND WAYLAND_SCANNER)
    set(protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/protocols")
    file(MAKE_DIRECTORY "${protocol_dir}")
    set(protocol_sources)
    foreach(protocol
        xdg-output-unstable-v1
        xdg-shell
        wlr-layer-shell-unstable-v1
        pointer-constraints-unstable-v1
        relative-pointer-unstable-v1
        keyboard-shortcuts-inhibit-unstable-v1)
      set(xml "${PROJECT_SOURCE_DIR}/protocols/${protocol}.xml")
      add_custom_command(
        OUTPUT "${protocol_dir}/${protocol}-client-protocol.h"
               "${protocol_dir}/${protocol}-protocol.c"
        COMMAND "${WAYLAND_SCANNER}" client-header "${xml}"
                "${protocol_dir}/${protocol}-client-protocol.h"
        COMMAND "${WAYLAND_SCANNER}" private-code "${xml}"
                "${protocol_dir}/${protocol}-protocol.c"
        DEPENDS "${xml}"
        VERBATIM)
      list(APPEND protocol_sources "${protocol_dir}/${protocol}-protocol.c"
                                   "${protocol_dir}/${protocol}-client-protocol.h")
    endforeach()
    add_library(aoahid_share_wayland STATIC ${protocol_sources} capture/wayland_outputs.cpp)
    target_include_directories(aoahid_share_wayland PUBLIC "${protocol_dir}"
                                                           "${CMAKE_CURRENT_SOURCE_DIR}/capture")
    target_link_libraries(aoahid_share_wayland PUBLIC aoahid_share_core PkgConfig::WAYLAND_CLIENT)
    set(AOAHID_SHARE_HAVE_WAYLAND ON)
  endif()

  # evdev needs only kernel headers.
  target_sources(aoahid_share_capture PRIVATE capture/evdev.cpp)
  target_compile_definitions(aoahid_share_capture PRIVATE AOAHID_SHARE_HAVE_EVDEV)

  pkg_check_modules(XCB IMPORTED_TARGET xcb xcb-xfixes xcb-xinput xcb-randr)
  if(XCB_FOUND)
    target_sources(aoahid_share_capture PRIVATE capture/x11.cpp)
    target_link_libraries(aoahid_share_capture PRIVATE PkgConfig::XCB)
    target_compile_definitions(aoahid_share_capture PRIVATE AOAHID_SHARE_HAVE_X11)
    message(STATUS "aoahid_share: x11 backend enabled")
  else()
    message(STATUS "aoahid_share: x11 backend disabled (needs xcb, xcb-xfixes, xcb-xinput, xcb-randr)")
  endif()

  if(AOAHID_SHARE_HAVE_WAYLAND)
    target_sources(aoahid_share_capture PRIVATE capture/layer_shell.cpp)
    target_link_libraries(aoahid_share_capture PRIVATE aoahid_share_wayland)
    target_compile_definitions(aoahid_share_capture PRIVATE AOAHID_SHARE_HAVE_LAYER_SHELL)
    message(STATUS "aoahid_share: layer_shell backend enabled")
  else()
    message(STATUS "aoahid_share: layer_shell backend disabled (needs wayland-client, wayland-scanner)")
  endif()

  pkg_check_modules(DBUS IMPORTED_TARGET dbus-1)
  pkg_check_modules(LIBEI IMPORTED_TARGET libei-1.0)
  if(DBUS_FOUND AND LIBEI_FOUND AND AOAHID_SHARE_HAVE_WAYLAND)
    target_sources(aoahid_share_capture PRIVATE capture/portal.cpp)
    target_link_libraries(aoahid_share_capture PRIVATE PkgConfig::DBUS PkgConfig::LIBEI
                                                       aoahid_share_wayland)
    target_compile_definitions(aoahid_share_capture PRIVATE AOAHID_SHARE_HAVE_PORTAL)
    message(STATUS "aoahid_share: portal backend enabled")
  else()
    message(STATUS "aoahid_share: portal backend disabled (needs dbus-1, libei-1.0, wayland-client)")
  endif()
elseif(WIN32)
  target_sources(aoahid_share_capture PRIVATE capture/windows.cpp)
  target_link_libraries(aoahid_share_capture PRIVATE user32 gdi32)
elseif(APPLE)
  target_sources(aoahid_share_capture PRIVATE capture/macos.cpp)
  target_link_libraries(aoahid_share_capture PRIVATE "-framework ApplicationServices"
                                                     "-framework CoreFoundation")
endif()

# --- Control endpoint, shared by the daemon and its clients ---

add_library(aoahid_share_control STATIC daemon/control.cpp daemon/paths.cpp daemon/store.cpp)
target_include_directories(aoahid_share_control PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
target_link_libraries(aoahid_share_control PUBLIC aoahid_share_core Threads::Threads)
aoahid_share_warnings(aoahid_share_control)

add_library(aoahid_share_daemon_lib STATIC daemon/daemon.cpp daemon/usb_watch.cpp)
target_link_libraries(aoahid_share_daemon_lib PUBLIC aoahid_share_control aoahid_share_capture
                                                     aoahid_share_output)
aoahid_share_warnings(aoahid_share_daemon_lib)

# aoahid_adb_proxy v3.2.1 (MIT), vendored unmodified from
# github.com/nemarpuc/aoahid_adb_proxy. It needs libaoahid 4.0. It is built
# apart from the rest so the project's warning flags do not apply to it.
add_library(aoahid_share_proxy STATIC
  "${PROJECT_SOURCE_DIR}/third_party/aoahid_adb_proxy/aoahid_adb_proxy.cpp")
target_include_directories(aoahid_share_proxy PUBLIC
  "${PROJECT_SOURCE_DIR}/third_party/aoahid_adb_proxy")
target_link_libraries(aoahid_share_proxy PUBLIC aoahid::aoahid Threads::Threads)
if(WIN32)
  target_link_libraries(aoahid_share_proxy PRIVATE ws2_32)
endif()
