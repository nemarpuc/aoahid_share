# Configuration and commands

## Files

Everything is kept in one `settings` folder next to the programs
(`aoahid_share`, `aoahid_share_daemon`, `aoahid_share_gui`), so a copy of the
folder is a portable install:

```text
settings/
  config.ini          computer-wide settings and defaults
  state.ini           what the daemon remembers (the portal's restore token, ...)
  daemon.log          the daemon's log, started over on each run
  device/
    <serial>.ini      one Android device each: placement, what was read from it
```

Where that folder cannot be used, the same four things are kept in the
user's own folder instead: `$XDG_CONFIG_HOME/aoahid_share` (else
`~/.config/aoahid_share`) on Linux, `%APPDATA%\aoahid_share` on Windows,
`~/Library/Application Support/aoahid_share` on macOS. That is the case when the
programs' folder cannot be written to (an installed copy under `/usr/bin` or
`Program Files`), when the programs are inside an application bundle, and, on
Linux and macOS, when `settings` is not this user's own or can be written to by
others (the settings name the adb program to run, so a folder others can change
is not trusted). `AOAHID_SHARE_SETTINGS_DIR` names another folder for all of it.

`aoahid_share paths` prints them. Each configured Android device has a file of
its own under `device/<serial>.ini`, created when you connect it.

The first time the `settings` folder is made, what an older layout kept
elsewhere (`config.ini` and `devices/*.ini` in the user's folder above,
`state.ini` in `$XDG_STATE_HOME/aoahid_share` or `~/.local/state/aoahid_share`
on Linux and `%LOCALAPPDATA%\aoahid_share` on Windows) is copied into it; the old
files are left where they were. A `config.ini` with `[device.N]` sections is
split into files under `device/`, and the original is kept beside it as
`config.ini.0.2` (the split stops if that copy cannot be made).

The daemon writes `daemon.log`, started over on each run: which backend it captures with, each device connecting or dropping,
and every error. If no backend can start yet (the desktop's permission dialog
is unanswered, for instance) the daemon keeps running, `status` shows
`state=starting` with the reason, and it tries again every 15 seconds.

## `[daemon]`

| Key | Default | |
| --- | --- | --- |
| `backend` | `auto` | `auto`, `x11`, `portal`, `layer_shell`, `evdev`, `windows`, `macos` |
| `toggle_hotkey` | `ctrl+alt+s` | move the input to the last used device and back |
| `panic_hotkey` | `ctrl+alt+shift+escape` | always back to the computer |
| `pause_hotkey` | empty | stop and restart crossing |
| `resync_hotkey` | empty | take the corner reference on the next crossing |

A hotkey is modifiers (`ctrl`, `shift`, `alt`, `meta`) and one key: a letter, a
digit, `f1`..`f24`, `escape`, `space`, `tab`, `enter`, `backspace`, `insert`,
`delete`, `home`, `end`, `pageup`, `pagedown`, `up`, `down`, `left`, `right`,
`pause`, `scrolllock`. Empty turns it off.

## `[mouse]` and `[keyboard]`

| Key | Default | |
| --- | --- | --- |
| `buttons` | `5` | 1 to 8; `aoahid_share buttons` reports what your mice have |
| `button_map` | empty | HID button for PC button 1, 2, ...; `1,3,2` swaps right and middle |
| `[keyboard] enabled` | `true` | `false` shares the mouse only |

## `[media]`

A hotkey per media key, empty for none: `previous`, `play_pause`, `next`,
`brightness_up`, `brightness_down`. The settings window has a button for
each under Actions on the Computer page (Tools tab).

`target` chooses which device receives media keys: a serial or name targets
that device wherever the input is. Empty (or `active`) sends to the device
that has the input, else the last used device or the first connected one.

```ini
[media]
target = Galaxy Tab S11
play_pause = ctrl+alt+p
next = ctrl+alt+right
```

