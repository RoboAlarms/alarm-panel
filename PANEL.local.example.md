# This panel

Copy this file to `PANEL.local.md` (git-ignored) and fill it in for your own board (a section per
board if you have several). AGENTS.md tells an AI assistant to read it first; without it, it will
ask you for the port and the silicon revision instead of assuming someone else's. Keep facts here
and one "Runs now" line per board, updated when you flash - not an install log.

## The board
- Board and P4 silicon revision (the boot log prints it, e.g. `chip revision: v1.3`) -> which
  build to flash: `pwsh tools/idf.ps1 -p <PORT> flash` for v1.x,
  `pwsh tools/idf.ps1 -ChipRev v3 -p <PORT> flash` for v3.x.
- Serial port (`<PORT>`, e.g. COM7 or /dev/ttyACM0).
- Where the factory flash backup is (see CONTRIBUTING.md, "Set up a panel"). Without one there is no
  way back to the vendor demo.
- Is this panel in real use, or a bench board? A panel that guards a house is only flashed
  while disarmed, and only when you have just said so.
- Runs now: the firmware version and profile, when and how it was flashed.

## Codes
- Still the factory codes (installer 69420, master 123456), or changed? If changed, say so:
  nobody should guess codes at the console - 5 wrong ones lock the keypad and each is logged.

## The owner
- Anything an assistant must know about the people using this panel: accessibility needs,
  who else lives with it, what must never be changed without asking.
- Note that the display-safety rules in AGENTS.md apply to every panel regardless.

## Keep safe (neither is in git)
- `secrets/ota_signing_key.pem` - every future update of this panel must be signed with it.
  Lose it and the panel can only be updated over USB again.
- The factory flash backup.
