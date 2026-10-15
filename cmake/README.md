# cmake - shared build logic

One copy of the build logic the firmware projects and the PC builds share (docs/features/33,
principle 5: one source of truth per list).

| File | Used by | What |
|---|---|---|
| `alarm_firmware.cmake` | `firmware/<board>/CMakeLists.txt` | a firmware project's settings from `ALARM_BOARD`: its components (through `alarm_idf.cmake`), the version from `VERSION`, the signed project name per profile (`alarm_<board>[-dev\|-release]`) and `alarm_check_build_profile()`, which also sets up the language packs and stamps each linked image with its own build date (`tools/stamp_app_desc.py`) |
| `alarm_idf.cmake` | `alarm_firmware.cmake` | where the components are: the tier folders as `EXTRA_COMPONENT_DIRS`, and of the boards only `board_api` and the project's own board (another board's registry packages would otherwise be solved for the wrong chip) |
| `alarm_host.cmake` | `host/`, `host/ui_preview`, `host/ui_sim` | the portable components as PC libraries, the warning and sanitizer flags, `alarm_add_test()`, discovery of every component's `test/` folder |
| `alarm_opus.cmake` | `host/` (the simulator and preview next) | micro-opus 0.4.1 at its pinned commit, with libopus, for the voice's decoder `voice_codec` (docs/features/08 voice-first-build.md "The Opus decoder") |
| `alarm_lvgl.cmake` | preview, simulator | LVGL 9.5 from the firmware's managed copy or a clone of the pinned tag |
| `alarm_ui.cmake` | preview, simulator | the UI library from a layout (`components/ui/alarm_ui/layouts/<WxH>/alarm_ui`) and `components/ui/alarm_ui/sources.cmake` |

A component's own source list stays in its `CMakeLists.txt` (or a `sources.cmake` beside it
when a PC build needs it too, like the UI's).
