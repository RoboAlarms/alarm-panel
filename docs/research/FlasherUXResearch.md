# RoboAlarms Flasher: UX research and redesign proposal

Research date: 2026-09-24. Reviewed: `tools/flasher` at `c7b70ff` (the page in `roboflash/web/`,
the engine's local API, `boards.json`), docs/features/34 and ADR-039, the Flasher walked through
against `run.py --mock` in the Browser pane, the website and
the docs' styles (the manual's [`src/styles/custom.css`](https://github.com/RoboAlarms/documentation/blob/main/src/styles/custom.css) in RoboAlarms/documentation, `docs/BRAND.md`), plus the comparable tools and
guidance under "Sources". Brief: *the Flasher is the first thing people use to get the
alarm going; make it easy for someone without hardware experience, with advanced options to
change pin layouts and choose the devices they have connected; it must not look thrown
together*; and, mid-way: *follow the styles and colours of the docs and the website*.

**Built 2026-09-24:** the Flasher's page follows this
proposal (docs/features/34 FLS-023..025), with every J7 place working in the firmware instead of
"planned" and a "Reset to factory defaults"; the prototype stays as the design reference.

Deliverables: this document and a clickable prototype,
[`docs/features/34-Flasher-and-Bench-Tool/prototype/guided-setup.html`](../features/34-Flasher-and-Bench-Tool/prototype/guided-setup.html)
(one file, no engine: open it in a browser; a strip at the top switches between a new panel, a
panel already running RoboAlarms and nothing plugged in, and between today's firmware and a
later one that supports the planned wiring places). **Nothing in `tools/flasher` was
changed, no test was run for it, no board was touched and nobody outside the project has tried
the prototype yet.**

## Answer and recommendation

The Flasher's engine is sound (ADR-039 stays: Python, esptool, pywebview with an HTML face) and
its safety rules are good. What makes it feel thrown together is the page on top: it is
organised around **serial ports and tools**, while a first-time user thinks in **"my panel"
and "set it up"**. Recommended:

1. **Two layers** (progressive disclosure, never deeper than two): a **guided setup** by
   default and **Advanced tools** one click away holding today's tabs (ports, console, hardware
   diagnostics, backups, firmware file, logs).
2. The guided setup is **five steps with a visible step list**: Connect, Check the panel,
   Connected parts, Install (or Update), Test and finish. One decision per screen, buttons
   named after what they do ("Check this panel", "Install RoboAlarms 0.6.0").
3. **Connected parts is a step of its own**: pick what is wired to the panel from plain
   descriptions, see the wiring drawn from the board's pin map, change a pin only among the
   pins the firmware accepts (with the reason for the others), apply everything with one
   restart after installing, then test each part on the last screen.
4. **The website's and the docs' identity**: their tokens (teal `#007b7d` on light, `#94eff0`
   on dark, the neutral teal-greys), the Segoe UI system type, 7 px buttons, 4 px tags, 12 px
   hairline cards, the uppercase eyebrow with its short rule, drawn SVG pictures of the board
   and parts. Still no motion (FLS-017), following the OS light/dark setting with a toggle.
5. **Trust at the door**: sign the Windows program before sharing it publicly (the SmartScreen
   "Windows protected your PC" page is the very first screen today), detect a missing CH340
   driver, and speak of "RoboAlarms 0.6.0, built in" rather than bundles and images.
6. **Try it on people first**: put the new page in front of 3-5 people who have never flashed a
   board (the first hardware tester is ideal) using `--mock`, before building Phase B.

## Who uses it

| Person | Wants | How often | Needs from the Flasher |
|---|---|---|---|
| Homeowner, first panel (primary) | A working alarm; reassurance nothing breaks | Once per panel | A path with no jargon, pictures of what to plug where, one obvious next step, clear "done, now do this" |
| Tinkerer or installer (e.g. setting up a second panel at another site) | Receiver on J7 IO3, a clock module, later sirens and relays | At install, and when adding parts | Which parts, their wiring, pins that can move and why others can't, a live test |
| Developer | Builds, bundles, console, raw captures, restores | Often | Everything today's tabs do, without the guided steps in the way |

## What works today (keep it)

- The safety model: look without restarting, identify before writing, back up before the first
  erase, refuse while armed or updating, the esptool allow-list, typed confirmations for the
  destructive paths, "unplugging mid-write does no harm, just retry".
- Determinate progress with named steps, no spinners, no blinking; dark mode; plain words in
  most messages; the recorded reasons for detection ("Looks like a CrowPanel 7": ESP32-P4...").
- `--mock` scenarios, which make a page redesign cheap to check.

## What makes it feel thrown together

Heuristic review of the current page (`web/app.js`, `app.css`) against the mock, severity for a
first-time user.

| # | Finding | Why it hurts | Severity | Fix in the proposal |
|---|---|---|---|---|
| 1 | The first thing on screen is a list of **COM ports** with bridge chips and USB IDs ("COM5 CH340 USB 1A86:7523") | The system's model, not the user's; nobody new knows which is "theirs" | High | Connect step: "Found a panel"; port details under Technical details |
| 2 | **"Identify chip"** and "Look again" are the header's buttons; a new panel needs Identify before anything else works | A jargon button the user must discover; its consequence (a restart) is in a tooltip | High | "Check this panel" as the step's primary button, with the restart explained on the screen |
| 3 | **No sense of steps**: Flash, Console, 433 MHz receiver and Clock module tabs are all there before anything is installed | Suggests all four are needed now; no "where am I, what's next" | High | Step list; Console and diagnostics live in Advanced tools |
| 4 | The primary action ("Install") sits at the bottom of the second card, below the fold at the default 1200x800 when nothing is identified; header buttons outrank it | The eye lands on secondary actions first | Medium | One primary button per screen, always in the same place (footer, right) |
| 5 | Developer words on the primary path: "image", "silicon up to v1.99", "dev, legacy", `alarm_crowpanel_p4-dev`, "(docs/features/19)", a full temp-folder path, "Firmware bundle" | Reads as an internal tool | Medium | Say / don't say table below; details one click away |
| 6 | Failures arrive as **toasts that vanish** (4.5-9 s), including "The Flasher engine is not answering" | Easy to miss; nothing to act on | Medium | Errors stay on the screen they belong to, with what to do and a retry |
| 7 | Hardware setup is split across two tabs mixing status, diagnostics, wiring and settings; each change (receiver type, pin) is its own modal and its own restart; the CC1101 and clock wiring are tables only; toggles are styled as primary buttons | Hard to see "what do I have and is it right"; restarts pile up | Medium | Connected parts step, one apply, a drawn wiring picture, radio cards for choices |
| 8 | Look: the Panel Manager's older palette (`#067060`, navy bar `#13293a`) instead of the website's and docs' teal; system font but Unicode marks (✓ ✕ •) for icons; same-weight stacked cards; a 290 px device column for what is usually one device | Doesn't match the site that sent people here; nothing guides the eye | Medium | Website/docs tokens, Lucide line icons, drawn pictures, a centred 760 px column |
| 9 | Unsigned program: Windows shows SmartScreen's "Windows protected your PC" and the README coaches "More info, Run anyway" | The first impression is a security warning | High once public | Code signing (below) |
| 10 | Browser behaviour inside the window (file input, context menu, reload) | Small tells that it is a web page | Low | Native file dialog through pywebview; check context menu and reload are off |

## What comparable tools do

| Tool | Pattern | Lesson for us |
|---|---|---|
| Raspberry Pi Imager 2.0 (Nov 2025) | Redesigned as a step-by-step wizard: Device, OS, Storage, Customisation, Writing, Done; each step gets the whole window with descriptions and validation; customisation (hostname, Wi-Fi, users) became part of the flow; keyboard-navigable and screen-reader labelled; aimed at first-time users | The closest model: device first, choices before writing, a Done step, room to explain |
| ESP Web Tools (ESPHome, WLED) | Detects the chip and picks the build; a device already running the firmware gets a dashboard (Logs & Console, Change Wi-Fi, Visit device) instead of the installer; Wi-Fi set over serial (Improv) right after flashing; whether a new install erases is the maker's choice (`new_install_prompt_erase`) | A panel that already runs RoboAlarms gets "Your panel" and Update, not the install path; set things up right after the install while the cable is in |
| Meshtastic Web Flasher (observed 2026-09-24) | Three numbered cards, Device, Firmware, Flash, each with a large picture and one button; the device picker is a grid of product pictures filtered by maker and chip, with Auto-detect for devices already running Meshtastic; the flash step asks update or wipe | Pictures beat names for "which board is this"; auto-detect first, choosing by hand second |
| ExpressLRS Configurator | Target chosen by category then model; options such as binding phrase, regulatory domain and Wi-Fi set before flashing | Settings that belong to the hardware are chosen before writing, next to the target |
| Tasmota | A "template" is the device's pin map as data; the Configure Module page assigns a component to each GPIO from dropdowns; templates are shared per device | Presets first (a kit, a wiring), raw pins behind Advanced |
| WLED | A Pin Info page (available vs system-reserved); issue #4070: pins reserved by a usermod still appeared free in the dropdowns and saving was silently rejected | Offer only pins the firmware accepts and say why the others are not offered |
| TI SysConfig | Conflicts flagged at configuration time, both uses of a pin marked, before anything is built | Validate before applying; show "no pin is used twice" |
| NN/g guidance | Wizards suit infrequent, unfamiliar tasks: show the steps, keep them in order, label buttons with the next step, reuse the last choices. Progressive disclosure: frequent needs up front, at most two levels, clear labels for the advanced layer | Two layers, five steps, descriptive buttons, remember choices per board |

## The proposed design

### Principles
1. The panel is the subject, not the port. Ports, chips and USB IDs are details.
2. One decision per screen, one primary button, always bottom right, named for its action.
3. Nothing happens to a board without words on the screen saying what and why (a restart, an
   erase), and the existing gates stay exactly as they are.
4. Pictures for anything physical: the board, each part, each wire.
5. Advanced is one click away and never more than one level deep.
6. Calm: no motion, determinate progress, words that change at most once a second (FLS-017).

### Guided setup, screen by screen

| Step | The screen says | Primary button | Behind it (today's engine) |
|---|---|---|---|
| 1 Connect | "Plug the panel into this computer" with a drawing, the power-supply warning, live "Found a panel"; "Nothing showing up?" opens the cable/port/program/driver checklist | "Next: check the panel" (disabled until something is found) | port list and classification (FLS-001), passive probe (FLS-003) |
| 2 Check the panel | New panel: why it must restart for a moment, "Is this a 3D printer? don't check it". After: "This is a CrowPanel Advanced 7"", a picture, "Ready for RoboAlarms 0.6.0", how the Flasher knows; "Not this board? Choose it yourself" opens a picture grid; Technical details (port, bridge, chip, MAC, build) | "Check this panel", then "Next: connected parts" | identify (FLS-004), matching and override (FLS-005/006) |
| 3 Connected parts | Wireless sensors (None / Receiver module / CC1101 board / Not sure) with the wiring drawn and the pin picker; Battery-backed clock on/off with its wiring; later parts shown as "later"; "Pin assignments (advanced)" with the conflict check | "Next: review", "Skip for now" | `boards.json` today; the panel's own description later |
| 4 Install / Update | Summary with "Change" links, "What happens" with durations, backup on by default, "Safe to retry", the power warning | "Install RoboAlarms 0.6.0" / "Update to 0.6.0" | install/update jobs (FLS-007/008), unchanged confirmations |
| 4 (running) | Overall percentage and time left, the named steps with their own bars, "Keep the panel plugged in" | "Stop..." (secondary) | job events |
| 5 Test and finish | "RoboAlarms is running on your panel", what to do on the touchscreen (Wi-Fi, change the factory codes), then a live test per chosen part (sensors heard, clock compared) | "Finish" (and "Advanced tools") | `rf status`/listen, `rtc` (FLS-014, FLS-022) |

A panel that already runs RoboAlarms follows the same five steps with different words: Check
says "Your panel runs RoboAlarms 0.5.2, disarmed and idle: safe to update", Connected parts
shows what the panel reports, Install becomes Update ("codes, users, zones, Wi-Fi and history
stay"), and "Install fresh (erases settings)" moves to Advanced tools > Software.

### Advanced tools
Panels and ports (every port, including Bluetooth and built-in ones), Console (today's, with
quick commands), Hardware (receiver mode and pin, raw capture, clock set, all pin assignments),
Backups (restore, back up now, open folder), Software (built-in version, open another file,
choose the build by hand, install fresh), Logs. The existing workflows keep every current
function; they are grouped instead of sitting beside the novice path.

### Connected parts and pins
- **Parts, not pins, first.** Each part card says what it does for the home ("Keeps the date
  and time through power cuts") before its name ("a DS3231 board"). Hardware names are right in
  the Flasher: it is the installer's tool (the touchscreen and Panel Manager keep speaking of
  capabilities only).
- **The wiring picture is the pin map.** The prototype draws the J7 header from the same rows
  as `boards.json`, wires routed between the rows so a wire never crosses a pin it doesn't end
  on (VCC must never look as if it lands on 5V), colour-coded VCC/GND/DATA with a legend.
- **Pins only where the firmware allows them**, the others dimmed with the reason: IO25 is the
  chip's USB data line; IO27/IO28 become the radio socket's serial pair on V1.2 boards
  (`board_pins.h`). Power pins say what they are for. Fixed pins (the CC1101's socket, the
  clock's I2C) say "fixed by the board".
- **One apply.** The choices are held until the install finishes, then applied in one go
  (`rf module`, `rf pin`, one restart) and read back; on a running panel, "Apply and restart"
  with a summary of what changes ("Receiver DATA: socket pad IO10 -> J7 IO3").
- **Presets later**: a named wiring ("D-FLIFE 433 MHz kit on J7 IO3") the way Tasmota shares
  templates, so the next person with that kit picks one card.

### Wiring, from the add-on research (2026-09-24)
[AddOnWiringResearch.md](AddOnWiringResearch.md) (where the DS3231, the CC1101 and the ESP32-H2
module go on a CrowPanel 7" V1.1) is folded into the prototype's Connected parts step:
- **Four parts, each with its places**: the 433 MHz receiver module (radio socket pad IO10, or
  any J7 terminal), the CC1101 (radio socket; J7 IO52/IO51/IO50/IO49 + IO5), the clock (the I2C
  connector J13; J7 IO27 + IO28 on its own bus, V1.1 only) and the Zigbee module (the socket
  only). Every place carries **"Works with 0.6.0" or "Planned"**: the J7 places for the CC1101
  and the clock, and anything Zigbee, need the firmware work the research lists. A planned place
  is shown with its wiring (to plan by) but cannot be applied; the prototype's "Firmware" switch
  shows the same screen once a later firmware supports it.
- **Suggested layouts** from the research: receiver module + clock (J7 IO3, J13: seven wired
  zones left, works today), CC1101 + clock (socket, J13: all eight zones, works today), Zigbee +
  CC1101 + clock (socket, J7, J7: three zones left, planned, the research's recommendation).
- **The rules become a live pin check**: the Zigbee module takes every socket pad, so the
  socket places and "Not sure" (whose search probes the socket) are refused with it fitted; a
  planned place on today's firmware is refused; "Next" stays disabled, with the reason, until
  the check passes. The check also counts **wired zones left on J7** (a zone is a contact from a
  terminal to GND), which is the real cost of each placement.
- **Drawings for J7 and J13, a table for the socket**: J7 and J13 are drawn from their printed
  labels, with every wire of the part in view (seven for a CC1101) and the pins other parts hold
  marked "taken"; J13 in its printed order SCL, SDA, 3V3, GND. The socket stays a table of pad
  labels until its geometry is drawn from the board.
- **What the drawing can't know yet, it asks for**: which J7 pad row is 5V is not clear from
  the board's labels, so a J7 wiring shows "find the 5V row with a meter" until the orientation
  is confirmed on a V1.1 board; IO27/IO28 say "V1.1 boards only"; a CC1101 says
  "match the labels" because breakout pin orders differ.
- **The research's wording, not the old one**: the clock on J13 keeps short leads (10-20 cm,
  away from siren and mains wires) because it shares the touch screen's bus; "never 5V" because
  the module's pull-ups would lift the connector's lines above 3.3 V (today's `boards.json` says
  it would reach the touch bus itself, which the shifters prevent); CC1101 leads 10-15 cm, its
  antenna away from the board, the display ribbon and the Zigbee antenna.
- **Socket pad names disagree.** The V1.0 board photo and the schematic (`J9`/`J11`) show the
  socket's pads printed with the chip's GPIO numbers (IO6, IO7, IO8, IO9, IO10), as the research
  says. `boards.json`, docs/features/25 and docs/features/34 call them IO0, IO1, IO2, IO5, IO4 (the
  ESP32-H2 module's own pin names), and the Flasher offers "Put it back on wireless-module socket
  pad IO4" - a label that on J7 is a different pin (GPIO 4). The prototype uses the printed
  names; check a V1.1 board before changing the shipped text.

### Visual identity: the website's and the docs'

The Flasher adopts the website's tokens (design "At
home") and the docs' accent (the manual's [`src/styles/custom.css`](https://github.com/RoboAlarms/documentation/blob/main/src/styles/custom.css) in RoboAlarms/documentation, `docs/BRAND.md`); the prototype uses
them verbatim.

| Token | Light | Dark | Use |
|---|---|---|---|
| `--bg` / `--surface` | `#f7faf9` / `#ffffff` | `#111719` / `#172023` | window / cards |
| `--soft` / `--surface-raised` | `#eef4f2` / `#edf3f2` | `#151d20` / `#1c282b` | side rail, quiet areas / hover, tracks |
| `--ink` / `--muted` / `--line` | `#142b30` / `#536970` / `#d3dfdd` | `#f3f7f6` / `#a5b5b8` / `#2a373b` | text / secondary text / hairlines |
| `--accent` / `--accent-bg` / `--button-ink` | `#007b7d` / `#e0f3ef` / `#ffffff` | `#94eff0` / `#17393b` / `#093335` | brand teal, selection, primary button text |
| `--amber` | `#865914` | `#f0cf8f` | warnings (with a tinted background added for the app) |
| red (added) | `#b3261e` | `#ff8a80` | errors only; the website has no red |

Type: `'Segoe UI', system-ui, -apple-system, BlinkMacSystemFont, sans-serif` (no web fonts),
headings 650 with tight tracking, the uppercase eyebrow with its 25 px rule. Shapes: 7 px
buttons, 4 px tags, 12 px cards with a hairline and no shadow, the panel drawn in the site's
`.panel-frame` colours (`#070e11`, edge `#34464a`). Icons: Lucide line icons (ISC) at 1.75
stroke, close to the site's 1.4-1.6 line drawings. Theme: follows the OS (FLS-017) with the
site's toggle. **Today's Flasher uses the Panel Manager's older palette**, so adopting this
changes `app.css`'s tokens; `docs/BRAND.md` should gain this table so the website, the docs and
the Flasher cite one place.

### Words

| Say | Don't say (primary path) |
|---|---|
| the panel, this computer | device, host, target |
| Check this panel (it restarts for a moment) | Identify chip, ROM bootloader, download mode |
| RoboAlarms 0.6.0, built for this panel's chip | image, bundle, silicon up to v1.99, dev/legacy profile, project name |
| back up what's on the panel now | read flash, dump |
| Found on COM5 (in details) | COM5 CH340 1A86:7523 (as the headline) |
| Receiver module / CC1101 board / Not sure | `rf module ook` / `cc1101` / `auto` |
| "Not offered: IO25 is the chip's own USB data line" | "(docs/features/19)", "invalid GPIO" |

### Errors, recovery and trust
- An error stays on the screen of the step it belongs to: what happened, what to do, a button
  to try again. Toasts only for things that need no action.
- "Nothing showing up?" is a checklist on the Connect screen: a charge-only cable, another
  port, programs holding the port, the CH340 driver (Windows: a board under "Other devices").
- **Code signing** (Windows, before public release): Azure Artifact Signing costs about $9.99 a
  month and takes individual developers in the USA and Canada; SignPath Foundation signs
  qualifying open-source projects for free; either way SmartScreen reputation builds over a
  few releases (EV certificates no longer skip it). macOS needs notarization when it ships.

## What it needs underneath

**Engine (Python, keeps ADR-039):** a per-device *plan* (which step, what the primary action
is, why it is blocked), computed in Python so it is unit-tested like the rest; the parts
choices held per board and applied as a last job step after the boot check; last choices
remembered per board MAC; time-left estimates from measured throughput; persistent errors in
state instead of one-shot replies; a native file dialog through pywebview.

**Firmware:** an `about` / `hw describe` answer on every board (already in docs/features/34's Phase 2
as `about`) carrying the parts, their settings, the pins in use and the allowed pins with
reasons from the board description and its validator (BRD-001..013), so the Flasher draws the
panel's own truth instead of a hand-kept copy; the same data written into bundles at build
time for boards that don't run RoboAlarms yet; optionally `rf module off` for an explicit
"None". A generic `hw set` only when more parts become settable (outputs, docs/features/16). The
description should also carry each connector as printed (J7's rows, J13's order, the socket's
pad labels), each part's places with whether this firmware supports them, and the rules between
parts (the Zigbee module takes the socket), so the Flasher's drawings, "Works with" tags and pin
check come from the panel. The placements themselves are the firmware work in
AddOnWiringResearch.md: the clock on its own I2C bus (IO27/IO28, controller 1); a CC1101 on J7
taken out of the terminal list and chosen at runtime like `rf pin`; the 433 MHz search kept off
the socket when a Zigbee module is fitted.

## Plan

| Phase | Scope | Firmware change |
|---|---|---|
| A. The new face | Website/docs tokens and components in `app.css`; the shell (app bar, step list, footer); Connect, Check, Install/Update, progress, Test and finish; Advanced tools holding today's tabs; inline errors; the words table; the engine's plan and estimates; mock scenarios and screenshots in `docs/features/34/screens/` | None |
| B. Connected parts | The parts step as prototyped: places with "Works with" / "Planned", suggested layouts, J7 and J13 drawings and the pin picker from `boards.json`, the live pin check and wired zones left, apply after install, part tests on the last screen; `boards.json` gains J13's printed order and the socket's printed pad names | Optional `rf module off` |
| C. The panel describes itself | `about` / `hw describe` on every board (connectors as printed, places and their support, rules), bundles carry the description, the Flasher stops hand-copying pins; the placements AddOnWiringResearch.md lists (clock on IO27/IO28, CC1101 on J7, the search kept off a fitted Zigbee module) | Yes |
| D. Trust and reach | Code signing, CH340 driver detection, "Update available" from releases, macOS and Linux builds | None |

## How to check it works

With `--mock` and the prototype first, then the new page: 3-5 people who have never flashed a
board, one at a time, thinking aloud. Tasks: install on a new panel; set up a receiver whose
DATA wire is on IO3 (from a photo of the wiring); update a panel that already runs RoboAlarms;
find the console when asked. It works when each finishes the install unaided, nobody picks a
wrong pin or asks what a word means on the primary path, and a developer can still do all of
today's tasks from Advanced tools.

## Decisions to take

1. The direction: guided setup by default, Advanced tools for everything else?
2. Apply the chosen parts automatically after the install (one more restart), or only when asked?
3. Keep the explicit "Check this panel" before restarting an unknown board (recommended: a CH340
   can be anything, a 3D printer included), or identify automatically when the bridge is a
   known ESP bridge?
4. The Panel Manager uses the same older palette as today's Flasher: move it to the
   website/docs tokens too, later?
5. Code signing: which route, and before which release?
6. Pictures: drawn SVG in the brand colours (recommended: consistent in both themes, no
   copyright questions) or photographs of the real boards?
7. On a V1.1 board: what is printed beside the radio socket's pads (IO6-IO10 as on the V1.0
   photo, or IO0-IO5)? And which J7 pad row reads 5 V? The first decides the shipped wiring text,
   the second lets the J7 drawing show the real orientation and drop the meter step.

Feedback, 2026-09-24, after the first prototype: *"UI looks good"*, with the add-on wiring research
to shape the wiring; questions 2-7 are still open.

## Sources

- Raspberry Pi: [A new Raspberry Pi Imager](https://www.raspberrypi.com/news/a-new-raspberry-pi-imager/);
  [OMG! Ubuntu: Imager 2.0 released](https://www.omgubuntu.co.uk/2025/11/raspberry-pi-imager-2-0-released);
  [CNX Software: Imager 2.0](https://www.cnx-software.com/2025/11/24/raspberry-pi-imager-2-0-released-with-a-revamped-user-interface-raspberry-pi-connect-support/)
- [ESP Web Tools](https://esphome.github.io/esp-web-tools/);
  [Kiril Peyanski: ESP Web Tools and Improv explained](https://peyanski.com/esp-web-tools-and-improv-explained/)
- [Meshtastic Web Flasher](https://flasher.meshtastic.org/) (observed 2026-09-24);
  [meshtastic/web-flasher](https://github.com/meshtastic/web-flasher)
- [Oscar Liang: flashing and setting up ExpressLRS](https://oscarliang.com/setup-expresslrs-2-4ghz/)
- [Tasmota: Templates](https://tasmota.github.io/docs/Templates/)
- [WLED: Pin Info](https://kno.wled.ge/features/pin-info/);
  [wled/WLED#4070](https://github.com/wled/WLED/issues/4070)
- [TI SysConfig deep dive (Hubble Network)](https://hubble.com/community/guides/ti-sysconfig-deep-dive-generating-pin-and-peripheral-configurations/)
- NN/g: [Wizards](https://www.nngroup.com/articles/wizards/);
  [Progressive disclosure](https://www.nngroup.com/articles/progressive-disclosure/)
- Microsoft Learn: [Code signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options);
  [SmartScreen reputation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)
- Project: the manual's [`src/styles/custom.css`](https://github.com/RoboAlarms/documentation/blob/main/src/styles/custom.css) in RoboAlarms/documentation;
  `docs/BRAND.md`; `components/boards/board_crowpanel_p4/include/board_pins.h`;
  `tools/flasher/roboflash/boards.json`