A media hotkey works wherever the input is. On a backend that
cannot see the keyboard while the input is on the PC (`local_hotkeys=no` in
`status`: `portal`, `layer_shell`), bind a desktop shortcut to
`aoahid_share media play_pause` instead.

The keyboard's own volume and media keys need no setting. While the input is
on a device they go to that device and the computer does not act on them,
on every backend but `macos`, which does not capture those keys.

## `[motion]`

Global defaults for pointer motion. Every setting here can be overridden per
device in its own file.

| Key | Default | |
| --- | --- | --- |
| `sensitivity` | `1.0` | 0.05 to 20; the mouse's movement is sent as it comes, times this |
| `sensitivity_y` | empty | vertical only; empty follows `sensitivity` |
| `scroll_sensitivity` | `1.0` | |
| `report_rate_hz` | `max` (a device may set its own) | `max` sends a report as soon as the previous one completes and sums what moved meanwhile; `every` sends each movement and scroll step as a report of its own, in order, with no limit; 10 to 8000 paces motion at that many reports a second |
| `entry` | `parked` | how the cursor enters a device: `parked` keeps the device's cursor in its bottom right corner while the input is on the PC, where it is all but hidden, and enters from there; `aligned` moves it from where it was left; `corner` takes the corner reference every time. `park = true\|false` and `entry = align\|corner` are also read |
| `resync_width` | `48` | pixels of uncertainty above which the corner reference is taken again |
| `enter_push` | `0` | pixels to push against the edge before crossing |
| `return_push` | empty | pixels past the device's edge before returning; empty is 1, or 16 when the gain is uncertain |
| `no_cross_while_button` | `true` | |

[MATH.md](MATH.md) defines each of these.

## `[adb]`

| Key | Default | |
| --- | --- | --- |
| `kill_server` | `true` | stop a running adb server that holds the ADB interface of a phone whose proxy is to start (once, then the proxy is tried again), and once more on exit if a proxy was served, so that adb sees the phone over USB again |
| `path` | empty | the adb program, run only by "fill"; empty uses `PATH` |
| `first_port` | `6555` | where the proxy ports start (1024 to 65535, not 5555 to 5585) |

The daemon does not use adb on its own. A device's ADB proxy is **off** until
its `proxy = true` is set (its page in the settings window, or its file); only
then is the phone's ADB interface served on `127.0.0.1:<port>`, and you run
`adb connect` yourself. A device's port is `first_port` plus the number of
devices before it (files are taken in the order of their names) unless its
file gives `port`; set `port` to keep one fixed.

