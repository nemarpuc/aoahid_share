# Sharing the edge with the desktop

Desktops already use screen edges: hot corners, auto-hiding bars, tiling a
window by dragging it to the edge, and, with several monitors, moving to the
next one. aoahid_share is built to leave those alone.

## What it does

1. **Only a segment crosses.** The device is placed against one monitor like
   another monitor would be, and only the part of the edge it touches lets the
   cursor through. The rest of the edge behaves as before.
2. **Monitor-to-monitor edges are never used.** Where two monitors touch, the
   cursor goes to the other monitor ([MATH.md](MATH.md#several-monitors)).
3. **Corners are left out.** The segment stops short of the corners by
   `corner_margin` (5% of the edge by default).
4. **Not while dragging.** With a mouse button held the cursor does not cross
   (`no_cross_while_button`), so dragging a window to the edge still tiles it.
   The `portal` and `layer_shell` backends cannot always see the buttons.
5. **Optional push.** `enter_push` makes the cursor cross only after pushing
   that many pixels against the edge, for a segment that overlaps a desktop
   feature. Not available on `portal`.
6. **Pause.** `aoahid_share pause` (or a hotkey) stops crossing until
   `resume`.

## While the input is on the device

Edge features usually fire when the cursor rests on the edge, and the PC
cursor has to stay somewhere:

| Backend | Where the PC cursor waits |
| --- | --- |
| `x11` | one pixel inside the edge: the barrier keeps it off the last column |
| `portal` | the compositor's business |
| `layer_shell` | locked on the daemon's own surface |
| `windows`, `macos` | parked two pixels inside the edge |

## Known overlaps

| Desktop | Edge feature | With aoahid_share |
| --- | --- | --- |
| KDE Plasma | screen edge actions, edge tiling | On Wayland the barrier has to cover the whole edge ([PLATFORMS.md](PLATFORMS.md#linux-portal)); a crossing outside the segment is handed back at once. Whether KWin's own edge actions still fire on that edge has not been checked. |
| GNOME | hot corner, edge tiling | corners are excluded |
| Xfce, Cinnamon | switching workspace at the edge | keep the segment clear of it, or use `enter_push` |
| Windows | auto-hide taskbar, Snap | the cursor is parked off the edge |
| macOS | hot corners, auto-hide Dock | corners are excluded; the cursor is parked off the edge |

None of the desktops' own settings are read or changed.
