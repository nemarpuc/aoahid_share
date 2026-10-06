# Platforms

How the daemon watches the screen edge and takes the keyboard and mouse on each
system, what it needs, and what it cannot take. The backend is chosen at start
(`backend = auto`) and can be fixed in the config.

| Backend | Edge | Takes the input with | Movement read from | Hotkey seen on the PC side |
| --- | --- | --- | --- | --- |
| `x11` | XFixes pointer barrier | core grabs | XInput 2 raw events (unaccelerated) | yes |
| `portal` | InputCapture barrier | the compositor | libei (as the compositor delivers it) | no |
| `layer_shell` | 1-pixel surface | pointer lock, exclusive keyboard | relative-pointer (unaccelerated) | no |
| `evdev` | none | `EVIOCGRAB` | kernel events (unaccelerated) | yes |
| `windows` | low-level mouse hook | low-level hooks | hook positions (accelerated) | yes |
| `macos` | event tap | event tap | event deltas (accelerated) | yes |

## Motion

Each backend reports the mouse's movement as its source gives it (the table
above), and that is what is sent to the device, times the sensitivity. Nothing
converts it to the PC's screen or matches it to the device's: a backend that
reports the mouse's own counts (`x11`, `layer_shell`, `evdev`) and one that
reports the pixels of the PC's cursor (`portal`: libei's "relative x movement
in logical pixels or mm, depending on the device type", `ei_event_pointer_get_dx`
in `libei.h`; `windows`, `macos`) therefore feel different at the same
sensitivity, as they would for a USB mouse plugged into the device.

Where the hotkey is not seen on the PC side, the input moves to the device by
crossing the edge, or by binding `aoahid_share enter` to a key in the desktop's
own shortcut settings. The hotkeys always work while the input is on the device.

Automatic choice on Linux: with `WAYLAND_DISPLAY`, `portal` then `layer_shell`;
with only `DISPLAY`, `x11`; with neither, `evdev`.

## Linux: `x11`

- A pointer barrier is placed on the segment, one pixel inside the edge
  ([XFixes 5](https://www.x.org/releases/current/doc/fixesproto/fixesproto.txt));
  an `XI_BarrierHit` event ([XInput 2.3](https://www.x.org/releases/current/doc/inputproto/XI2proto.txt))
  reports the push.
- The grabs are on a window of the daemon's own, not the root window: the X
  server withholds raw events from a client whose grab is on the root.
- The pointer grab confines the cursor to that window, which is moved to where
  the cursor is and made one pixel wide, so while the input is on the device
  the cursor stays where it was taken (no edges, no hot corners). The raw events
  still carry every movement of the mouse.
- If another client already holds a grab (an open menu, a screen locker), the
  cursor stays on the PC.
- Not usable under XWayland: an X client there does not see other Wayland
  clients' input.
- Cannot take: VT switching (`Ctrl+Alt+F1`...).

## Linux: `portal`

- Uses [`org.freedesktop.portal.InputCapture`](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.InputCapture.html)
  and [libei](https://gitlab.freedesktop.org/libinput/libei). KDE Plasma 6.1
  added it ([6.1 beta announcement](https://kde.org/announcements/plasma/6/6.0.90/));
  GNOME's Mutter 45 added input capture through libei
  ([Phoronix on 45 beta](https://www.phoronix.com/news/GNOME-Shell-Mutter-45-Beta)),
  which is a news report, not GNOME's own release notes.
- The desktop shows a permission dialog. With interface version 2 the grant is
  remembered through a restore token kept in the daemon's state file.
- **KDE only accepts a barrier that covers a whole screen edge**
  ([`checkAndMakeBarrier`](https://invent.kde.org/plasma/xdg-desktop-portal-kde/-/blob/master/src/inputcapturebarrier.cpp)),
  and none on an edge that touches another screen anywhere. The daemon first
  asks for the segment; when that is refused it asks for the whole edge and
  hands the cursor straight back when it crosses outside the segment.
- The compositor decides when a capture starts, so `aoahid_share enter` and
  the toggle hotkey cannot start one; `enter_push` has no effect either.
- Which keys the compositor keeps for itself during a capture has not been
  examined.

## Linux: `layer_shell`

- Needs `wlr-layer-shell`, `pointer-constraints`, `relative-pointer` and
  `xdg-output`; uses `keyboard-shortcuts-inhibit` when present. Compositors
  built on wlroots are expected to have them; which versions of Sway, Hyprland
  and Wayfire do has not been checked. Only started on KDE Plasma so far.
- A transparent one-pixel surface lies on the segment. When the pointer enters
  it, the pointer is locked and the surface takes the keyboard. Nothing is
  forwarded until the compositor confirms both.
- The cursor returns on the edge itself, at the matching height.
- The compositor may keep shortcuts it marks as working while inhibited.

## Linux: `evdev`

- Reads `/dev/input/event*` and needs read access to them, usually membership
  of the `input` group. That lets the daemon read every key typed on the
  machine; it is never chosen automatically inside a desktop session.
- It cannot see the cursor, so there is no edge: use the hotkey or
  `aoahid_share enter`.
- The devices are taken only at a moment when no key is down, and nothing is
  forwarded before that.

## Windows

- `WH_MOUSE_LL` and `WH_KEYBOARD_LL` hooks. While the input is on the device
  every event is swallowed and the cursor stays parked two pixels inside the
  edge.
- The phone needs a driver libusb can open (WinUSB); see
  [libaoahid PORTING.md](https://github.com/nemarpuc/libaoahid/blob/main/docs/PORTING.md#windows).
- Cannot take: `Ctrl+Alt+Del` (the secure attention sequence; a low-level
  hook cannot remove it, see
  [Disabling Shortcut Keys in Games](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/disabling-shortcut-keys-in-games)).
  `Win+L`, the UAC desktop and elevated windows are expected to be out of
  reach as well; that has not been checked against a primary source or on
  hardware.
- Built in CI; not yet run on real hardware.

## macOS

- A session event tap. It needs the Accessibility permission
  (System Settings > Privacy & Security > Accessibility).
- While the input is on the device the cursor is detached from the mouse and
  hidden.
- Cannot take: keys typed while Secure Input is on, which keeps keyboard
  events from intercepting processes
  ([TN2150](https://developer.apple.com/library/archive/technotes/tn2150/_index.html)),
  as in password fields.
- `aoahid_share buttons` is not implemented here.
- Built in CI; not yet run on real hardware.

## Synthetic input

Input that another program injects is not forwarded to the device where the
system marks it: on Windows by the hooks' injected flag, on macOS by the
event's source. The macOS mark can be forged by a program that is itself
allowed to post input events, so treat it as protection against automation
tools reaching the phone by accident, not against a hostile program running
as you. The Linux backends read below the level where clients inject.

## Keys

Every backend maps the physical key to a HID usage; none sends characters.
The layout is whatever Android has set for the physical keyboard. Media keys go
to a separate Consumer Control device.

## Mouse buttons

The number of buttons is part of the HID descriptor the device is opened with
(`[mouse] buttons`, 1 to 8, default 5). `aoahid_share buttons` reports how many
the connected mice have; it is only run on request, and the value is yours to
write into the config.