`fill` (the "Fill from ADB" button, or `aoahid_share fill DEVICE`) is the one
use of adb: with the proxy on, it runs `adb connect` to it, reads the
resolution, rotation, physical size (`dumpsys display`), pointer speed and
whether pointer acceleration is on, and writes them, with the gain they give
([MATH.md](MATH.md#gain)), into the device's file. It does not close or
reopen any device. If the phone is waiting for the "Allow USB debugging"
answer (a new computer), it waits up to 15 seconds for it, trying again; after
that, fill again. A value the phone does not give is left as it was and is
named in the answer. `enabled`, `connect`, `tune` and `restore_accel`
(the daemon's own use of adb, which it no longer has) are read and ignored, and
are not written back.

On Linux with a glibc older than 2.34, or musl, an adb server started by `adb
connect` can keep the proxy's port bound after the daemon exits (the
descriptor cannot be closed on `exec` there).

## Per-device files (`device/<serial>.ini`)

Each configured device has its own file named after its USB serial number
(letters, digits, `-`, `_`, `.`; at most 64). A key left out takes the global
value where there is one, else is not set: the daemon then says in the
device's status what is missing, and does not guess it. A value is written as
a number, and its line is left out when it was never given (`auto` or an empty
value, as older files wrote them, still read as not set).

### `[device]`

| Key | Default | |
| --- | --- | --- |
| `serial` | empty | USB serial number; an older profile that named none is filed as `unassigned N.ini` until a device takes it |
| `name` | empty | display name, at most 48 bytes, without `=`, `;`, `#`; it may not be another device's serial |
| `enabled` | `true` | `false` keeps the device configured but left alone |
| `hotkey` | empty | takes the input straight to this device |

`accessory_fallback` (switching the phone to accessory mode when it refused HID)
is no longer supported; a file that has it still reads.

### `[placement]`

| Key | Default | |
| --- | --- | --- |
| `beside` | empty | serial of another device to sit next to; empty sits beside a monitor |
| `monitor` | empty | monitor name as `status` shows it; empty is the first monitor |
| `side` | `right` | `left`, `right`, `top`, `bottom` |
| `start` | empty | where the device starts, in PC pixels from the start of that monitor's or neighbour's edge; not set, it is centred |
| `length` | empty | its length in PC pixels; not set, it follows the real sizes |
| `corner_margin` | `5%` | dead zone at each corner of a monitor's edge (not used beside a device) |
| `monitor_diagonal_inch` | empty | for a monitor that reports no physical size |
| `rotation` | empty | `0`, `90`, `180`, `270`: how the device is turned now. It is not followed: set it again (or fill) when the device is turned |
| `turn_input` | `0` | how far the picture on the device is turned for the person looking at it (see [MATH.md](MATH.md)); leave at `0` unless the device is mounted turned |

### `[detected]`

What was read through the device's ADB proxy, or entered by hand:

| Key | Default | |
| --- | --- | --- |
| `width`, `height` | empty | resolution in the natural orientation |
| `diagonal_inch` | empty | screen diagonal |
| `pointer_speed` | `0` | Android's pointer speed setting (-7 to 7) |
| `density` | empty | the density Android lays out with (`wm density`), in dpi |
| `accel` | empty | `off` or `on`: whether the device's pointer acceleration is on |
| `gain` | empty | pixels the cursor moves per count at low speed, typed or measured ([MATH.md](MATH.md#gain)); empty: calculated from `pointer_speed` and `density` |
| `read_at` | empty | date the values were filled from adb |

**What Android's pointer speed changes.** Only `gain`, which follows from it:
`gain = density / 320 * 0.64 * S[speed] / 10 * 3.19` ([MATH.md](MATH.md#gain)),
with `density` the value above. The `gain` key is for a gain you typed or
measured: left empty, the daemon uses that calculated value, so changing
`pointer_speed` (or filling after changing it on the device) changes the gain
in use at once. "Fill from ADB" empties `gain` for that reason. The settings
window shows the calculation with its numbers (Screen tab). A `gain` in a file that is only what the speed and density give
is read as empty. The counts sent do not depend on any of this (the mouse's
movement is sent as it comes); the gain only tells the daemon where Android's
cursor is, for the return and the entry. Turn the speed up on the device and
the cursor moves further for the same mouse movement, as it would for any USB
mouse.

The device is not used until `width`, `height`, `rotation`, `diagonal_inch`,
`accel` and a gain (typed, or `pointer_speed` with `density`) are set, by hand or
by `fill`; its status names those that are not.

### `[motion]` (device override)

Any of `sensitivity`, `sensitivity_y`, `scroll_sensitivity`, `entry`,
`resync_width`, `enter_push`, `return_push`, `no_cross_while_button`,
`report_rate_hz` from the global `[motion]` section: a device is paced by its own
`report_rate_hz` when its file has one, else by the computer's. `gain_min`,
`gain_max`, `match_physical` and `counts_per_pixel` are read and ignored.

### `[keyboard]` (device override)

`enabled = true|false`: whether this device receives the keyboard.

### `[adb]` (device override)

| Key | Default | |
| --- | --- | --- |
| `proxy` | `false` | serve this device's ADB interface on its port |
| `port` | next free | local TCP port for this device's ADB proxy |

## Status

`aoahid_share status` prints `key=value` lines. It is answered from a snapshot
the daemon keeps, so asking never holds up the input. Per device the keys are
`device.<serial>.<key>`:

| Key | Meaning |
| --- | --- |
| `name`, `state`, `status`, `fill`, `reading` | what it is called; `ready`, `error` or `absent`; why it is not ready; the last "fill"; a fill is under way |
| `serial`, `proxy` | its serial; the proxy port or `off` |
| `wanted`, `plugged` | `yes` when `connect` was asked for it and has not been undone; `yes` when the last scan saw it plugged in |
| `size`, `rotation`, `density` | the values in use: pixels, degrees, Android pixels per millimetre |
| `mode`, `gain` | `exact` or `curve`; pixels per count at low speed |
| `segment`, `beside` | monitor, side, crossing span, device span (PC pixels) |
| `position` | the tracked cursor range in the display's view space: x low, x high, y low, y high |
| `reports`, `merged`, `depth` | USB reports sent; inputs summed into a report that was already waiting; reports waiting now |
| `samples`, `queue_us`, `total_us` | over the last 256 reports (microseconds, as typical, 99 of 100, worst since opened): from an input reaching the daemon to the start of its USB call, and to the report's completion. The device's own delay is not included |
| `realtime` | whether the sender thread got a real-time priority (needs `RLIMIT_RTPRIO`) |

Top level: `protocol` (the version of this format), `state` (`pc`, `android`,
`paused`, `starting`), `active`, `last_used`, `backend`, `media_target`,
`local_hotkeys`, `monitor.N`, `new.<serial>`, `config_error`, `capture_error`,
`files_version`.

## Commands

`aoahid_share <command>` talks to the daemon over a Unix socket in
`$XDG_RUNTIME_DIR` (a named pipe on Windows) that only the same user can open.

| Command | |
| --- | --- |
| `status` | state, backend, monitors, configured devices and newly detected devices; see [Status](#status) |
| `connect SERIAL\|NAME` | open a device. One that has a file is opened as it is; a plugged-in device that has none is given a file first (nothing is read from it). Nothing else is touched. The daemon opens no device by itself: not when it starts, not on `reload`. A device that was connected and drops out (unplugged, a glitch) is opened again when it comes back, until `disconnect` or `forget` |
| `disconnect SERIAL\|NAME` | close a device and leave it closed |
| `fill SERIAL\|NAME` | read a connected device through its ADB proxy (which has to be on) and write what it says into its file; nothing is closed or reopened |
| `forget SERIAL\|NAME` | remove a device and its configuration file |
| `enter [SERIAL\|NAME]` | move the input to that device (or the last used device) |
| `release` | bring input back to the computer |
| `pause`, `resume` | pause or resume cursor crossing |
| `resync` | re-reference the corner reference on the next crossing |
| `rescan` | look for devices now and wait for the result. The daemon also looks by itself when a USB device comes or goes (Linux), and every 30 seconds as a safety net; elsewhere every 2 seconds. A scan sends a request to every USB device, so none is made while a device has the input |
| `reload` | read the configuration files again; a device that is open stays open unless how it was opened changed (the mouse buttons, the keyboard, the proxy, its port), and then it is opened again; no other device is opened |
| `media KEY [SERIAL\|NAME]` | send a media key (`previous`, `play_pause`, `next`, `brightness_up`, `brightness_down`) |
| `probe SERIAL\|NAME corner` | send the device's cursor to its top left corner |
| `probe SERIAL\|NAME move DX DY` | move the cursor by raw counts, up to 30000 |
| `export [--device SERIAL\|NAME] FILE` | export the whole configuration, or one device, as one text file |
| `import FILE` | check FILE, take it as the configuration or as one device, and reload; this computer's `[adb]` section is kept |
| `buttons` | count the buttons of connected mice |
| `quit` | stop the daemon |
| `paths` | show the settings, device, state and control paths |
