# Changelog

## 0.3.6

- Volume up and volume down join the media keys: `volume_up` and
  `volume_down` in a device's `[keys]`, in `aoahid_share media`, and as
  buttons in the device's Tools tab.

## 0.3.5

- Shortcuts belong to each device. A device's file has a `[keys]` section:
  `switch` (to the device, and back to the computer when it has the input),
  `lock`, `resync` and the five media keys. All start empty.
- `lock` keeps the input on the device whatever edge the cursor reaches, for
  a game that takes the mouse. The same key, a `switch` key, `release`,
  `pause` or closing the device ends it. `status` has `locked` per device.
- A device's media keys go to that device, wherever the input is; several
  devices can each have their own.
- Gone: `toggle_hotkey`, `panic_hotkey`, `pause_hotkey`, `resync_hotkey`, the
  `[media]` hotkeys and `target` in `config.ini`, and `[device] hotkey`. They
  are still read, and dropped; set the keys again in each device's Keys tab.
  `aoahid_share pause`, `resume` and `release` are unchanged.
- `status`: `protocol` is 3; `media_target` is gone.
- Settings window: a Keys tab per device; the computer's page has an HID tab
  and no hotkeys; Tools, then Status and Reports, are the last tabs; Status
  lists what is wrong first; the media-key buttons are in each device's Tools.

## 0.3.4

- CI and release builds link libaoahid 4.2.0 (was 4.1.0). The `find_package`
  minimum (4.0.0) is unchanged.
- The vendored aoahid_adb_proxy is 3.2.1 (only its header's version macros
  differ from 3.2.0).

## 0.3.3

- `scroll_overlap` starts as `restart`: every wheel notch is a swipe of its own
  from the cursor. With `add` (the old start) the finger went further from the
  cursor with every notch, and the view drifted. The setting is labelled "Next
  notch during a swipe" and its help says what each value does.

## 0.3.2

- `[touch] swipe` turns the wheel swipe on and off; it is off by default and
  the settings window shows the swipe settings only while it is on. The swipe
  settings start from values to begin with: 100 device pixels per notch in both
  directions (a starting value, not measured against a mouse wheel), 4 steps
  in 8 ms, put down 1 ms before the first step, lifted 200 ms after the last, a
  notch during a swipe added to it.

## 0.3.0

- Touchscreen mode (`[touch]`, global and per device): registers a touchscreen
  next to the mouse and keyboard. The tap button (`tap_button`, left by
  default) becomes a tap at the tracked cursor position and moving while it is
  held drags. Needs the device's `width`, `height` and `rotation`; the
  position is converted for every rotation and mount (docs/MATH.md).
- The wheel can swipe a second finger (`scroll`, `scroll_pan` in device pixels
  per notch). Every notch is a swipe: put down where the cursor is, moved in
  `scroll_steps` steps (the first `scroll_start_ms` after the finger is down,
  the last `scroll_total_ms` after the first), and lifted `scroll_release_ms`
  after the last step. Android takes a swipe split in steps for a scroll more
  readily than a jump. A notch that comes while a swipe goes on is added to
  what is left of it, or lifts that finger and starts again
  (`scroll_overlap = add | restart`).
- Android drops a touch when a mouse report arrives while it is down, so no
  mouse movement is sent while a finger is down: it is summed and sent in one
  report after the finger's lift. While the tap button is held the cursor
  therefore stays where it was and jumps to the drop point (docs/CONFIG.md).
- The daemon scans for devices when it starts (stopping the adb server first,
  as for any scan that was asked for). A device that has a file and shows up
  in a scan is listed but no longer opened by itself; it is opened with
  `connect` or the Connect button. A device that was connected that way and
  drops out still comes back by itself.
- New `reports SERIAL|NAME` command and a Reports tab on a device's page: the
  calls handed to the device (mouse, keys, media keys, touch contacts), one
  line each, grouped by report. Recorded only while it is being watched.
- The migration of the 0.2 file layout was removed.

## 0.2.1

- The daemon stops a running adb server before every scan that was asked for (a
  reload, `rescan`, `connect`), so a phone the server had claimed is found
  again. It is switched off with `kill_server = false` under `[adb]`.

## 0.2.0

The settings window was rebuilt.

- Black on white, laid out like a device manager: the computer and the devices
  on the left, and under the list the buttons that connect or disconnect the
  one chosen (for the computer: disconnect all, stop the daemon). On the right
  every setting is a row with its name at the left and its control at the right
  edge: real switches for on and off, drop-downs, fields. A device has the tabs
  Device, Placement, Screen, Motion, Status and Tools; the computer has General,
  Motion (the polling rate is here), ADB, Status and Tools.
- Every key of the config files and every key of `aoahid_share status` is shown
  with its unit, default and meaning (as a tip on the name), and every setting
  can be edited except the serial number and the date a device was read, which
  mean nothing when typed. A value the parsers refuse stays on screen with their
  reason and holds Apply back.
- The devices are listed only while the daemon runs; before that the button at
  the bottom left starts it.
- A device that is open can be disconnected even when "In use" has been switched
  off.
- The polling rate (`report_rate_hz`) can be set for each device (its Motion
  tab, or `[motion]` in its file); a device without one follows the computer's.
- `x11`: while the input is on a device the PC's cursor is held where it was
  taken. It was hidden but kept moving over the screen (and its edges and hot
  corners) before.
- Removed background automatic USB device scanning; devices are scanned only
  when asked (`rescan`), with a refresh button placed at the top right of the
  device list.
- Rescan immediately detects and drops physically unplugged devices, and
  automatically connects and applies configuration for previously configured
  devices.
- The device list now displays only currently connected devices by default;
  disconnected configured devices are tucked away under a "Disconnected" group.
- Fix: "Fill from ADB" can now be used on unconfigured devices as long as their
  ADB proxy is running, without requiring the layout to already be ready.
