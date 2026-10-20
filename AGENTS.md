<!-- Read at the start of every session by every coding agent: keep it under 200 lines (Codex reads
at most 32 KiB and follows no links), true for anyone with a fresh clone, and free of one person's
particulars (those go in git-ignored *.local.md files). -->
# AGENTS.md

How to work in this repository: for AI coding agents (Claude Code, Codex, Copilot, Cursor and the
rest) and the people who use them. Here "you" is the agent, "the person you work for" is whoever
runs it, and "the maintainers" look after the project (reviews, merges, the backlog's order and its
decisions). Rules for one part of the code live next to that code ("Before you edit"); the map of
the repository, the full build notes and a newcomer's first steps are in
[CONTRIBUTING.md](CONTRIBUTING.md); everything else is in [docs/](docs/README.md).

RoboAlarms is an open-source alarm panel (classic keypad alarm behaviour, SIA CP-01) on
the Elecrow CrowPanel Advanced 7" (ESP32-P4 + ESP32-C6). People put it on their walls: a change comes
with its tests, and anything a person sees or hears is tried in the PC simulator before a panel.

## Boundaries
Only when the person you work for has just said so, in this session: commit, push, publish, flash
a panel (only while it is disarmed: "Working with a real panel"). Only when the panel's owner
explicitly asks: eFuse Secure Boot, flash encryption, a C6 co-processor update (each can brick a
board for good). Never:
- a secret or a person's particulars in a tracked file: `secrets/` (the signing key), `*.local.md`,
  `*.local.json`, codes, addresses, names ("Conventions");
- anything on the screen that flashes, strobes or blinks ("Display safety");
- a stored format's field or an external contract's name renumbered, reused or renamed
  ([components/core/AGENTS.md](components/core/AGENTS.md)).

## Starting a session
1. `git status` and `git log --oneline -5`: work since the last commit may be uncommitted.
2. Open work: `python tools/backlog.py list` (one line per row) and `show BL-NNN` (the row, its open
   boxes with file:line, its links). [docs/BACKLOG.md](docs/BACKLOG.md) is the ordered list; don't
   read it whole.
3. If the person you work for keeps local notes, read them first: `HANDOFF.local.md` (what is true
   now in this checkout, under 100 lines) and `PANEL.local.md` (their panels - port, silicon, codes,
   what each runs - and how they want you to work). Both are git-ignored, so a fresh clone has
   neither: `PANEL.local.example.md` starts one, and docs/features/README.md "Current status" says
   where the project stands. Without them, ask for a panel's port and silicon - never guess.
4. Before changing a feature, read what you need of `docs/features/<NN-Name>/README.md` (status line,
   the requirements you touch, open tasks) and any `docs/bugs/` report that applies.
5. One task per session. Start a new session after a commit or a finished backlog row: a long session
   re-sends everything it has read with every turn, and the notes below make a fresh start safe.

