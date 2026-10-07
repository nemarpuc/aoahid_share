# Changelog

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
