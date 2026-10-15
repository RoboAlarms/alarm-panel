# RoboAlarms Flasher

A desktop program that puts the panel's firmware on a board and then tests what is wired to it
(docs/features/34). It lists the USB serial ports that are plugged in, works out which board is on
the selected one, picks the image for it (including the P4's v1 or v3 silicon build), backs up
the factory flash, installs, updates or resets to factory defaults, sets the panel up for the
parts wired to it (a 433 MHz receiver, a clock module, a Z-Wave radio, on the socket or the J7
header), and then
tests them (docs/features/34 FLS-023..025). Windows first; macOS and Linux from the same code.

## For the person flashing a panel

The Flasher walks you through five steps; the list on the left shows where you are.

1. **Connect.** Double-click **RoboAlarms Flasher** (Windows may say it "protected your PC" the
   first time: *More info*, then *Run anyway* - the program is not code-signed yet). On the
   panel, slide **POWER** to ON and plug the USB-C port marked **UART0** into the computer. Also
   plug the port marked **USB2.0** into a phone charger (5 V, 2 A or more): with its screen on
   the panel uses 8-10 W, more than a computer's USB port gives, and the screen can go black
   (Elecrow's advice). A panel with its own 5 V supply keeps it connected: each power input has
   its own diode, so none of it reaches the computer. The panel appears within a second or two;
   nothing showing up? The screen lists what to try (the POWER switch, the UART0 port, another
   cable or port, the CH340 driver); on Windows it says so when a CH340 is plugged in but has no
   COM port because its driver is missing or failed to start).
2. **Check the panel.** A new panel still runs its maker's demo: press **Check this panel** (it
   restarts for a moment to read its chip; nothing changes). A panel already running RoboAlarms
   answers by itself.