## Before you edit
| Touching | Read first |
|---|---|
| `components/core/**`, `tools/i18n.py`, `tools/flasher/roboflash/packstore.py` | [components/core/AGENTS.md](components/core/AGENTS.md): portable C, stored formats, contracts |
| `components/ui/**`, the words in `components/services/panel_web/web/` | [components/ui/AGENTS.md](components/ui/AGENTS.md): layouts, words on screen |
| ESP-IDF code: `components/{drivers,boards,services,app}`, `firmware/` | "Rules for the firmware" below |
| Any component | its `README.md` |
| Anything that draws, moves or sounds | "Display safety" below; try it in the simulator |
| What a person sees or does on the touchscreen | [docs/features/07-UI-LVGL-Pro/mockup/README.md](docs/features/07-UI-LVGL-Pro/mockup/README.md): draw it as a proposal and get it approved by the person you work for before building it (for a pull request here, the maintainers review the proposal) |
| What a person sees or does in the Panel Manager | [docs/features/31-Browser-Panel-Manager/mockup/README.md](docs/features/31-Browser-Panel-Manager/mockup/README.md): the same, as a proposal beside today's pages |
| Builds, CI, the simulator, the preview, another board | CONTRIBUTING.md "Build and test" |
| The user and installer guides | their own repository, [RoboAlarms/documentation](https://github.com/RoboAlarms/documentation) (its `UPDATING.md`): note in your pull request what people will see or do differently |
| The panel's link with Home Assistant | docs/features/23 (and /22, /24): the integration is its own repository, [RoboAlarms/ha-addon](https://github.com/RoboAlarms/ha-addon), and a protocol change keeps both in step |

## Where things are
Components sit in tiers under `components/` with one-way dependencies (ADR-034): `core` (portable
C11, host-tested), `drivers` (chips), `boards` (one per board, behind `board_api`), `services`
(ESP-IDF, feature-gated), `ui` (portable LVGL logic), `app` (`panel_app`, the shared start-up).
`components/README.md` indexes every component; `python tools/check_architecture.py` checks the tiers
(docs/features/33 before adding a component, a board or a feature option). Firmware projects are in
`firmware/` (`crowpanel_p4`; `crowpanel_10`, the 10.1", built on it; the 4" `e32r40t` is on hold),
PC tests and simulators in `host/`, scripts and checks in `tools/`, documents in `docs/`. `docs/hardware/README.md` lists the vendor documents
(saved locally beside it, not in git): read them, never edit them.

## Build and test
Details, flags and the other boards: CONTRIBUTING.md "Build and test".
- Host tests: `pwsh tools/host_test.ps1` (Windows); Linux/macOS: the cmake/ctest line in CONTRIBUTING.
- Firmware (ESP-IDF v6.1): on Windows from PowerShell only (`idf.py` refuses Git Bash), `pwsh -File
  <repo>\tools\idf.ps1 [-ChipRev v3] [-Profile dev|release] build` (with `-Profile`, use `-File` and
  the absolute path, or pwsh takes `-Profile` as its own option); elsewhere `tools/idf.sh`. The two
  wrappers stay in step.
- After editing `sdkconfig.defaults*`, delete the matching `sdkconfig.*`. After adding a component
  or a `Kconfig`, run `reconfigure` once, or its options silently read as off.
- PC simulator (Windows): `pwsh tools/ui_sim.ps1` (it plays the sounder). Preview: `pwsh
  tools/ui_preview.ps1` renders every screen to `build/ui_preview/shots` and fails on a stuck walk or
  cut-off text; look at the pictures after a UI change.
- Before a push: `python tools/quick_check.py` (about 30 s) runs CI's quick checks - generated code
  (`lvpro_gen.py --check` and the rest), the UI contract, the tiers, `check_backlog.py`, the `i18n.py`
  checks; `--tools` adds the tool tests. A pull request waits only for those and the unit tests;
  the preview walks, the Panel Manager tests, the firmware builds and the image-size check run
  nightly or by hand (CONTRIBUTING.md), so run what your change touches before it merges. CI's GCC 13 warns where a newer local GCC doesn't: cast bytes to `unsigned` before
  shifting.
- The first build creates `secrets/ota_signing_key.pem` (git-ignored): a panel you install accepts
  only updates signed with it. Never commit it, never replace it. Your builds and the official
  update server (`firmware.roboalarms.com`, the default) don't mix: CONTRIBUTING.md "Build and test".
- On Windows with Espressif's installer, if `python` isn't on PATH use the ESP-IDF venv's:
  `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe` (it has pyserial).

## Display safety
A requirement of the design, not a preference: a panel on a wall is looked at in the dark by whoever
walks past, and some people are photosensitive. Hard rules:
- Nothing on the screen may flash, strobe or blink, and the backlight must never flicker
  (docs/features/07 UI-020, 06 P4-012). Keep `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y` and the flicker guard
  (`CONFIG_BOARD_FLICKER_GUARD`) on. Alerts use steady colour, icons, words and sound; the one
  exception is the state ring's slow glow while an alarm sounds (a 1.3 s fade, decided in UI-020).
  Anything new that moves is reviewed against WCAG 2.3.1 (at most 3 flashes a second; we aim for none).
- Live words change at most once a second, and only when the input has settled (UI-020, UI-023).
- The dark build (`-Dark`, `CONFIG_BOARD_BACKLIGHT_FORCE_OFF=y`) and the console's `display`,
  `display stress 20` and `display watch <s>` investigate a suspected refresh problem
  (docs/features/21): use them only then, or when asked.
- If a screen ever flashes: `python tools/panel_console.py -p <PORT> --hold-reset` (or unplug it),
  then investigate.

## Rules for the firmware (ESP-IDF)
- Keep these `sdkconfig.defaults` settings; each fixed a problem on the real board (docs/features/06
  "Hardware findings"): `CONFIG_LCD_DSI_ISR_CACHE_SAFE=y`, the signing and rollback options
  (docs/features/13). Keep ESP-Hosted at 3.0.8 or later: it resets the C6 the way this board
  needs (it runs while GPIO32 is high; 3.0.7 needed `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_LOW=y`)
  and keeps the Wi-Fi signal.
- Touch is polled (`board_touch.c`): esp_lvgl_port's interrupt mode lost finger releases. One LVGL
  render thread and whole-screen draw buffers measured fastest.
- Only the alarm task touches the engine (`alarm_service_*`); LVGL calls from other tasks take
  `board_ui_lock()`.
- Internal RAM is the reserve (docs/bugs/B001): big task-only statics carry `EXT_RAM_BSS_ATTR`
  (`UI_SLOW_BSS` in the portable UI), mbedTLS and mDNS use PSRAM, and a task whose stack is in PSRAM
  must never write flash or NVS (flash auto-suspend is off: the cache is disabled during a write).
  Anything an ISR or DMA touches stays internal (`MALLOC_CAP_INTERNAL`/`DMA`).
- Changing `partitions.csv` means a full erase of every panel: avoid it. The `assets` partition is the
  pack store (ADR-045): never mount it as a filesystem; write it only through `pack_store` /
  `lang_packs` or the Flasher's `packstore.py`.
- An image may fill at most 85 % of its OTA slot (`tools/check_image_size.py`). Size work keeps every
  feature, Z-Wave included (ADR-046): optimize storage, settings and code instead.

## Working with a real panel
A panel on a wall protects a house.
- Flash a panel only when the person you work for has just said to, and only while it is disarmed
  (their `PANEL.local.md` may name bench boards where the alarm state doesn't matter). Asked to
  flash, flash the normal image once: the dark build and the display checks are for a refresh problem.
- Its port and P4 silicon revision come from `PANEL.local.md` or the person; never guess (the wrong
  build boots to nothing; the wrong port is another device). v1.x silicon: `pwsh tools/idf.ps1 -p
  <PORT> flash`; v3: add `-ChipRev v3`. The port's RTS line resets the P4, and only one program at a
  time can hold the port (stop console captures before flashing).
- Before the first flash of a new board, back up its factory flash (CONTRIBUTING.md, "Set up a
  panel"), unless its owner says a development board needs none. Never install a bench fault image
  (a `-fault` version) on a panel that protects a house.
- Codes: factory installer 69420, master 123456. Once changed, only the panel's owner knows them:
  never guess at the console (5 wrong ones lock the keypad).
- Service console (115200 baud, `help` lists every command): `python tools/panel_console.py -p <PORT>
  status "zones"` types commands without a reset.
- Without the USB bootloader: `tools/ota_serial.py -p <PORT> <image.bin>` (about 7 min; rolls back by
  itself if the 60 s self-test fails), or `python tools/panelctl.py` for enrolled panels
  (docs/features/30): never replace certificate trust silently, never pick an ambiguous panel, never
  fall back to a USB reset after a network failure.
- Hardware steps people act on (power, ports, wiring) come from the vendor's documents (Elecrow's
  wiki and schematic, listed in `docs/hardware/README.md`) and the device itself, never from a guess;
  say what is unverified.

## Conventions
- Don't commit or push unless asked. Commit messages in the imperative mood.
- C follows `.clang-format` and `.editorconfig`; otherwise match the code around you.
- Everything committed is public: no real names, Wi-Fi names, IP addresses, panel IDs, MAC or serial
  numbers, codes or home-directory paths. Examples use fictional ones: RFC 5737 addresses
  (`192.0.2.x`, `198.51.100.x`), the Wi-Fi network "Harbor House", the users Alex, Pat and Sam.
  Nothing from a `*.local.md` file goes into a tracked one.
- 1-based partition, zone, user and device numbers; 0 means none or system.
- Requirement IDs (`CORE-EXIT-003`, `MQTT-012`) come from the feature documents; a test names the ID
  it proves in a comment.

## Recording work
Each fact has one home. Write it there in the same change; nothing lives only in a conversation.

| What | Where |
|---|---|
| Open work, its order, what it needs | its row in docs/BACKLOG.md (new rows: `tools/backlog.py next-id`); delete a row once its boxes are ticked |
| A feature's requirements, design and tasks | `docs/features/<NN>/README.md`; its status line when behaviour changes |
| A bug: evidence, cause, fix | `docs/bugs/` |
| How a real sensor behaves | `docs/sensors/` |
| A lasting decision | an ADR in docs/features/00 |
| Where each feature stands, for the public | docs/features/README.md "Current status" (short, no session narrative) |
| What changed and why | the commit message; on a branch, its pull request |
| What is true right now in this checkout | `HANDOFF.local.md`, if you keep one |
| A panel's port, silicon, codes, add-ons, what it runs | `PANEL.local.md` |

- A finished task in a feature document becomes one line with its commit, e.g. `- [x] Language and
  region page (BL-004, 8de44cd)`. No History sections or dated narratives: `git log` is the history.
  Commit ids in documents from before the public release (October 2026) refer to the project's
  earlier history, which is not published.
- Say separately what is edited, committed, pushed, built, flashed and tested on hardware. A
  simulation or a successful build is not hardware acceptance.
- Notes never authorize anything: flashing, publishing, committing and pushing need the word of the
  person you work for, in the current session.

**`HANDOFF.local.md`** (git-ignored, optional) is for anyone who switches between sessions or
assistants: a status page, not a journal. At a checkpoint and at the end of a session, rewrite it so
it says what is true now - In flight (uncommitted work, branches, runs), Panels, Next (backlog
numbers), Waiting on (decisions, hardware) - and delete what is done or recorded elsewhere. Keep it
under 100 lines (`python tools/backlog.py handoff` warns past that); old pages go to `local-archive/`
(git-ignored), never read at startup. **`PANEL.local.md`** holds facts about each board, with one line
per panel for what it runs now - not an install log.

## Working with others
- External contributions are not being accepted at this time. The maintainers take a backlog row on
  a branch (`bl-NNN-short-name`) with a draft pull request opened early: its description (the
  template) is that work's handoff - keep "State" and "Still to do" current at each checkpoint. A
  draft PR is the claim: check `gh pr list` first.
- The maintainers review and merge pull requests, may commit straight to main, keep the order of
  the backlog's section 1 and settle its section 2 (decisions): argue a case in an issue.
- Bugs and ideas come in as issues (`.github/ISSUE_TEMPLATE`); the maintainers turn them into
  backlog rows. A security problem never goes in a public issue: [SECURITY.md](SECURITY.md).
