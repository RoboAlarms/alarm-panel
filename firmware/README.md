# firmware - one ESP-IDF project per board

Each folder is a thin ESP-IDF project that composes a firmware for one board from the
components under `components/` (docs/features/33). Everything that another board could use
belongs in a component, not here.

| Project | Board | Status |
|---|---|---|
| [`crowpanel_p4`](crowpanel_p4) | Elecrow CrowPanel Advanced 7" (ESP32-P4 + C6), docs/features/06 | in daily use |
| [`crowpanel_10`](crowpanel_10) | Elecrow CrowPanel Advanced 10.1" V1.2 (the 7"'s main board), built on `crowpanel_p4` | runs on a development board; not offered by the Flasher yet |
| [`e32r40t`](e32r40t) | LCDWiki E32R40T 4" (classic ESP32, no PSRAM), docs/features/19 | headless: builds (docs/features/33 step 10); display, touch and sound are docs/features/19 Phase 2; never flashed |
| [`radios/h2_rcp`](radios/h2_rcp) | not a panel: the ESP32-H2 Zigbee radio in the 7"'s wireless socket, docs/features/20 | builds; its own README says how it goes on the module (over its USB-C); not yet on a module |

## A project's folder
```
<board>/
  CMakeLists.txt          sets ALARM_BOARD (this folder's name) and includes
                          cmake/alarm_firmware.cmake: the component tiers with only this
                          board's component, the version from VERSION, the signed project
                          name alarm_<board>[-dev|-release]
  main/                   app_main.c: the board's hooks and a call to panel_app_run() (the
                          start-up every board shares, components/app/panel_app), and what
                          only this board has (the 7": dev_adapter.c), idf_component.yml
  partitions.csv          the flash layout (changing it wipes a panel: avoid)
  sdkconfig.defaults      the board's settings and its manifest: every feature option stated
                          explicitly (tools/check_boards.py checks it; the PC simulator
                          profile reads the same lines)
  sdkconfig.defaults.*    fragments stacked by the build wrapper: dev, release, dark, silicon
  dependencies.lock       the registry components' exact versions (committed)
```

### A board built on another
Boards on one main board share their board support through a family library
(`components/boards/<name>_family`, named by `ALARM_BOARD_FAMILY`), and one board's firmware can
be built on another's with `set(ALARM_BOARD_BASE <project>)` in its `CMakeLists.txt`. Then:
- every `sdkconfig.defaults*` fragment is read from the base first and the project's own after
  it (`cmake/alarm_firmware.cmake`), so the project holds only what differs: `crowpanel_10`'s
  `sdkconfig.defaults` names the 7"'s `partitions.csv` and its own update channel;
- `main/` builds the base's sources: `crowpanel_10/main/CMakeLists.txt` includes
  `crowpanel_p4/main/crowpanel_p4_app.cmake` and names its own board component;
- the wrappers, `tools/check_boards.py` and the PC simulator's profile read the base's manifest,
  then the project's. A base cannot have a base of its own.

On Windows a P4 project folder's path can be at most 63 characters: ESP-Hosted's deepest file
lies 196 characters below it and Windows stops at 259. `firmware\crowpanel_10` and
`firmware\crowpanel_p4` fit in a checkout whose own path has at most 41 characters; a git
worktree under `.claude\worktrees` is too deep to build them.

## Building
Always through the wrappers, which activate ESP-IDF v6.1, create the signing key on first use
and keep each profile in its own build folder:
`pwsh tools/idf.ps1 [-Project <board>] [-Profile dev|release] [-ChipRev v1|v3] build` on
Windows (from PowerShell), `tools/idf.sh [--project <board>] ...` elsewhere. See CONTRIBUTING.md,
"Build and test", and AGENTS.md, "Working with a real panel", before building for or flashing a board.

Build folders: `crowpanel_p4` and the boards built on it build one image per P4 silicon
family (`build_v1`, `build_v3`, `sdkconfig.v1`/`.v3`; `-ChipRev` applies to them alone); every
other board builds in `build` with `sdkconfig`. A profile, version or dark build adds its
suffix: `build_v1_dev`, `build_dev`, `build_0.6.1-test`. The 7" image carries the ESP32-H2 Zigbee radio's stack
(docs/features/20, ADR-047); its 170 MB library is fetched by every P4 build.

`python tools/check_image_size.py firmware/<board>/<build folder> --ram` reports the image
against its OTA slot (at most 85 %, docs/features/19 CYD-013) and the static RAM; CI
(`.github/workflows/firmware.yml`) builds every board and runs it, signing with a key made
for the run (CI images are never for a panel).
