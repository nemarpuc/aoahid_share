# aoahid_share

Share your computer's mouse and keyboard with an Android phone or tablet over a
USB cable. Move the cursor past the edge of your screen and it continues on the
Android device; push it back and it returns.

It needs **nothing installed on the Android device** and **no USB debugging**:
the input reaches Android as an ordinary USB mouse and keyboard through
[Android Open Accessory 2.0](https://source.android.com/docs/core/interaction/accessories/aoa2)
HID, using [libaoahid](https://github.com/nemarpuc/libaoahid). With USB
debugging on, it also keeps `adb` working, and can fill in the device's size and
gain from it.

Linux (X11 and Wayland), Windows and macOS.

> **Status:** used end to end with a Galaxy Tab S11
> (Android 16) on Arch Linux, KDE Plasma (Wayland). The Windows and macOS
> backends build in CI but have not been run on real hardware yet. See
> [What has been tested](#what-has-been-tested).

## How it works

```
your mouse/keyboard ─▶ aoahid_share_daemon ─▶ USB (AOA 2.0 HID) ─▶ Android
                              ▲
              aoahid_share (CLI) / aoahid_share_gui
```

- `aoahid_share_daemon` runs in the background with no window. It watches one
  span of one screen edge, takes the keyboard and mouse when the cursor pushes
  through it, and sends them to the device.
- Android's cursor cannot be read back, so the daemon keeps track of where it
  must be: it pushes the cursor into a corner once (Android stops it there),
  then counts every movement it sends. [docs/MATH.md](docs/MATH.md) has the
  arithmetic.
- When the tracked position reaches the shared edge and you keep pushing, the
  input comes back to the computer at the matching height.

## Quick start (Linux)

1. Install [libaoahid](https://github.com/nemarpuc/libaoahid/releases) 4.0 or
   newer and its udev rule, so your user can open the phone
   ([libaoahid PORTING.md](https://github.com/nemarpuc/libaoahid/blob/main/docs/PORTING.md#linux)).
2. Build:
   ```sh
   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libaoahid
   cmake --build build
   ```
3. Plug the phone in and start the daemon:
   ```sh
   build/out/aoahid_share_daemon &
   build/out/aoahid_share status
   ```
   The daemon opens no device by itself. Plugged in devices appear as newly
   detected. Run `aoahid_share connect <serial>` (or the Connect button in
   `aoahid_share_gui`) to give one a file and open it; a device that already has
   a file is opened the same way, by name or serial. Then turn its ADB proxy on
   and run `aoahid_share fill <serial>` to read its size and pointer speed, or
   enter them by hand ([docs/CONFIG.md](docs/CONFIG.md)).
4. Push the cursor through the right edge of your screen.

On GNOME and KDE the desktop asks once for permission to capture input.

`aoahid_share_gui` does the same from a window, in black and white, laid out
like a device manager: the computer is listed on the left, and once the daemon
runs (the button at the bottom left starts it) the devices too. The buttons
under the list search for devices again and connect or disconnect the one that
is chosen (for the computer: disconnect every device, stop the daemon). Apply
and Discard changes are at the bottom right. On the right each setting
is a row, its name at the left and its control (a switch, a drop-down, a field)
at the right edge. A device has the tabs Device (name, in use, keyboard, ADB
proxy), Placement (drag it around your monitors, or say which side of what it
sits on), Screen, Motion, Status (every key the daemon reports) and Tools; the
computer has General, Motion (with the polling rate), ADB, Status and Tools.
It only edits the config and talks to the daemon; closing it changes nothing.
Nothing starts the daemon but you: run it yourself, or press its button in the
window.

### Getting back

| | |
| --- | --- |
| Push the cursor back through the shared edge | returns to the computer |
| `Ctrl+Alt+S` | toggles between the computer and the device |
| `Ctrl+Alt+Shift+Esc` | always returns to the computer |
| `aoahid_share release` | the same, from a terminal |

If the device is unplugged or stops answering, the input returns on its own.

## Delay

Each phone has its own USB context, sender thread and lock, and scanning for
phones has its own context too, so a scan or a slow phone never holds up a
report. The daemon measures its own part of the delay for every phone and
shows it in the settings window (the Status tab) and in `aoahid_share status`: from an
input reaching the daemon to its USB report being finished. On the Galaxy Tab
S11 that was 0.34 ms typical and 0.56 ms for 99 reports of 100 (0.9 to 2.5 ms
worst), over 600 reports. The device's own delay, the USB frame and Android's
input stack are not in that number and cannot be removed from here.

## adb keeps working

While the daemon holds the phone over USB, `adb` cannot open it itself. Switch
a device's **ADB proxy** on (its Device tab, or `proxy = true` in its
file) and the daemon serves the phone's ADB interface on `127.0.0.1:<port>`
(6555 for the first device, through the vendored
[aoahid_adb_proxy](https://github.com/nemarpuc/aoahid_adb_proxy)); you then
run `adb connect 127.0.0.1:6555` yourself and the phone shows up in
`adb devices`. The proxy is off until you switch it on, and nothing else in
the daemon uses adb on its own: no `adb connect`, no reading of the phone, no
change of its settings.

**Fill from ADB** (the Screen card of a device's page, or `aoahid_share fill
DEVICE`) is the one place adb is run: with the proxy on, it connects, reads
the resolution, rotation, physical size, pointer speed and whether pointer
acceleration is on, and writes them (and the gain they give) into the
device's file. It does not close or reopen anything. Every value can also be
typed in by hand; a value that is not set is said to be missing, never
guessed. The one time the daemon touches adb's server is to stop it when it
holds the phone's ADB interface that a proxy is to take (`kill_server`, in
`[adb]`), and once more on exit if a proxy was served.

## Configuration

Everything is kept in a `settings` folder next to the programs, so a copy of
the folder is a portable install (an installed copy that cannot write there
uses the user's own folder; [docs/CONFIG.md](docs/CONFIG.md#files)). The global
`settings/config.ini` holds computer-wide settings and defaults; each Android
device has its own file, `settings/device/<serial>.ini`, with placement, what
was read from the device, and optional motion overrides.
[docs/CONFIG.md](docs/CONFIG.md) lists every key.

`settings/config.ini`:
```ini
[motion]
sensitivity = 1.0      ; the mouse's movement is sent as it comes, times this
report_rate_hz = max   ; or every | 10..8000

[adb]
kill_server = true     ; stop adb's server if it holds a phone a proxy is to take
```

`settings/device/<serial>.ini`:
```ini
[device]
serial = R52X...
name = Galaxy Tab S11

[placement]
monitor = HDMI-A-2     ; which monitor the device sits next to (or beside = another serial)
side = right           ; left | right | top | bottom
rotation = 0           ; set by hand, or filled from adb

[detected]
width = 1600
height = 2560
diagonal_inch = 11
gain = 2.169
accel = off

[adb]
proxy = true           ; serve its ADB interface; off until switched on
```

Several devices have a file each under `settings/device/`, and can be arranged beside
a monitor edge or beside each other so the cursor crosses directly from device
to device.

## Platforms

| Platform | Backend | Crosses at the edge | Notes |
| --- | --- | --- | --- |
| Linux, X11 (any desktop) | `x11` | yes | |
| Linux, Wayland: GNOME, KDE Plasma 6.1+ | `portal` | yes | asks for permission once |
| Linux, Wayland: Sway, Hyprland, other wlroots | `layer_shell` | yes | |
| Linux, anything else | `evdev` | hotkey only | needs read access to `/dev/input` |
| Windows | `windows` | yes | the phone needs a WinUSB driver |
| macOS | `macos` | yes | needs the Accessibility permission |

Details, permissions and limits: [docs/PLATFORMS.md](docs/PLATFORMS.md). How it
shares a screen edge with your desktop's own edge features:
[docs/COEXISTENCE.md](docs/COEXISTENCE.md).

## What has been tested

- **Real device, Arch Linux:** Galaxy Tab S11, Android 16. Corner reference,
  tracking, return, re-entry at a new height, sensitivity, the ADB proxy and the
  setup from adb were compared against the position Android itself reports and
  matched to within a pixel.
- **`x11` backend:** crossing, movement, keys, buttons, return and hotkeys on a
  virtual X server.
- **`portal` backend:** KDE Plasma 6.7 (Wayland), with a physical mouse:
  crossing onto the tablet and back works.
- **`layer_shell` backend:** starts on KDE Plasma 6.7 without protocol errors.
  Not run on a wlroots compositor.
- **The settings window:** drawn on a virtual X server against a fake
  USB device: its rows, a refused value, Apply, and a config file
  that cannot be read. Not run with a real phone, on Wayland, on Windows or on
  macOS.
- **`evdev`, `windows`, `macos` backends:** built only.
- **Other phones:** none yet. How far one count moves the cursor depends on the
  Android version and vendor; see [docs/MATH.md](docs/MATH.md#gain).

## Building

Requires CMake 3.22, a C++20 compiler and libaoahid 4.0 or newer. On Linux each
backend is built when its libraries are found:

| Backend | Packages (Debian names) |
| --- | --- |
| `x11` | `libxcb1-dev libxcb-xfixes0-dev libxcb-xinput-dev libxcb-randr0-dev` |
| `portal` | `libdbus-1-dev libei-dev libwayland-dev` |
| `layer_shell` | `libwayland-dev` |
| `evdev` | none |
| the settings window | `libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxkbcommon-dev libwayland-dev` |

Dear ImGui and GLFW are downloaded at configure time for the settings window;
`-DAOAHID_SHARE_BUILD_GUI=OFF` leaves it out.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/libaoahid
cmake --build build
ctest --test-dir build
```

## License

MIT. Vendored: [aoahid_adb_proxy](third_party/licenses/aoahid_adb_proxy-LICENSE.txt)
(MIT), [doctest](tests/third_party/doctest.h) (MIT), and the Wayland protocol
descriptions under `protocols/` (their own notices, in each file).