3. **Connected parts.** Pick what is wired to the panel - a suggested layout, or each part: the
   433 MHz receiver (a receiver module or a CC1101, on the radio socket or the J7 header), the
   battery-backed clock (on the I2C connector or J7 IO27/IO28), the Zigbee module and the Z-Wave
   radio (a Zooz ZAC93 on the radio socket's serial line or J7 IO27/IO28). The
   Flasher draws every wire; check the J7 header's 5V row with a meter before wiring it, and
   power parts from **3V3, never 5V**. The pin check says what to fix and how many wired zones
   are left. Nothing extra? **Skip for now**.
4. **Install** (or **Update** a panel running RoboAlarms - its codes, zones and Wi-Fi stay).
   The Flasher downloads the newest RoboAlarms for the panel by itself, so the computer needs the
   internet the first time; what it downloaded is kept for the next panel.
   Leave **Back up the panel first** ticked on a new panel: the backup is the only way back to
   the maker's demo. Every step shows as it happens, then the panel is set up for its parts
   with one more restart. **Reset to factory defaults** is here too: it erases the whole panel
   and installs RoboAlarms fresh (a complete reflash) - tick "Erase everything" first.
5. **Test and finish.** The panel runs its first-start setup on the touchscreen: join Wi-Fi,
   then change the factory codes (installer 69420, master 123456) when it asks. Then test the
   parts here: **Start listening** and press a remote or open a sensor - each one appears in the
   list (to use one as a zone: on the panel, Zones > Add a zone > 433 MHz sensor); the clock
   card compares the panel's clock with this computer and can set it.

**Advanced tools** (top right) hold everything else: every port, the console, the hardware of a
running panel (change the parts, apply and restart), backups and restore, the software (the
version - the steadiest release unless you pick another - each board's download, a firmware file
instead, the build choice), and the logs.

Backups go to `Documents\RoboAlarms\Backups` (keep them: the only way back to the maker's
demo), recordings to `Documents\RoboAlarms\Captures` (only a whole recording is saved), the Flasher's log to
`%LOCALAPPDATA%\RoboAlarms\Flasher\logs` (Advanced tools > Logs) and its downloads to `...\Flasher\cache`.

Nothing the Flasher does can burn eFuses, turn on secure boot or flash encryption, or touch the
C6 (tools/tests/test_flasher_safety.py). It never restarts a panel that is armed, in installer
mode or checking an update, and it never types keypad codes.

## For developers

Run it from the repo (ESP-IDF's Python has esptool and pyserial; without pywebview it opens a
browser tab):

```powershell
$py = "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe"
& $py tools/flasher/run.py                                               # firmware from the firmware server
& $py tools/flasher/run.py --server http://localhost:8092                # another server (a test one here)
& $py tools/flasher/run.py --build firmware/crowpanel_p4/build_v1_dev   # images straight from a build
& $py tools/flasher/run.py --mock                                        # pretend boards and server, no hardware
& $py tools/flasher/run.py ports                                         # the serial ports
& $py tools/flasher/run.py probe COM4                                    # look without restarting
```

**The firmware server** (docs/features/34 FLS-032 to FLS-034): the
Flasher carries no firmware. It asks `https://firmware.roboalarms.com` (or `--server`, or the
`ROBOALARMS_FIRMWARE_SERVER` variable; plain HTTP only for a server on this computer) for each
board's steadiest release (Stable, else Beta, else Alpha; a development build only when you choose
it, in the version list at the bottom left or Advanced tools > Software, FLS-033), downloads the bundle with the languages,
voices and asset packs made for that version, and checks every image and pack against the release
key built into it (`fwserver.TRUSTED_KEYS`): a release signed with another key is refused before
anything is downloaded. A new panel gets English with its US English voice, or the language chosen
with its own voice, and every asset pack (the screensaver's picture among them). Newer board
descriptions come from the server as a signed asset pack (`asset_pack.py build --kind boards`,
docs/features/39 asset-packs.md), and a newer Flasher is only pointed at, never installed.

**Make a firmware bundle** (a `.roboflash` file: the four parts `idf.py flash` writes, for each
board and silicon build, with checksums). The images must be signed with the owner's key
(`secrets/ota_signing_key.pem`), or a panel flashed with them would refuse the owner's network
updates; dark builds are refused.

```powershell
pwsh tools/idf.ps1 -Profile dev build
pwsh tools/idf.ps1 -Profile dev -ChipRev v3 build
pwsh tools/idf.ps1 -Project crowpanel_10 -Profile dev build
& $py tools/flasher/run.py bundle --build firmware/crowpanel_p4/build_v1_dev --build firmware/crowpanel_p4/build_v3_dev --build firmware/crowpanel_10/build_v1_dev
```

The asset packs come from the first build; every other build's must hold the same contents (the 7" and
10.1" share them). `--voice build/voice/<name>.robovoice` (repeatable) adds a signed voice pack: a new
panel gets the one for its language (else US English), and an update replaces a panel's voice only
with a newer revision of the same voice (FLS-031). It lands in `build/flasher/`: upload it to the firmware server, or open it in
the Flasher with Advanced tools > Software > *Open firmware…* (or drop it on the window, or
`run.py --bundle <file>`).

**Build the program** (a build environment of its own in `build/flasher-venv`; the Flasher's
tests run first):

```powershell
pwsh tools/flasher/build.ps1                                   # build/flasher/dist/RoboAlarms Flasher.exe
```

macOS and Linux: `tools/flasher/build.sh` (keep the two in step). No firmware goes inside.

## How it is built

`roboflash/` is the engine and the page: `ports` (the port list), `probe` (asks `dev describe`
without a reset), `chip` (esptool through an allow-list), `boards` + `boards.json` (detection
and what may be chosen by hand), `bundle`/`make_bundle`, `fwserver` (the firmware server:
downloads, the cache, channels, what is trusted), `jobs` (install, update, backup,
restore, and their gates), `console`, `rf_test` (the `rf` lines), `parts` (connected parts: the rules, the console commands, what a
running panel reports), `controller` (state and
actions), `server` (127.0.0.1, a per-run token, a Host check) and `web/` (the page). `mock`,
`fakechip` and `testkit` are the pretend boards and server the tests and `--mock` use. Tests: `tools/tests/test_flasher_*.py`.
