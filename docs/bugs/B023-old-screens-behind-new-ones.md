# B023 · Pieces of earlier screens flicker behind the current one (display safety)

**Status: reopened by hardware observation.** On the 7-inch panel, the screensaver's clock and
parts of its background remain visible with flicker after repeated USB-flash/restart cycles,
then clear after a few minutes. The 10.1-inch panel has not shown the same symptom in this test;
its longer screensaver delay makes that an incomplete comparison. The existing buffer clearing,
hardware reset and orderly software-shutdown fixes are still present. Earlier diagnostics found
correct frame-buffer contents, but this recurrence needs a fresh diagnostic capture; normal
frame timing alone does not establish that the glass is clean. Backlog: BL-160.

The follow-up diagnostic build was visually clean after a USB flash and after an orderly
console restart, including a screensaver wake. The new capture found no differing pixels,
unpainted areas or late frames; the orderly restart logged the sleep handler before reset.
The shared display source is identical to the original repository's implementation of
`f313f1e`; no missing 10-inch-only fix was found. This does not close the recurrence:
diagnostic rendering adds work and changes timing, and the normal build still needs the
same visual comparison. No new display-driver fix is claimed.

## Symptom

On a bench panel (CrowPanel Advanced 7" V1.1, P4 v1.3, a v1 dev build with B022's fix), soon
after it was flashed:
- turning the chime on from the home screen brings up the code keypad, and parts of the
  screensaver (where its clock was) show behind the keypad;
- right after a restart, the screen flickers.

Reported later the same day: it happens after the panel is powered on and goes away once it has
been on for a while. A few seconds without power bring it back, and the first start after power
flickers.

Like B022, this breaks the display-safety rule (docs/features/07 UI-020, 06 P4-012). The flicker
guard can't catch it: the frames arrive on time.

## Measured on board 3 (2026-10-04)

A v1 dev build with the display self-check (`CONFIG_BOARD_DISPLAY_DIAG`, console `dispdiag`):
- Every area LVGL flushed matched the frame buffer once its copy was done (0 differing pixels).
- Every flushed pixel had been painted: before each redraw the draw buffer was filled with a
  marker, and no marker pixel reached a flush.
- This held from each start, across 11 console restarts and 6 resets from the USB port.
- It also held through a person's steps, driven by remote taps (panelctl `screen`): the saver,
  a tap to wake it, then the chime's code keypad over the home screen. The frame capture showed
  that keypad on a clean backdrop.
- 0 late frames and no DMA underrun.
- The DSI host flags errors only while the panel takes its init commands. One LP1 contention flag
  shows at each start (the software reset and the init table, code we didn't change). There were
  none while video ran.

So the frame buffer holds only what LVGL drew. A code review earlier the same day found no way
for old draw-buffer bytes to reach the screen either. What lingers is on the glass.

## Cause (most likely)

From the EK79007 datasheet and the board's schematic; not measured on the glass. An IPS panel
keeps a faint ghost of an earlier picture when it is left undriven or powered down out of order
(a residual DC charge in the liquid crystal). Normal driving clears it over minutes, which is the
"goes away after a while". A power cut or a restart leaves a new one, which is "a few seconds off
brings it back".

Our start and stop broke the panel's order in four places:
- **The panel was never properly reset.** Its reset line GRB (GPIO41) has a 2K pull-up on the
  P4's LDO4 rail. It rose as soon as `board_power_init` turned LDO4 on, before the DSI lanes
  existed, and the datasheet wants them idle (LP11) first. A P4 restart didn't reset the panel at
  all: nothing drove GPIO41, and only a software reset command followed. On the P4, `esp_restart`
  and a panic reset only the CPUs: GPIO states and the backlight's PWM survive them.
- **Nothing put the panel to sleep before a restart.** The last picture stayed on the glass with
  no video until the next start, with the backlight still on.
- **The backlight came on over whatever the glass showed.**
  - At start-up it came on at the UI's first tick.
  - After a panic it stayed lit at its last level until the display started again.
  - Waking from dark, it came on at once over the previous screen, until the new one was drawn:
    the saver's clock behind the keypad.
- **Installs leave the panel undriven.** The Flasher or a USB flash holds the P4 in download mode
  with the panel in reset and its supplies up, the last picture frozen on it (B022's demo).

## Fix

In `components/boards/board_crowpanel_p4` and the CrowPanel's `app_main`:
- **A hardware reset on every start.** Once the DSI bus is up and its lanes are idle,
  `board_display_init` drives GRB low for 2 ms (the datasheet asks for at least 1 ms), then
  high, and waits 60 ms before the first command.
- **The backlight off first.** `app_main` sets up the backlight's PWM at 0 before anything else,
  so a panic restart no longer leaves it lit until the display starts, even when start-up stops
  early to wait for recovery (its stored settings unreadable).
- **The backlight comes on from dark only over a picture drawn since it was asked for.**
  - It waits for a redraw that began after the request, plus 3 more frames.
  - With nothing to redraw (no refresh under way and none due), it comes on after 100 ms; it
    never waits more than 500 ms.
  - It never comes on before 200 ms of video.
  - A change of level while lit applies at once.
  - The board does this itself (`board_display_reveal_request`, `board_backlight_reveal`), so the
    UI's start-up and its wake from dark both get it. Each request has a number, so an answer to
    an older one can't light a newer one early.
- **The panel goes to sleep before every planned restart** (an update, a restore, the console's
  restart). A shutdown handler turns the backlight off for good, takes the UI lock (waiting up
  to a second), writes black into the frame buffer and lets 3 frames go out. It then sends
  sleep-in (0x10) and keeps video running for 120 ms while the panel discharges. Last, it pulses
  GRB low for 2 ms and leaves it high. A panic skips shutdown handlers.
- **GRB is low as briefly as it can be.** A restart keeps GPIO states, so a panic during one of
  the two 2 ms pulses leaves GRB low; if that restart also rolls back to a firmware from before
  this fix, which never drives GRB, the panel stays in reset until the power is cut. The pulses
  are busy-waited, not slept, to keep that window small; it is accepted.
- **The frame capture reads memory, not the cache.** `board_dev_frame` invalidates the cache
  first: DMA2D writes the frame buffer past the CPU's cache, so a capture could show an older
  picture.

Tried on board 3 (2026-10-04, the fix with the self-check):

| Start | Panel slept first | First picture / backlight on | Frame buffer | Link while video ran |
|---|---|---|---|---|
| 6 console restarts | yes, every time, 0.34 s before the reset | 1.57 s / 2.80-2.90 s | 0 differing, 0 unpainted | no errors, 0 late frames |
| 3 resets from the USB port | no (no software runs) | 1.57 s / 2.86-2.88 s | 0 differing, 0 unpainted | no errors, 0 late frames |

The backlight first came on about 1.3 s after the first picture: that is when the UI first asks
for it, so the new wait adds nothing at start-up.

The backlight off and back on through the console (`dispdiag dark <ms> [redraw]`): with a
redraw, it came on 240-315 ms after the request, once the redraw and 3 frames were out. With
nothing to redraw, it came on after 100-111 ms. That build carries the self-check, whose marker
fill and compare make each redraw slower than in a normal build.

## Left

1. **A person's eyes, on board 3.** Do these, watching the first minute each time:
   - power it off for a few seconds and on;
   - wake it from the saver and turn the chime on;
   - restart it from the console.

   `dispdiag` says whether the frame buffer was right meanwhile.
   - Still a ghost after a power cut, but not after a restart: it is the power cut (2).
   - A one-frame flash when screens change: tearing (5).
2. **A power cut** can't put the panel to sleep from firmware. A hardware path could: a supply
   supervisor that blanks the panel before 5 V falls, or the battery.
3. **The Flasher**, before it resets a panel running our firmware into download mode, could ask
   the console to put the panel to sleep. That needs a console command for it.
4. **The saver's clock** sits still, high-contrast, at one spot for hours, and that can leave its
   own ghost on IPS. Moving it slowly would spread the wear. This changes the touchscreen, so it
   needs a mockup first.
5. **Tearing**, only if a flash on screen changes remains. There is one DPI frame buffer with no
   tearing avoidance, so during a full-screen copy the rows already scanned show the previous
   picture for one frame. The fix is two frame buffers swapped at vertical sync (1.2 MB more
   PSRAM).

The self-check stays available for investigation builds: `CONFIG_BOARD_DISPLAY_DIAG=y`, console
`dispdiag`. It slows each redraw by its marker fill and compare.
