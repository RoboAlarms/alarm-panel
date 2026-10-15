# host - the panel on a PC

Three PC builds of the same code the firmware runs, so behaviour is proven and seen before a
panel is flashed (docs/features/21, docs/features/33 principle 7). All of them build the portable
components the same way, through `cmake/alarm_host.cmake`.

| Build | What | Windows | Linux / macOS |
|---|---|---|---|
| `CMakeLists.txt` | every portable component and every component's host tests (Unity), run with CTest | `pwsh tools/host_test.ps1` | `cmake -S host -B build/host -G Ninja && cmake --build build/host && ctest --test-dir build/host --output-on-failure` |
| `ui_preview/` | the real engine, event log and UI rendered headless; a scripted walk through every screen, saved as PNGs to `build/ui_preview/shots`, failing when a check reports the wrong thing | `pwsh tools/ui_preview.ps1` | `cmake -S host/ui_preview -B build/ui_preview -G Ninja && cmake --build build/ui_preview && ./build/ui_preview/ui_preview build/ui_preview/shots` |
| `ui_sim/` | the interactive simulator: the panel's screen in a window, buttons for the states worth trying, the sounder through the PC's speakers | `pwsh tools/ui_sim.ps1` | not yet (LVGL's Windows backend) |

## Where things are
- **Tests live with their components**: `components/<tier>/<name>/test/`. This folder only
  holds the runner (`CMakeLists.txt`) and Unity (`third_party/unity`). The engine harness
  shared by several suites is `components/core/alarm_core/test/sim.c`.
- `ui_preview/host_panel.c` is the simulated world (a panel backend with zones, network,
  storage...) that both the preview and the simulator drive; `ui_preview/main.c` is the
  scripted walk; `ui_sim/sim_main.c` the simulator's buttons.
- LVGL comes from the firmware's managed copy or a clone of the pinned tag
  (`cmake/alarm_lvgl.cmake`); the UI library from `cmake/alarm_ui.cmake`, which reads the
  same source list as the firmware (`components/ui/alarm_ui/sources.cmake`).

## The voice
`ui_preview/host_panel.c` wires the panel's voice policy (`components/core/annunciator`) to the
simulated engine as `panel_app.c` and `panel_voice` wire it on the board: engine events, sounds,
the bell and every snapshot go in, the zone words come from the configuration, and Preview in
Settings goes through `ann_preview`. It speaks the tracked en-US voice, packed unsigned at build
time (`ui_preview/voice_pack.cmake`; nothing to sign, no model or panel needed).
- **The simulator** (`ui_sim/sim_audio.c`) plays tones through `snd_play_tracked` with tickets and
  renders speech with `voice_render` into a small ring that the audio thread mixes with
  `snd_render_pcm` - the panel's mixer, so speech sits 6 dB under the entry beeps and the fast
  exit beeps, and the slow exit beeps wait while speech is due or playing (AUD-034). Mute and volume work as for tones. Turn Announcements on in Settings >
  Screen and sound, then try a chime on a zone with a spoken name, arming, a disarm or a walk test.
- **The preview** has no sound. A virtual sound card runs the same mixer in 10 ms blocks of virtual
  time (`snd_render_pcm`, with the current tones, user volume, the speech level under the entry and
  fast exit beeps, the exit beeps' wait (AUD-034) and power saving) and records each utterance to `build/ui_preview/voice/<n>-<kind>.wav`: 16-bit **stereo**
  32 kHz, what the speaker would get while it played - tones playing meanwhile included, scaled by
  the volume. An utterance that completes gets a WAV of at least its words (plus the mixer's silent
  warm-up block); one that a later alarm, identity change or power saving cancels stops at the
  cancellation and the trace says `voice: cancelled` (`voice: completed` otherwise). One trace line
  per utterance, `voice: <kind> <zone> <clip ids> <total ms>`, in virtual time, so a run is
  repeatable. `ui_preview/voice_walk.c` (the end of the walk, and alone as `voice_walk_smoke` in
  CTest) asserts the trace and renders the same words through a copy of the mixer to compare the
  WAV sample for sample: a chime then the zone's words, arming over the exit pattern, a disarm, a
  walk test, volume changes, an alarm cutting speech, nothing with Announcements off. It drives
  the power policy (`host_panel_power_sample`, `power_saving_update`'s 15 s rules), sensor
  reassignment and rebind (the spoken-name approval is lost, a mapping-only change keeps it) and
  long clock advances with automations off, and plays a chime through the simulator's own audio core
  without a sound card.
- **Another voice:** `-VoicePack <file>` (`tools/ui_sim.ps1`, `tools/ui_preview.ps1`), an unsigned
  pack from `python tools/voice/voice.py pack --unsigned`; it sets `ALARM_VOICE_PACK`, which any
  of these programs reads. Without it, the tracked voice.
- Power saving follows `host_panel_power_sample()` (mains by default); the preview's battery screens
  (`host_panel_set_power`) only draw. Not simulated: the panel's speaker and I2S timing.

## Options
- `-DALARM_SANITIZE=ON` (host tests, GCC/Clang on Linux/macOS): address and undefined-behaviour
  sanitizers on the components and the tests. CI runs with it.

## Board profiles
`-Board <board>` (`tools/ui_preview.ps1`, `tools/ui_sim.ps1`; CMake `-DALARM_BOARD=<board>`)
builds the preview and the simulator the way that board's firmware is built, from the same
manifest (`firmware/<board>/sdkconfig.defaults`, read by `cmake/alarm_board.cmake`): its UI
layout and screen size, its DPI, the optional pages its feature options keep, and every feature
option as `CONFIG_<option>=0|1` (`ui_preview/host_features.h`): the simulated panel offers only
what that firmware's backend would, and the one scripted walk (`ui_preview/main.c`) visits only
the pages the board has - the same walk runs on every layout. When the manifest gives the UI an
LVGL budget (`CONFIG_ALARM_UI_LVGL_BUDGET_KB`), LVGL gets its own allocator on the PC and the
walk ends by reporting the most it ever held against that budget (halved: the PC's pointers
are twice as wide); `ALARM_PREVIEW_MEM=1` prints the heap at every screenshot. The screenshots'
PNG encoder never uses LVGL's heap (`ui_preview/shot_png.c`). The default is `crowpanel_p4`
(build folders `build/ui_preview`, `build/ui_sim`); other boards build in
`build/ui_preview-<board>` and `build/ui_sim-<board>`.
